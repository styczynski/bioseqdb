/**
 * @file
 * @brief Stores backend-local providers and publishes the cache API to other extensions.
 *
 * Provider descriptions and names are copied into fixed slots. Removal leaves
 * the slot unused for the rest of the process. Registration tokens and active
 * reference counts prevent  removal by the wrong caller or during execution.
 */
module;

extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/errcodes.h"
#include "utils/plancache.h"
}

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

module bioseq.cache.pg.provider;

import :registry;
import bioseq.cache.provider_api;

namespace bioseq::cache::pg::provider_registry::detail {

enum class provider_slot_state : std::uint32_t {
    vacant = 0,
    active = 1,
    tombstone = 2,
};

struct provider_slot {
    provider_slot_state state = provider_slot_state::vacant;
    std::uint32_t provider_id = 0;
    std::uint64_t registration_generation = 0;
    std::uint64_t owner_cookie = 0;
    std::uint32_t active_callbacks = 0;
    std::uint32_t active_leases = 0;
    provider provider_descriptor{};
    char provider_name[maximum_provider_name_bytes]{};
};

static_assert(std::is_standard_layout_v<provider_slot>);
static_assert(std::is_trivially_copyable_v<provider_slot>);

inline provider_slot g_provider_slots[maximum_registered_providers]{};
inline std::size_t g_next_provider_slot = 0;
inline std::size_t g_active_provider_count = 0;
inline std::uint64_t g_registry_generation = 1;
inline std::uint64_t g_registry_cookie = 0;
inline std::uint32_t g_active_callback_count = 0;
inline relation_write_observer g_relation_write_observer = nullptr;
inline planner_bridge_observer g_planner_bridge_observer = nullptr;
inline finalized_plan_bridge_observer
    g_finalized_plan_bridge_observer = nullptr;
inline const provider_directory* g_preflight_directory = nullptr;
inline bool g_preflight_directory_compatible = false;
inline void** g_cache_api_rendezvous_slot = nullptr;
inline void** g_provider_directory_rendezvous_slot = nullptr;

inline constexpr std::uint32_t k_known_provider_flags =
    static_cast<std::uint32_t>(
        bioseq_cache_provider_exact_source_elision_safe)
    | static_cast<std::uint32_t>(bioseq_cache_provider_reusable_coverage)
    | static_cast<std::uint32_t>(
        bioseq_cache_provider_complete_write_observation);

[[nodiscard]] constexpr std::uint64_t mix_cookie(
        std::uint64_t value) noexcept {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    return value == 0 ? 0x4253514341434845ULL : value;
}

[[nodiscard]] std::uint64_t process_cookie() noexcept {
    if (g_registry_cookie == 0) {
        const std::uint64_t address = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(&g_provider_slots));
        const std::uint64_t pid =
            static_cast<std::uint64_t>(static_cast<std::uint32_t>(MyProcPid));
        g_registry_cookie = mix_cookie(
            address ^ (pid << 32U) ^ 0x76325f7265676973ULL);
    }
    return g_registry_cookie;
}

[[nodiscard]] std::uint64_t next_generation() noexcept {
    ++g_registry_generation;
    if (g_registry_generation == 0) {
        g_registry_generation = 1;
    }
    return g_registry_generation;
}

[[nodiscard]] bool provider_name_valid(const char* name) noexcept {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    bool contains_non_space = false;
    for (std::size_t index = 0; index < maximum_provider_name_bytes;
         ++index) {
        const unsigned char value = static_cast<unsigned char>(name[index]);
        if (value == 0) {
            return contains_non_space;
        }
        if (value < 0x20U || value == 0x7fU) {
            return false;
        }
        contains_non_space = contains_non_space || value != 0x20U;
    }
    return false;
}

[[nodiscard]] bool provider_names_equal(
        const char* left, const char* right) noexcept {
    if (left == nullptr || right == nullptr) {
        return false;
    }
    for (std::size_t index = 0; index < maximum_provider_name_bytes;
         ++index) {
        if (left[index] != right[index]) {
            return false;
        }
        if (left[index] == '\0') {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool provider_tables_equal(
        const provider& left, const provider& right) noexcept {
    return left.abi_revision == right.abi_revision
        && left.postgres_major == right.postgres_major
        && left.provider_id == right.provider_id
        && left.provider_flags == right.provider_flags
        && left.reserved == right.reserved
        && provider_names_equal(left.provider_name, right.provider_name)
        && left.provider_scan_methods == right.provider_scan_methods
        && left.analyze_query == right.analyze_query
        && left.describe_path == right.describe_path
        && left.append_domain_semantic_digest
            == right.append_domain_semantic_digest
        && left.append_generation_semantic_digest
            == right.append_generation_semantic_digest
        && left.append_runtime_generation_semantic_digest
            == right.append_runtime_generation_semantic_digest
        && left.map_reusable_source == right.map_reusable_source
        && left.decorate_path == right.decorate_path
        && left.build_plan_digest_recipe == right.build_plan_digest_recipe
        && left.extract_occurrence == right.extract_occurrence
        && left.late_fetch_batch == right.late_fetch_batch
        && left.explain_domain == right.explain_domain;
}

void capture_provider(
        provider_slot& destination,
        const provider& source,
        std::size_t slot_index) noexcept {
    destination = {};
    destination.state = provider_slot_state::active;
    destination.provider_id = source.provider_id;
    destination.registration_generation = next_generation();
    destination.owner_cookie = mix_cookie(
        process_cookie()
        ^ destination.registration_generation
        ^ (static_cast<std::uint64_t>(source.provider_id) << 17U)
        ^ static_cast<std::uint64_t>(slot_index));
    destination.provider_descriptor = source;
    destination.provider_descriptor.struct_size =
        static_cast<std::uint32_t>(sizeof(provider));
    for (std::size_t index = 0; index < maximum_provider_name_bytes;
         ++index) {
        destination.provider_name[index] = source.provider_name[index];
        if (source.provider_name[index] == '\0') {
            break;
        }
    }
    destination.provider_descriptor.provider_name = destination.provider_name;
}

[[nodiscard]] bool baseline_callbacks_present(
        const provider& provider_descriptor) noexcept {
    return provider_descriptor.provider_scan_methods != nullptr
        && provider_descriptor.analyze_query != nullptr
        && provider_descriptor.describe_path != nullptr
        && provider_descriptor.append_domain_semantic_digest != nullptr
        && provider_descriptor.append_generation_semantic_digest != nullptr
        && provider_descriptor.append_runtime_generation_semantic_digest
            != nullptr
        && provider_descriptor.build_plan_digest_recipe != nullptr;
}

[[nodiscard]] bool reusable_callbacks_present(
        const provider& provider_descriptor) noexcept {
    return provider_descriptor.map_reusable_source != nullptr
        && provider_descriptor.decorate_path != nullptr
        && provider_descriptor.extract_occurrence != nullptr
        && provider_descriptor.late_fetch_batch != nullptr;
}

[[nodiscard]] std::size_t provider_slot_for_id(
        std::uint32_t provider_id) noexcept {
    if (provider_id == 0) {
        return invalid_provider_slot;
    }
    for (std::size_t index = 0; index < g_next_provider_slot; ++index) {
        const provider_slot& slot = g_provider_slots[index];
        if (slot.state == provider_slot_state::active
            && slot.provider_id == provider_id) {
            return index;
        }
    }
    return invalid_provider_slot;
}

[[nodiscard]] std::size_t provider_slot_for_pointer(
        const provider* provider_descriptor) noexcept {
    if (provider_descriptor == nullptr) {
        return invalid_provider_slot;
    }
    for (std::size_t index = 0; index < g_next_provider_slot; ++index) {
        const provider_slot& slot = g_provider_slots[index];
        if (slot.state == provider_slot_state::active
            && &slot.provider_descriptor == provider_descriptor) {
            return index;
        }
    }
    return invalid_provider_slot;
}

[[nodiscard]] bool registration_token_valid(
        const provider_registration* registration,
        std::size_t* slot_index) noexcept {
    if (slot_index != nullptr) {
        *slot_index = invalid_provider_slot;
    }
    if (registration == nullptr
        || registration->struct_size
            < offsetof(provider_registration, owner_cookie)
                + sizeof(registration->owner_cookie)
        || registration->provider_id == 0
        || registration->registry_generation == 0
        || registration->owner_cookie == 0) {
        return false;
    }
    const std::size_t index =
        provider_slot_for_id(registration->provider_id);
    if (index == invalid_provider_slot) {
        return false;
    }
    const provider_slot& slot = g_provider_slots[index];
    if (slot.registration_generation
            != registration->registry_generation
        || slot.owner_cookie != registration->owner_cookie) {
        return false;
    }
    if (slot_index != nullptr) {
        *slot_index = index;
    }
    return true;
}

[[nodiscard]] bool physical_identity_valid(
        const physical_identity* identity) noexcept {
    if (identity == nullptr
        || identity->struct_size
            < offsetof(physical_identity, bytes)
                + sizeof(identity->bytes)
        || identity->reserved != 0
        || identity->length > sizeof(identity->bytes)
        || identity->kind > 2U) {
        return false;
    }
    if (identity->kind == 0U) {
        return identity->length == 0U;
    }
    return identity->length != 0U;
}

[[nodiscard]] bool directory_valid(
        const provider_directory* directory) noexcept {
    return directory != nullptr
        && directory->struct_size
            >= offsetof(provider_directory, bind_cache_api)
                + sizeof(directory->bind_cache_api)
        && directory->abi_revision == bioseq::cache::k_provider_abi_revision
        && directory->postgres_major == k_postgres_major_version
        && directory->reserved == 0
        && directory->instance_cookie != 0
        && directory->bind_cache_api != nullptr;
}

[[nodiscard]] bioseq_cache_status api_register_provider(
        const provider* provider_descriptor,
        provider_registration* registration) noexcept {
    return register_provider(provider_descriptor, registration);
}

[[nodiscard]] bool api_unregister_provider(
        const provider_registration* registration) noexcept {
    return unregister_provider(registration);
}

void api_record_relation_write(
        std::uint32_t database_oid,
        std::uint32_t relation_oid,
        const physical_identity* identity) noexcept {
    record_relation_write(database_oid, relation_oid, identity);
}

void bridge_after_provider_rel_paths(
        PlannerInfo* root,
        RelOptInfo* rel,
        unsigned int rti,
        RangeTblEntry* rte,
        const provider* provider_descriptor) noexcept {
    after_provider_rel_paths(root, rel, rti, rte, provider_descriptor);
}

void bridge_after_planned_statement(PlannedStmt* statement) noexcept {
    after_planned_statement(statement);
}

inline constinit bioseq::cache::bioseq_cache_planner_bridge g_planner_bridge{
    static_cast<std::uint32_t>(
        sizeof(bioseq::cache::bioseq_cache_planner_bridge)),
    bioseq::cache::k_provider_abi_revision,
    k_postgres_major_version,
    &bridge_after_provider_rel_paths,
    &bridge_after_planned_statement,
};

inline bioseq::cache::bioseq_cache_api g_cache_api{
    .struct_size = static_cast<std::uint32_t>(
        sizeof(bioseq::cache::bioseq_cache_api)),
    .abi_revision = bioseq::cache::k_provider_abi_revision,
    .postgres_major = k_postgres_major_version,
    .reserved = 0,
    .instance_cookie = 0,
    .register_provider = &api_register_provider,
    .unregister_provider = &api_unregister_provider,
    .record_relation_write = &api_record_relation_write,
    .planner_bridge = &g_planner_bridge,
};

} // namespace bioseq::cache::pg::provider_registry::detail

namespace bioseq::cache::pg::provider_registry {

bool validate_provider(const provider* provider_descriptor) noexcept {
    if (provider_descriptor == nullptr
        || provider_descriptor->struct_size
            < offsetof(provider, explain_domain)
                + sizeof(provider_descriptor->explain_domain)
        || provider_descriptor->abi_revision
            != bioseq::cache::k_provider_abi_revision
        || provider_descriptor->postgres_major != k_postgres_major_version
        || provider_descriptor->provider_id == 0
        || provider_descriptor->reserved != 0
        || (provider_descriptor->provider_flags
            & ~detail::k_known_provider_flags) != 0
        || !detail::provider_name_valid(provider_descriptor->provider_name)
        || !detail::baseline_callbacks_present(*provider_descriptor)) {
        return false;
    }
    const bool reusable =
        (provider_descriptor->provider_flags
         & static_cast<std::uint32_t>(
             bioseq_cache_provider_reusable_coverage)) != 0;
    return !reusable
        || detail::reusable_callbacks_present(*provider_descriptor);
}

bioseq_cache_status register_provider(
        const provider* provider_descriptor,
        provider_registration* registration) noexcept {
    if (registration == nullptr
        || registration->struct_size
            < offsetof(provider_registration, owner_cookie)
                + sizeof(registration->owner_cookie)
        || !validate_provider(provider_descriptor)
        || detail::g_active_callback_count != 0) {
        return bioseq_cache_status_error;
    }

    const std::size_t existing =
        detail::provider_slot_for_id(provider_descriptor->provider_id);
    if (existing != invalid_provider_slot) {
        const detail::provider_slot& slot =
            detail::g_provider_slots[existing];
        if (!detail::provider_tables_equal(
                slot.provider_descriptor, *provider_descriptor)) {
            return bioseq_cache_status_error;
        }
        *registration = provider_registration{
            .struct_size =
                static_cast<std::uint32_t>(sizeof(provider_registration)),
            .provider_id = slot.provider_id,
            .registry_generation = slot.registration_generation,
            .owner_cookie = slot.owner_cookie,
        };
        return bioseq_cache_status_ok;
    }

    for (std::size_t index = 0;
         index < detail::g_next_provider_slot;
         ++index) {
        const detail::provider_slot& slot = detail::g_provider_slots[index];
        if (slot.state != detail::provider_slot_state::active) {
            continue;
        }
        if (slot.provider_descriptor.provider_scan_methods
                == provider_descriptor->provider_scan_methods
            || detail::provider_names_equal(
                slot.provider_descriptor.provider_name,
                provider_descriptor->provider_name)) {
            return bioseq_cache_status_error;
        }
    }
    if (detail::g_next_provider_slot == maximum_registered_providers) {
        return bioseq_cache_status_error;
    }

    const std::size_t slot_index = detail::g_next_provider_slot++;
    detail::provider_slot& slot = detail::g_provider_slots[slot_index];
    detail::capture_provider(slot, *provider_descriptor, slot_index);
    ++detail::g_active_provider_count;
    *registration = provider_registration{
        .struct_size =
            static_cast<std::uint32_t>(sizeof(provider_registration)),
        .provider_id = slot.provider_id,
        .registry_generation = slot.registration_generation,
        .owner_cookie = slot.owner_cookie,
    };
    ResetPlanCache();
    return bioseq_cache_status_ok;
}

bool unregister_provider(
        const provider_registration* registration) noexcept {
    if (detail::g_active_callback_count != 0) {
        return false;
    }
    std::size_t slot_index = invalid_provider_slot;
    if (!detail::registration_token_valid(registration, &slot_index)) {
        return false;
    }
    detail::provider_slot& slot = detail::g_provider_slots[slot_index];
    if (slot.active_callbacks != 0 || slot.active_leases != 0) {
        return false;
    }
    slot.state = detail::provider_slot_state::tombstone;
    slot.provider_descriptor = {};
    slot.provider_name[0] = '\0';
    slot.provider_id = 0;
    slot.owner_cookie = 0;
    slot.registration_generation = 0;
    if (detail::g_active_provider_count != 0) {
        --detail::g_active_provider_count;
    }
    static_cast<void>(detail::next_generation());
    ResetPlanCache();
    return true;
}

const provider* find_provider(std::uint32_t provider_id) noexcept {
    const std::size_t slot_index =
        detail::provider_slot_for_id(provider_id);
    return slot_index == invalid_provider_slot
        ? nullptr
        : &detail::g_provider_slots[slot_index].provider_descriptor;
}

provider_snapshot snapshot_providers() noexcept {
    provider_snapshot snapshot{};
    snapshot.generation = detail::g_registry_generation;
    for (std::size_t index = 0;
         index < detail::g_next_provider_slot;
         ++index) {
        const detail::provider_slot& slot = detail::g_provider_slots[index];
        if (slot.state != detail::provider_slot_state::active) {
            continue;
        }
        snapshot.providers[snapshot.provider_count++] =
            &slot.provider_descriptor;
    }
    return snapshot;
}

bool acquire_provider_lease(
        const provider* provider_descriptor,
        provider_lease* lease) noexcept {
    if (lease == nullptr) {
        return false;
    }
    *lease = {};
    const std::size_t slot_index =
        detail::provider_slot_for_pointer(provider_descriptor);
    if (slot_index == invalid_provider_slot) {
        return false;
    }
    detail::provider_slot& slot = detail::g_provider_slots[slot_index];
    if (slot.active_leases
        == (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    ++slot.active_leases;
    *lease = provider_lease{
        .slot_index = slot_index,
        .registry_generation = slot.registration_generation,
        .provider_id = slot.provider_id,
        .owner_cookie = slot.owner_cookie,
    };
    return true;
}

const provider* provider_from_lease(
        const provider_lease& lease) noexcept {
    if (lease.slot_index >= detail::g_next_provider_slot
        || lease.slot_index == invalid_provider_slot) {
        return nullptr;
    }
    const detail::provider_slot& slot =
        detail::g_provider_slots[lease.slot_index];
    if (slot.state != detail::provider_slot_state::active
        || slot.provider_id != lease.provider_id
        || slot.registration_generation != lease.registry_generation
        || slot.owner_cookie != lease.owner_cookie
        || slot.active_leases == 0) {
        return nullptr;
    }
    return &slot.provider_descriptor;
}

bool validate_provider_lease(
        const provider_lease& lease,
        const provider* expected_provider) noexcept {
    const provider* leased_provider = provider_from_lease(lease);
    return leased_provider != nullptr
        && (expected_provider == nullptr
            || leased_provider == expected_provider);
}

void release_provider_lease(provider_lease* lease) noexcept {
    if (lease == nullptr) {
        return;
    }
    if (lease->slot_index < detail::g_next_provider_slot
        && lease->slot_index != invalid_provider_slot) {
        detail::provider_slot& slot =
            detail::g_provider_slots[lease->slot_index];
        if (slot.state == detail::provider_slot_state::active
            && slot.provider_id == lease->provider_id
            && slot.registration_generation == lease->registry_generation
            && slot.owner_cookie == lease->owner_cookie
            && slot.active_leases != 0) {
            --slot.active_leases;
        }
    }
    *lease = {};
}

bool begin_provider_callback(
        const provider* provider_descriptor,
        const provider_lease* lease,
        provider_callback_token* token) noexcept {
    if (token == nullptr) {
        return false;
    }
    *token = {};
    std::size_t slot_index = invalid_provider_slot;
    if (lease != nullptr) {
        if (!validate_provider_lease(*lease, provider_descriptor)) {
            return false;
        }
        slot_index = lease->slot_index;
    } else {
        slot_index = detail::provider_slot_for_pointer(provider_descriptor);
    }
    if (slot_index == invalid_provider_slot) {
        return false;
    }
    detail::provider_slot& slot = detail::g_provider_slots[slot_index];
    if (slot.active_callbacks
            == (std::numeric_limits<std::uint32_t>::max)()
        || detail::g_active_callback_count
            == (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    ++slot.active_callbacks;
    ++detail::g_active_callback_count;
    *token = provider_callback_token{
        .slot_index = slot_index,
        .registry_generation = slot.registration_generation,
        .owner_cookie = slot.owner_cookie,
    };
    return true;
}

void end_provider_callback(provider_callback_token* token) noexcept {
    if (token == nullptr) {
        return;
    }
    if (token->slot_index < detail::g_next_provider_slot
        && token->slot_index != invalid_provider_slot) {
        detail::provider_slot& slot =
            detail::g_provider_slots[token->slot_index];
        if (slot.state == detail::provider_slot_state::active
            && slot.registration_generation == token->registry_generation
            && slot.owner_cookie == token->owner_cookie
            && slot.active_callbacks != 0
            && detail::g_active_callback_count != 0) {
            --slot.active_callbacks;
            --detail::g_active_callback_count;
        }
    }
    *token = {};
}

void install_relation_write_observer(
        relation_write_observer observer) noexcept {
    detail::g_relation_write_observer = observer;
}

void record_relation_write(
        std::uint32_t database_oid,
        std::uint32_t relation_oid,
        const physical_identity* identity) noexcept {
    const relation_write_observer observer =
        detail::g_relation_write_observer;
    if (observer == nullptr) {
        return;
    }
    const physical_identity* validated_identity =
        detail::physical_identity_valid(identity)
        ? identity
        : nullptr;
    observer(database_oid, relation_oid, validated_identity);
}

void install_planner_bridge_observer(
        planner_bridge_observer observer) noexcept {
    detail::g_planner_bridge_observer = observer;
}

void after_provider_rel_paths(
        PlannerInfo* root,
        RelOptInfo* rel,
        unsigned int rti,
        RangeTblEntry* rte,
        const provider* provider_descriptor) noexcept {
    const planner_bridge_observer observer =
        detail::g_planner_bridge_observer;
    if (observer == nullptr || root == nullptr || rel == nullptr
        || rti == 0 || rte == nullptr
        || !validate_provider(provider_descriptor)) {
        return;
    }
    const provider* const registered =
        find_provider(provider_descriptor->provider_id);
    if (registered == nullptr
        || !detail::provider_tables_equal(
            *registered, *provider_descriptor)) {
        return;
    }
    observer(root, rel, rti, rte, registered);
}

void install_finalized_plan_bridge_observer(
        finalized_plan_bridge_observer observer) noexcept {
    detail::g_finalized_plan_bridge_observer = observer;
}

void after_planned_statement(PlannedStmt* statement) noexcept {
    const finalized_plan_bridge_observer observer =
        detail::g_finalized_plan_bridge_observer;
    if (observer != nullptr && statement != nullptr) {
        observer(statement);
    }
}

bool preflight_provider_registry() noexcept {
    detail::g_cache_api_rendezvous_slot = find_rendezvous_variable(
        bioseq::cache::k_cache_api_rendezvous_name);
    if (detail::g_cache_api_rendezvous_slot == nullptr
        || (*detail::g_cache_api_rendezvous_slot != nullptr
            && *detail::g_cache_api_rendezvous_slot
                != static_cast<void*>(&detail::g_cache_api))) {
        return false;
    }
    detail::g_provider_directory_rendezvous_slot =
        find_rendezvous_variable(
            bioseq::cache::k_provider_directory_rendezvous_name);
    detail::g_preflight_directory =
        detail::g_provider_directory_rendezvous_slot == nullptr
            || *detail::g_provider_directory_rendezvous_slot == nullptr
        ? nullptr
        : static_cast<const provider_directory*>(
            *detail::g_provider_directory_rendezvous_slot);
    detail::g_preflight_directory_compatible =
        detail::g_preflight_directory != nullptr
        && detail::directory_valid(detail::g_preflight_directory);
    if (detail::g_cache_api.instance_cookie == 0) {
        detail::g_cache_api.instance_cookie =
            detail::mix_cookie(
                detail::process_cookie() ^ 0x76325f6361636865ULL);
    }
    return true;
}

bool initialize_provider_registry() noexcept {
    if (detail::g_cache_api_rendezvous_slot == nullptr
        || (*detail::g_cache_api_rendezvous_slot != nullptr
            && *detail::g_cache_api_rendezvous_slot
                != static_cast<void*>(&detail::g_cache_api))) {
        return false;
    }
    *detail::g_cache_api_rendezvous_slot =
        static_cast<void*>(&detail::g_cache_api);

    if (detail::g_provider_directory_rendezvous_slot == nullptr
        || *detail::g_provider_directory_rendezvous_slot == nullptr) {
        return true;
    }
    const auto* directory =
        static_cast<const provider_directory*>(
            *detail::g_provider_directory_rendezvous_slot);
    const bool preflight_matches =
        directory == detail::g_preflight_directory;
    if (!preflight_matches
        || !detail::g_preflight_directory_compatible
        || !detail::directory_valid(directory)) {
        ereport(WARNING,
            (errmsg("ignoring incompatible bioseq cache-provider directory")));
        return true;
    }
    try {
        if (directory->bind_cache_api(&detail::g_cache_api)
            != bioseq_cache_status_ok) {
            ereport(WARNING,
                (errmsg("bioseq cache-provider directory declined binding")));
        }
    } catch (...) {
        /* Report the optional provider failure. The cache API has already been published. */
        ereport(WARNING,
            (errmsg("bioseq cache-provider directory binding failed")));
    }
    return true;
}

const bioseq::cache::bioseq_cache_api& cache_api() noexcept {
    return detail::g_cache_api;
}

const bioseq::cache::bioseq_cache_planner_bridge&
planner_bridge() noexcept {
    return detail::g_planner_bridge;
}

#if defined(BIOSEQ_CACHE_TESTING)
bool provider_registry_owns_copy() {
    if (!superuser()) {
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
             errmsg("provider registry copy probe requires superuser")));
    }

    const provider_snapshot before = snapshot_providers();
    if (before.provider_count == 0 || before.providers[0] == nullptr
        || find_provider(1U) != nullptr) {
        return false;
    }

    const provider& source = *before.providers[0];
    const std::uint32_t source_id = source.provider_id;
    const auto source_analyze = source.analyze_query;
    char probe_name[] = "registry-copy-probe";
    provider candidate = source;
    candidate.provider_id = 1U;
    candidate.provider_name = probe_name;
    candidate.provider_scan_methods =
        reinterpret_cast<const CustomScanMethods*>(&candidate);
    const auto probe_methods = candidate.provider_scan_methods;
    provider_registration registration{
        .struct_size = sizeof(provider_registration),
    };

    if (register_provider(&candidate, &registration)
        != bioseq_cache_status_ok) {
        return false;
    }

    probe_name[0] = 'X';
    candidate.provider_id = 2U;
    candidate.provider_name = "mutated";
    candidate.provider_scan_methods = nullptr;
    candidate.analyze_query = nullptr;

    const provider* const stored = find_provider(1U);
    bool valid = stored != nullptr
        && stored != &candidate
        && stored->provider_id == 1U
        && stored->provider_name != probe_name
        && detail::provider_names_equal(
            stored->provider_name, "registry-copy-probe")
        && stored->provider_scan_methods == probe_methods
        && stored->analyze_query == source_analyze
        && validate_provider(stored);

    valid = valid && unregister_provider(&registration);
    const provider* const restored = find_provider(source_id);
    valid = valid
        && find_provider(1U) == nullptr
        && restored != nullptr
        && restored->provider_id == source_id
        && restored->analyze_query == source_analyze
        && snapshot_providers().provider_count == before.provider_count;
    return valid;
}
#endif

} // namespace bioseq::cache::pg::provider_registry

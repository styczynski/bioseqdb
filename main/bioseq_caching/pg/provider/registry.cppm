/*-------------------------------------------------------------------------
 *
 * provider/registry.cppm
 *     Backend-local V2 provider registry and rendezvous publication.
 *
 * Provider descriptors and names are copied into stable, never-compacted
 * slots.  A removed slot becomes a permanent tombstone for the life of the
 * backend.  Registration tokens authenticate synthetic unregister requests;
 * production providers remain registered for process lifetime.
 *
 *-------------------------------------------------------------------------
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

export module bioseq.cache.pg.provider:registry;

import bioseq.cache.provider_api;

export namespace bioseq::cache::pg::provider_registry {

using ProviderV2 = bioseq::cache::BioseqCacheProviderV2;
using ProviderRegistrationV2 =
    bioseq::cache::BioseqCacheProviderRegistrationV2;
using ProviderDirectoryV2 =
    bioseq::cache::BioseqCacheProviderDirectoryV2;
using PhysicalIdentityV2 = bioseq::cache::BioseqCachePhysicalIdentityV2;
using PlannerBridgeV2 = bioseq::cache::BioseqCachePlannerBridgeV2;
using CacheApiV2 = bioseq::cache::BioseqCacheApiV2;

inline constexpr std::uint32_t kPostgresMajorVersion =
    static_cast<std::uint32_t>(PG_VERSION_NUM / 10000);
inline constexpr std::size_t kMaximumRegisteredProvidersV2 = 32;
inline constexpr std::size_t kMaximumProviderNameBytesV2 = 128;
inline constexpr std::size_t kInvalidProviderSlotV2 =
    (std::numeric_limits<std::size_t>::max)();

/** Immutable point-in-time copy of active stable provider pointers. */
struct ProviderSnapshotV2 {
    std::uint32_t struct_size = sizeof(ProviderSnapshotV2);
    std::uint32_t provider_count = 0;
    std::uint64_t generation = 0;
    const ProviderV2* providers[kMaximumRegisteredProvidersV2]{};
};

/** Execution-scoped proof that one stable provider slot remains active. */
struct ProviderLeaseV2 {
    std::size_t slot_index = kInvalidProviderSlotV2;
    std::uint64_t registry_generation = 0;
    std::uint32_t provider_id = 0;
    std::uint64_t owner_cookie = 0;
};

/** One in-flight callback counted against registry mutation. */
struct ProviderCallbackTokenV2 {
    std::size_t slot_index = kInvalidProviderSlotV2;
    std::uint64_t registry_generation = 0;
    std::uint64_t owner_cookie = 0;
};

/** A null identity requests conservative/coarse write observation. */
using RelationWriteObserverV2 = void (*)(
    std::uint32_t database_oid,
    std::uint32_t relation_oid,
    const PhysicalIdentityV2* physical_identity) noexcept;

using PlannerBridgeObserverV2 = void (*)(
    PlannerInfo* root,
    RelOptInfo* rel,
    unsigned int rti,
    RangeTblEntry* rte,
    const ProviderV2* provider) noexcept;

/** Cache-owned observer for a provider's fully mutated PlannedStmt. */
using FinalizedPlanBridgeObserverV2 = void (*)(
    PlannedStmt* statement) noexcept;

[[nodiscard]] bool validate_provider_v2(const ProviderV2* provider) noexcept;
[[nodiscard]] BioseqCacheStatusV2 register_provider_v2(
    const ProviderV2* provider,
    ProviderRegistrationV2* registration) noexcept;
[[nodiscard]] bool unregister_provider_v2(
    const ProviderRegistrationV2* registration) noexcept;
[[nodiscard]] const ProviderV2* find_provider_v2(
    std::uint32_t provider_id) noexcept;
[[nodiscard]] ProviderSnapshotV2 snapshot_providers_v2() noexcept;

[[nodiscard]] bool acquire_provider_lease_v2(
    const ProviderV2* provider,
    ProviderLeaseV2* lease) noexcept;
[[nodiscard]] const ProviderV2* provider_from_lease_v2(
    const ProviderLeaseV2& lease) noexcept;
[[nodiscard]] bool validate_provider_lease_v2(
    const ProviderLeaseV2& lease,
    const ProviderV2* expected_provider = nullptr) noexcept;
void release_provider_lease_v2(ProviderLeaseV2* lease) noexcept;

[[nodiscard]] bool begin_provider_callback_v2(
    const ProviderV2* provider,
    const ProviderLeaseV2* lease,
    ProviderCallbackTokenV2* token) noexcept;
void end_provider_callback_v2(ProviderCallbackTokenV2* token) noexcept;

void install_relation_write_observer_v2(
    RelationWriteObserverV2 observer) noexcept;
void record_relation_write_v2(
    std::uint32_t database_oid,
    std::uint32_t relation_oid,
    const PhysicalIdentityV2* physical_identity) noexcept;

void install_planner_bridge_observer_v2(
    PlannerBridgeObserverV2 observer) noexcept;
void after_provider_rel_paths_v2(
    PlannerInfo* root,
    RelOptInfo* rel,
    unsigned int rti,
    RangeTblEntry* rte,
    const ProviderV2* provider) noexcept;
void install_finalized_plan_bridge_observer_v2(
    FinalizedPlanBridgeObserverV2 observer) noexcept;
void after_planned_statement_v2(PlannedStmt* statement) noexcept;

/** Validate publication ownership and the optional peer before mutation. */
[[nodiscard]] bool preflight_provider_registry_v2() noexcept;

/** Publish the cache API, then request an optional Bioseq directory bind. */
[[nodiscard]] bool initialize_provider_registry_v2() noexcept;

[[nodiscard]] const CacheApiV2& cache_api_v2() noexcept;
[[nodiscard]] const PlannerBridgeV2& planner_bridge_v2() noexcept;

#if defined(BIOSEQ_CACHE_TESTING)
/** Test-only copy/token/tombstone probe exposed through generated SQL. */
[[nodiscard]] bool provider_registry_owns_copy();
#endif

static_assert(std::is_standard_layout_v<ProviderSnapshotV2>);
static_assert(std::is_trivially_copyable_v<ProviderSnapshotV2>);
static_assert(std::is_standard_layout_v<ProviderLeaseV2>);
static_assert(std::is_trivially_copyable_v<ProviderLeaseV2>);
static_assert(std::is_standard_layout_v<ProviderCallbackTokenV2>);
static_assert(std::is_trivially_copyable_v<ProviderCallbackTokenV2>);

} // namespace bioseq::cache::pg::provider_registry

namespace bioseq::cache::pg::provider_registry::detail {

enum class ProviderSlotState : std::uint32_t {
    Vacant = 0,
    Active = 1,
    Tombstone = 2,
};

struct ProviderSlot {
    ProviderSlotState state = ProviderSlotState::Vacant;
    std::uint32_t provider_id = 0;
    std::uint64_t registration_generation = 0;
    std::uint64_t owner_cookie = 0;
    std::uint32_t active_callbacks = 0;
    std::uint32_t active_leases = 0;
    ProviderV2 provider{};
    char provider_name[kMaximumProviderNameBytesV2]{};
};

static_assert(std::is_standard_layout_v<ProviderSlot>);
static_assert(std::is_trivially_copyable_v<ProviderSlot>);

inline ProviderSlot g_provider_slots[kMaximumRegisteredProvidersV2]{};
inline std::size_t g_next_provider_slot = 0;
inline std::size_t g_active_provider_count = 0;
inline std::uint64_t g_registry_generation = 1;
inline std::uint64_t g_registry_cookie = 0;
inline std::uint32_t g_active_callback_count = 0;
inline RelationWriteObserverV2 g_relation_write_observer = nullptr;
inline PlannerBridgeObserverV2 g_planner_bridge_observer = nullptr;
inline FinalizedPlanBridgeObserverV2
    g_finalized_plan_bridge_observer = nullptr;
inline const ProviderDirectoryV2* g_preflight_directory = nullptr;
inline bool g_preflight_directory_compatible = false;
inline void** g_cache_api_rendezvous_slot = nullptr;
inline void** g_provider_directory_rendezvous_slot = nullptr;

inline constexpr std::uint32_t kKnownProviderFlags =
    static_cast<std::uint32_t>(
        BIOSEQ_CACHE_PROVIDER_EXACT_SOURCE_ELISION_SAFE)
    | static_cast<std::uint32_t>(BIOSEQ_CACHE_PROVIDER_REUSABLE_COVERAGE)
    | static_cast<std::uint32_t>(
        BIOSEQ_CACHE_PROVIDER_COMPLETE_WRITE_OBSERVATION);

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
    for (std::size_t index = 0; index < kMaximumProviderNameBytesV2;
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
    for (std::size_t index = 0; index < kMaximumProviderNameBytesV2;
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
        const ProviderV2& left, const ProviderV2& right) noexcept {
    return left.abi_version == right.abi_version
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
        ProviderSlot& destination,
        const ProviderV2& source,
        std::size_t slot_index) noexcept {
    destination = {};
    destination.state = ProviderSlotState::Active;
    destination.provider_id = source.provider_id;
    destination.registration_generation = next_generation();
    destination.owner_cookie = mix_cookie(
        process_cookie()
        ^ destination.registration_generation
        ^ (static_cast<std::uint64_t>(source.provider_id) << 17U)
        ^ static_cast<std::uint64_t>(slot_index));
    destination.provider = source;
    destination.provider.struct_size =
        static_cast<std::uint32_t>(sizeof(ProviderV2));
    for (std::size_t index = 0; index < kMaximumProviderNameBytesV2;
         ++index) {
        destination.provider_name[index] = source.provider_name[index];
        if (source.provider_name[index] == '\0') {
            break;
        }
    }
    destination.provider.provider_name = destination.provider_name;
}

[[nodiscard]] bool baseline_callbacks_present(
        const ProviderV2& provider) noexcept {
    return provider.provider_scan_methods != nullptr
        && provider.analyze_query != nullptr
        && provider.describe_path != nullptr
        && provider.append_domain_semantic_digest != nullptr
        && provider.append_generation_semantic_digest != nullptr
        && provider.append_runtime_generation_semantic_digest != nullptr
        && provider.build_plan_digest_recipe != nullptr;
}

[[nodiscard]] bool reusable_callbacks_present(
        const ProviderV2& provider) noexcept {
    return provider.map_reusable_source != nullptr
        && provider.decorate_path != nullptr
        && provider.extract_occurrence != nullptr
        && provider.late_fetch_batch != nullptr;
}

[[nodiscard]] std::size_t provider_slot_for_id(
        std::uint32_t provider_id) noexcept {
    if (provider_id == 0) {
        return kInvalidProviderSlotV2;
    }
    for (std::size_t index = 0; index < g_next_provider_slot; ++index) {
        const ProviderSlot& slot = g_provider_slots[index];
        if (slot.state == ProviderSlotState::Active
            && slot.provider_id == provider_id) {
            return index;
        }
    }
    return kInvalidProviderSlotV2;
}

[[nodiscard]] std::size_t provider_slot_for_pointer(
        const ProviderV2* provider) noexcept {
    if (provider == nullptr) {
        return kInvalidProviderSlotV2;
    }
    for (std::size_t index = 0; index < g_next_provider_slot; ++index) {
        const ProviderSlot& slot = g_provider_slots[index];
        if (slot.state == ProviderSlotState::Active
            && &slot.provider == provider) {
            return index;
        }
    }
    return kInvalidProviderSlotV2;
}

[[nodiscard]] bool registration_token_valid(
        const ProviderRegistrationV2* registration,
        std::size_t* slot_index) noexcept {
    if (slot_index != nullptr) {
        *slot_index = kInvalidProviderSlotV2;
    }
    if (registration == nullptr
        || registration->struct_size
            < offsetof(ProviderRegistrationV2, owner_cookie)
                + sizeof(registration->owner_cookie)
        || registration->provider_id == 0
        || registration->registry_generation == 0
        || registration->owner_cookie == 0) {
        return false;
    }
    const std::size_t index =
        provider_slot_for_id(registration->provider_id);
    if (index == kInvalidProviderSlotV2) {
        return false;
    }
    const ProviderSlot& slot = g_provider_slots[index];
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
        const PhysicalIdentityV2* identity) noexcept {
    if (identity == nullptr
        || identity->struct_size
            < offsetof(PhysicalIdentityV2, bytes)
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
        const ProviderDirectoryV2* directory) noexcept {
    return directory != nullptr
        && directory->struct_size
            >= offsetof(ProviderDirectoryV2, bind_cache_api)
                + sizeof(directory->bind_cache_api)
        && directory->abi_version == bioseq::cache::kProviderAbiVersion
        && directory->postgres_major == kPostgresMajorVersion
        && directory->reserved == 0
        && directory->instance_cookie != 0
        && directory->bind_cache_api != nullptr;
}

[[nodiscard]] BioseqCacheStatusV2 api_register_provider(
        const ProviderV2* provider,
        ProviderRegistrationV2* registration) noexcept {
    return register_provider_v2(provider, registration);
}

[[nodiscard]] bool api_unregister_provider(
        const ProviderRegistrationV2* registration) noexcept {
    return unregister_provider_v2(registration);
}

void api_record_relation_write(
        std::uint32_t database_oid,
        std::uint32_t relation_oid,
        const PhysicalIdentityV2* physical_identity) noexcept {
    record_relation_write_v2(database_oid, relation_oid, physical_identity);
}

void bridge_after_provider_rel_paths(
        PlannerInfo* root,
        RelOptInfo* rel,
        unsigned int rti,
        RangeTblEntry* rte,
        const ProviderV2* provider) noexcept {
    after_provider_rel_paths_v2(root, rel, rti, rte, provider);
}

void bridge_after_planned_statement(PlannedStmt* statement) noexcept {
    after_planned_statement_v2(statement);
}

inline constinit PlannerBridgeV2 g_planner_bridge_v2{
    static_cast<std::uint32_t>(sizeof(PlannerBridgeV2)),
    bioseq::cache::kProviderAbiVersion,
    kPostgresMajorVersion,
    &bridge_after_provider_rel_paths,
    &bridge_after_planned_statement,
};

inline CacheApiV2 g_cache_api_v2{
    .struct_size = static_cast<std::uint32_t>(sizeof(CacheApiV2)),
    .abi_version = bioseq::cache::kProviderAbiVersion,
    .postgres_major = kPostgresMajorVersion,
    .reserved = 0,
    .instance_cookie = 0,
    .register_provider = &api_register_provider,
    .unregister_provider = &api_unregister_provider,
    .record_relation_write = &api_record_relation_write,
    .planner_bridge = &g_planner_bridge_v2,
};

} // namespace bioseq::cache::pg::provider_registry::detail

namespace bioseq::cache::pg::provider_registry {

bool validate_provider_v2(const ProviderV2* provider) noexcept {
    if (provider == nullptr
        || provider->struct_size
            < offsetof(ProviderV2, explain_domain)
                + sizeof(provider->explain_domain)
        || provider->abi_version != bioseq::cache::kProviderAbiVersion
        || provider->postgres_major != kPostgresMajorVersion
        || provider->provider_id == 0
        || provider->reserved != 0
        || (provider->provider_flags & ~detail::kKnownProviderFlags) != 0
        || !detail::provider_name_valid(provider->provider_name)
        || !detail::baseline_callbacks_present(*provider)) {
        return false;
    }
    const bool reusable =
        (provider->provider_flags
         & static_cast<std::uint32_t>(
             BIOSEQ_CACHE_PROVIDER_REUSABLE_COVERAGE)) != 0;
    return !reusable || detail::reusable_callbacks_present(*provider);
}

BioseqCacheStatusV2 register_provider_v2(
        const ProviderV2* provider,
        ProviderRegistrationV2* registration) noexcept {
    if (registration == nullptr
        || registration->struct_size
            < offsetof(ProviderRegistrationV2, owner_cookie)
                + sizeof(registration->owner_cookie)
        || !validate_provider_v2(provider)
        || detail::g_active_callback_count != 0) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }

    const std::size_t existing =
        detail::provider_slot_for_id(provider->provider_id);
    if (existing != kInvalidProviderSlotV2) {
        const detail::ProviderSlot& slot =
            detail::g_provider_slots[existing];
        if (!detail::provider_tables_equal(slot.provider, *provider)) {
            return BIOSEQ_CACHE_STATUS_ERROR;
        }
        *registration = ProviderRegistrationV2{
            .struct_size =
                static_cast<std::uint32_t>(sizeof(ProviderRegistrationV2)),
            .provider_id = slot.provider_id,
            .registry_generation = slot.registration_generation,
            .owner_cookie = slot.owner_cookie,
        };
        return BIOSEQ_CACHE_STATUS_OK;
    }

    for (std::size_t index = 0;
         index < detail::g_next_provider_slot;
         ++index) {
        const detail::ProviderSlot& slot = detail::g_provider_slots[index];
        if (slot.state != detail::ProviderSlotState::Active) {
            continue;
        }
        if (slot.provider.provider_scan_methods
                == provider->provider_scan_methods
            || detail::provider_names_equal(
                slot.provider.provider_name, provider->provider_name)) {
            return BIOSEQ_CACHE_STATUS_ERROR;
        }
    }
    if (detail::g_next_provider_slot == kMaximumRegisteredProvidersV2) {
        return BIOSEQ_CACHE_STATUS_ERROR;
    }

    const std::size_t slot_index = detail::g_next_provider_slot++;
    detail::ProviderSlot& slot = detail::g_provider_slots[slot_index];
    detail::capture_provider(slot, *provider, slot_index);
    ++detail::g_active_provider_count;
    *registration = ProviderRegistrationV2{
        .struct_size =
            static_cast<std::uint32_t>(sizeof(ProviderRegistrationV2)),
        .provider_id = slot.provider_id,
        .registry_generation = slot.registration_generation,
        .owner_cookie = slot.owner_cookie,
    };
    ResetPlanCache();
    return BIOSEQ_CACHE_STATUS_OK;
}

bool unregister_provider_v2(
        const ProviderRegistrationV2* registration) noexcept {
    if (detail::g_active_callback_count != 0) {
        return false;
    }
    std::size_t slot_index = kInvalidProviderSlotV2;
    if (!detail::registration_token_valid(registration, &slot_index)) {
        return false;
    }
    detail::ProviderSlot& slot = detail::g_provider_slots[slot_index];
    if (slot.active_callbacks != 0 || slot.active_leases != 0) {
        return false;
    }
    slot.state = detail::ProviderSlotState::Tombstone;
    slot.provider = {};
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

const ProviderV2* find_provider_v2(std::uint32_t provider_id) noexcept {
    const std::size_t slot_index =
        detail::provider_slot_for_id(provider_id);
    return slot_index == kInvalidProviderSlotV2
        ? nullptr
        : &detail::g_provider_slots[slot_index].provider;
}

ProviderSnapshotV2 snapshot_providers_v2() noexcept {
    ProviderSnapshotV2 snapshot{};
    snapshot.generation = detail::g_registry_generation;
    for (std::size_t index = 0;
         index < detail::g_next_provider_slot;
         ++index) {
        const detail::ProviderSlot& slot = detail::g_provider_slots[index];
        if (slot.state != detail::ProviderSlotState::Active) {
            continue;
        }
        snapshot.providers[snapshot.provider_count++] = &slot.provider;
    }
    return snapshot;
}

bool acquire_provider_lease_v2(
        const ProviderV2* provider,
        ProviderLeaseV2* lease) noexcept {
    if (lease == nullptr) {
        return false;
    }
    *lease = {};
    const std::size_t slot_index =
        detail::provider_slot_for_pointer(provider);
    if (slot_index == kInvalidProviderSlotV2) {
        return false;
    }
    detail::ProviderSlot& slot = detail::g_provider_slots[slot_index];
    if (slot.active_leases
        == (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    ++slot.active_leases;
    *lease = ProviderLeaseV2{
        .slot_index = slot_index,
        .registry_generation = slot.registration_generation,
        .provider_id = slot.provider_id,
        .owner_cookie = slot.owner_cookie,
    };
    return true;
}

const ProviderV2* provider_from_lease_v2(
        const ProviderLeaseV2& lease) noexcept {
    if (lease.slot_index >= detail::g_next_provider_slot
        || lease.slot_index == kInvalidProviderSlotV2) {
        return nullptr;
    }
    const detail::ProviderSlot& slot =
        detail::g_provider_slots[lease.slot_index];
    if (slot.state != detail::ProviderSlotState::Active
        || slot.provider_id != lease.provider_id
        || slot.registration_generation != lease.registry_generation
        || slot.owner_cookie != lease.owner_cookie
        || slot.active_leases == 0) {
        return nullptr;
    }
    return &slot.provider;
}

bool validate_provider_lease_v2(
        const ProviderLeaseV2& lease,
        const ProviderV2* expected_provider) noexcept {
    const ProviderV2* provider = provider_from_lease_v2(lease);
    return provider != nullptr
        && (expected_provider == nullptr || provider == expected_provider);
}

void release_provider_lease_v2(ProviderLeaseV2* lease) noexcept {
    if (lease == nullptr) {
        return;
    }
    if (lease->slot_index < detail::g_next_provider_slot
        && lease->slot_index != kInvalidProviderSlotV2) {
        detail::ProviderSlot& slot =
            detail::g_provider_slots[lease->slot_index];
        if (slot.state == detail::ProviderSlotState::Active
            && slot.provider_id == lease->provider_id
            && slot.registration_generation == lease->registry_generation
            && slot.owner_cookie == lease->owner_cookie
            && slot.active_leases != 0) {
            --slot.active_leases;
        }
    }
    *lease = {};
}

bool begin_provider_callback_v2(
        const ProviderV2* provider,
        const ProviderLeaseV2* lease,
        ProviderCallbackTokenV2* token) noexcept {
    if (token == nullptr) {
        return false;
    }
    *token = {};
    std::size_t slot_index = kInvalidProviderSlotV2;
    if (lease != nullptr) {
        if (!validate_provider_lease_v2(*lease, provider)) {
            return false;
        }
        slot_index = lease->slot_index;
    } else {
        slot_index = detail::provider_slot_for_pointer(provider);
    }
    if (slot_index == kInvalidProviderSlotV2) {
        return false;
    }
    detail::ProviderSlot& slot = detail::g_provider_slots[slot_index];
    if (slot.active_callbacks
            == (std::numeric_limits<std::uint32_t>::max)()
        || detail::g_active_callback_count
            == (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    ++slot.active_callbacks;
    ++detail::g_active_callback_count;
    *token = ProviderCallbackTokenV2{
        .slot_index = slot_index,
        .registry_generation = slot.registration_generation,
        .owner_cookie = slot.owner_cookie,
    };
    return true;
}

void end_provider_callback_v2(ProviderCallbackTokenV2* token) noexcept {
    if (token == nullptr) {
        return;
    }
    if (token->slot_index < detail::g_next_provider_slot
        && token->slot_index != kInvalidProviderSlotV2) {
        detail::ProviderSlot& slot =
            detail::g_provider_slots[token->slot_index];
        if (slot.state == detail::ProviderSlotState::Active
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

void install_relation_write_observer_v2(
        RelationWriteObserverV2 observer) noexcept {
    detail::g_relation_write_observer = observer;
}

void record_relation_write_v2(
        std::uint32_t database_oid,
        std::uint32_t relation_oid,
        const PhysicalIdentityV2* physical_identity) noexcept {
    const RelationWriteObserverV2 observer =
        detail::g_relation_write_observer;
    if (observer == nullptr) {
        return;
    }
    const PhysicalIdentityV2* validated_identity =
        detail::physical_identity_valid(physical_identity)
        ? physical_identity
        : nullptr;
    observer(database_oid, relation_oid, validated_identity);
}

void install_planner_bridge_observer_v2(
        PlannerBridgeObserverV2 observer) noexcept {
    detail::g_planner_bridge_observer = observer;
}

void after_provider_rel_paths_v2(
        PlannerInfo* root,
        RelOptInfo* rel,
        unsigned int rti,
        RangeTblEntry* rte,
        const ProviderV2* provider) noexcept {
    const PlannerBridgeObserverV2 observer =
        detail::g_planner_bridge_observer;
    if (observer == nullptr || root == nullptr || rel == nullptr
        || rti == 0 || rte == nullptr || !validate_provider_v2(provider)) {
        return;
    }
    const ProviderV2* const registered =
        find_provider_v2(provider->provider_id);
    if (registered == nullptr
        || !detail::provider_tables_equal(*registered, *provider)) {
        return;
    }
    observer(root, rel, rti, rte, registered);
}

void install_finalized_plan_bridge_observer_v2(
        FinalizedPlanBridgeObserverV2 observer) noexcept {
    detail::g_finalized_plan_bridge_observer = observer;
}

void after_planned_statement_v2(PlannedStmt* statement) noexcept {
    const FinalizedPlanBridgeObserverV2 observer =
        detail::g_finalized_plan_bridge_observer;
    if (observer != nullptr && statement != nullptr) {
        observer(statement);
    }
}

bool preflight_provider_registry_v2() noexcept {
    detail::g_cache_api_rendezvous_slot = find_rendezvous_variable(
        bioseq::cache::kCacheApiRendezvousName);
    if (detail::g_cache_api_rendezvous_slot == nullptr
        || (*detail::g_cache_api_rendezvous_slot != nullptr
            && *detail::g_cache_api_rendezvous_slot
                != static_cast<void*>(&detail::g_cache_api_v2))) {
        return false;
    }
    detail::g_provider_directory_rendezvous_slot =
        find_rendezvous_variable(
            bioseq::cache::kProviderDirectoryRendezvousName);
    detail::g_preflight_directory =
        detail::g_provider_directory_rendezvous_slot == nullptr
            || *detail::g_provider_directory_rendezvous_slot == nullptr
        ? nullptr
        : static_cast<const ProviderDirectoryV2*>(
            *detail::g_provider_directory_rendezvous_slot);
    detail::g_preflight_directory_compatible =
        detail::g_preflight_directory != nullptr
        && detail::directory_valid(detail::g_preflight_directory);
    if (detail::g_cache_api_v2.instance_cookie == 0) {
        detail::g_cache_api_v2.instance_cookie =
            detail::mix_cookie(
                detail::process_cookie() ^ 0x76325f6361636865ULL);
    }
    return true;
}

bool initialize_provider_registry_v2() noexcept {
    if (detail::g_cache_api_rendezvous_slot == nullptr
        || (*detail::g_cache_api_rendezvous_slot != nullptr
            && *detail::g_cache_api_rendezvous_slot
                != static_cast<void*>(&detail::g_cache_api_v2))) {
        return false;
    }
    *detail::g_cache_api_rendezvous_slot =
        static_cast<void*>(&detail::g_cache_api_v2);

    if (detail::g_provider_directory_rendezvous_slot == nullptr
        || *detail::g_provider_directory_rendezvous_slot == nullptr) {
        return true;
    }
    const auto* directory =
        static_cast<const ProviderDirectoryV2*>(
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
        if (directory->bind_cache_api(&detail::g_cache_api_v2)
            != BIOSEQ_CACHE_STATUS_OK) {
            ereport(WARNING,
                (errmsg("bioseq cache-provider directory declined binding")));
        }
    } catch (...) {
        /* Optional providers fail open; the published cache API stays valid. */
        ereport(WARNING,
            (errmsg("bioseq cache-provider directory binding failed")));
    }
    return true;
}

const CacheApiV2& cache_api_v2() noexcept {
    return detail::g_cache_api_v2;
}

const PlannerBridgeV2& planner_bridge_v2() noexcept {
    return detail::g_planner_bridge_v2;
}

#if defined(BIOSEQ_CACHE_TESTING)
bool provider_registry_owns_copy() {
    if (!superuser()) {
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
             errmsg("provider registry copy probe requires superuser")));
    }

    const ProviderSnapshotV2 before = snapshot_providers_v2();
    if (before.provider_count == 0 || before.providers[0] == nullptr
        || find_provider_v2(1U) != nullptr) {
        return false;
    }

    const ProviderV2& source = *before.providers[0];
    const std::uint32_t source_id = source.provider_id;
    const auto source_analyze = source.analyze_query;
    char probe_name[] = "registry-copy-probe";
    ProviderV2 candidate = source;
    candidate.provider_id = 1U;
    candidate.provider_name = probe_name;
    candidate.provider_scan_methods =
        reinterpret_cast<const CustomScanMethods*>(&candidate);
    const auto probe_methods = candidate.provider_scan_methods;
    ProviderRegistrationV2 registration{
        .struct_size = sizeof(ProviderRegistrationV2),
    };

    if (register_provider_v2(&candidate, &registration)
        != BIOSEQ_CACHE_STATUS_OK) {
        return false;
    }

    probe_name[0] = 'X';
    candidate.provider_id = 2U;
    candidate.provider_name = "mutated";
    candidate.provider_scan_methods = nullptr;
    candidate.analyze_query = nullptr;

    const ProviderV2* const stored = find_provider_v2(1U);
    bool valid = stored != nullptr
        && stored != &candidate
        && stored->provider_id == 1U
        && stored->provider_name != probe_name
        && detail::provider_names_equal(
            stored->provider_name, "registry-copy-probe")
        && stored->provider_scan_methods == probe_methods
        && stored->analyze_query == source_analyze
        && validate_provider_v2(stored);

    valid = valid && unregister_provider_v2(&registration);
    const ProviderV2* const restored = find_provider_v2(source_id);
    valid = valid
        && find_provider_v2(1U) == nullptr
        && restored != nullptr
        && restored->provider_id == source_id
        && restored->analyze_query == source_analyze
        && snapshot_providers_v2().provider_count == before.provider_count;
    return valid;
}
#endif

} // namespace bioseq::cache::pg::provider_registry

/*-------------------------------------------------------------------------
 *
 * provider_registry.cppm
 *     Backend-local provider registry and PostgreSQL rendezvous publication.
 *
 * The registry owns bounded copies of validated provider tables and names.
 * Callback and PostgreSQL method pointers remain valid only while the provider
 * DSO is mapped, so providers unregister before unload.  All exported
 * operations are bounded, allocation-free and non-throwing so malformed or
 * late-loaded providers fail closed.
 *
 *-------------------------------------------------------------------------
 */
module;

extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/errcodes.h"
}

#include <cstddef>
#include <cstdint>
#include <type_traits>

export module bioseq.cache.pg.provider_registry;

import bioseq.cache.provider_api;

export namespace bioseq::cache::pg::provider_registry {

using ProviderV1 = bioseq::cache::BioseqCacheProviderV1;
using PhysicalIdentityV1 = bioseq::cache::BioseqCachePhysicalIdentityV1;
using PlannerBridgeV1 = bioseq::cache::BioseqCachePlannerBridgeV1;
using CacheApiV1 = bioseq::cache::BioseqCacheApiV1;

inline constexpr std::uint32_t kPostgresMajorVersion =
    static_cast<std::uint32_t>(PG_VERSION_NUM / 10000);
inline constexpr std::size_t kMaximumRegisteredProvidersV1 = 32;
inline constexpr std::size_t kMaximumProviderNameBytesV1 = 128;
inline constexpr char kCacheApiRendezvousNameV1[] = "bioseq_cache.api.v1";
inline constexpr char kPlannerBridgeRendezvousNameV1[] =
    "bioseq_cache.planner_bridge.v1";

/** Immutable point-in-time copy of the backend-local provider pointer set. */
struct ProviderSnapshotV1 {
    std::uint32_t struct_size;
    std::uint32_t provider_count;
    std::uint64_t generation;
    const ProviderV1* providers[kMaximumRegisteredProvidersV1];
};

/** A null identity requests conservative/coarse write observation. */
using RelationWriteObserverV1 = void (*)(
    std::uint32_t database_oid,
    std::uint32_t relation_oid,
    const PhysicalIdentityV1* physical_identity) noexcept;

using PlannerBridgeObserverV1 = void (*)(
    PlannerInfo* root,
    RelOptInfo* rel,
    unsigned int rti,
    RangeTblEntry* rte,
    const ProviderV1* provider) noexcept;

[[nodiscard]] bool validate_provider_v1(const ProviderV1* provider) noexcept;
[[nodiscard]] bool register_provider_v1(const ProviderV1* provider) noexcept;
void unregister_provider_v1(std::uint32_t provider_id) noexcept;
[[nodiscard]] const ProviderV1* find_provider_v1(
    std::uint32_t provider_id) noexcept;
[[nodiscard]] ProviderSnapshotV1 snapshot_providers_v1() noexcept;

void install_relation_write_observer_v1(
    RelationWriteObserverV1 observer) noexcept;
void record_relation_write_v1(
    std::uint32_t database_oid,
    std::uint32_t relation_oid,
    const PhysicalIdentityV1* physical_identity) noexcept;

void install_planner_bridge_observer_v1(
    PlannerBridgeObserverV1 observer) noexcept;
void after_provider_rel_paths_v1(
    PlannerInfo* root,
    RelOptInfo* rel,
    unsigned int rti,
    RangeTblEntry* rte,
    const ProviderV1* provider) noexcept;

/** Publish both cache rendezvous pointers. Idempotent for this registry. */
[[nodiscard]] bool initialize_provider_registry_v1() noexcept;

/** Clear only rendezvous slots still owned by this registry. */
void unpublish_provider_registry_v1() noexcept;

[[nodiscard]] const CacheApiV1& cache_api_v1() noexcept;
[[nodiscard]] const PlannerBridgeV1& planner_bridge_v1() noexcept;

static_assert(std::is_standard_layout_v<ProviderSnapshotV1>);
static_assert(std::is_trivially_copyable_v<ProviderSnapshotV1>);
static_assert(std::is_standard_layout_v<ProviderV1>);
static_assert(std::is_trivially_copyable_v<ProviderV1>);
static_assert(std::is_standard_layout_v<PhysicalIdentityV1>);
static_assert(std::is_trivially_copyable_v<PhysicalIdentityV1>);
static_assert(std::is_standard_layout_v<PlannerBridgeV1>);
static_assert(std::is_trivially_copyable_v<PlannerBridgeV1>);
static_assert(std::is_standard_layout_v<CacheApiV1>);
static_assert(std::is_trivially_copyable_v<CacheApiV1>);

} // namespace bioseq::cache::pg::provider_registry

namespace bioseq::cache::pg::provider_registry::detail {

struct ProviderSlot {
    std::uint32_t provider_id;
    ProviderV1 provider;
    char provider_name[kMaximumProviderNameBytesV1];
};

static_assert(std::is_standard_layout_v<ProviderSlot>);
static_assert(std::is_trivially_copyable_v<ProviderSlot>);

inline ProviderSlot g_provider_slots[kMaximumRegisteredProvidersV1]{};
inline std::size_t g_provider_count = 0;
inline std::uint64_t g_provider_generation = 1;
inline RelationWriteObserverV1 g_relation_write_observer = nullptr;
inline PlannerBridgeObserverV1 g_planner_bridge_observer = nullptr;
inline void** g_cache_api_rendezvous_slot = nullptr;
inline void** g_planner_bridge_rendezvous_slot = nullptr;

inline constexpr std::uint32_t kKnownProviderFlags =
    static_cast<std::uint32_t>(
        BIOSEQ_CACHE_PROVIDER_EXACT_SOURCE_ELISION_SAFE)
    | static_cast<std::uint32_t>(BIOSEQ_CACHE_PROVIDER_REUSABLE_COVERAGE)
    | static_cast<std::uint32_t>(
        BIOSEQ_CACHE_PROVIDER_COMPLETE_WRITE_OBSERVATION);

[[nodiscard]] bool provider_name_valid(const char* name) noexcept {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }

    bool contains_non_space = false;
    for (std::size_t index = 0; index < kMaximumProviderNameBytesV1; ++index) {
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
    for (std::size_t index = 0; index < kMaximumProviderNameBytesV1; ++index) {
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
        const ProviderV1& left, const ProviderV1& right) noexcept {
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

void clear_provider_slot(ProviderSlot& slot) noexcept {
    slot = {};
}

void copy_provider_slot(
        ProviderSlot& destination, const ProviderSlot& source) noexcept {
    destination.provider_id = source.provider_id;
    destination.provider = source.provider;
    for (std::size_t index = 0; index < kMaximumProviderNameBytesV1;
         ++index) {
        destination.provider_name[index] = source.provider_name[index];
    }
    destination.provider.provider_name = destination.provider_name;
}

void capture_provider(
        ProviderSlot& destination, const ProviderV1& source) noexcept {
    clear_provider_slot(destination);
    destination.provider_id = source.provider_id;
    destination.provider = source;
    destination.provider.struct_size =
        static_cast<std::uint32_t>(sizeof(ProviderV1));
    for (std::size_t index = 0; index < kMaximumProviderNameBytesV1;
         ++index) {
        destination.provider_name[index] = source.provider_name[index];
        if (source.provider_name[index] == '\0') {
            break;
        }
    }
    destination.provider.provider_name = destination.provider_name;
}

[[nodiscard]] bool baseline_callbacks_present(
        const ProviderV1& provider) noexcept {
    return provider.provider_scan_methods != nullptr
        && provider.analyze_query != nullptr
        && provider.describe_path != nullptr
        && provider.append_domain_semantic_digest != nullptr
        && provider.append_generation_semantic_digest != nullptr
        && provider.append_runtime_generation_semantic_digest != nullptr
        && provider.build_plan_digest_recipe != nullptr;
}

[[nodiscard]] bool reusable_callbacks_present(
        const ProviderV1& provider) noexcept {
    return provider.map_reusable_source != nullptr
        && provider.decorate_path != nullptr
        && provider.extract_occurrence != nullptr
        && provider.late_fetch_batch != nullptr;
}

[[nodiscard]] std::size_t lower_bound_provider(
        std::uint32_t provider_id) noexcept {
    std::size_t first = 0;
    std::size_t count = g_provider_count;
    while (count != 0) {
        const std::size_t step = count / 2;
        const std::size_t middle = first + step;
        if (g_provider_slots[middle].provider_id < provider_id) {
            first = middle + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }
    return first;
}

void advance_generation() noexcept {
    ++g_provider_generation;
    if (g_provider_generation == 0) {
        g_provider_generation = 1;
    }
}

[[nodiscard]] bool physical_identity_valid(
        const PhysicalIdentityV1* identity) noexcept {
    if (identity == nullptr
        || identity->struct_size < sizeof(PhysicalIdentityV1)
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

[[nodiscard]] bool api_register_provider(
        const ProviderV1* provider) noexcept {
    return register_provider_v1(provider);
}

void api_unregister_provider(std::uint32_t provider_id) noexcept {
    unregister_provider_v1(provider_id);
}

void api_record_relation_write(
        std::uint32_t database_oid,
        std::uint32_t relation_oid,
        const PhysicalIdentityV1* physical_identity) noexcept {
    record_relation_write_v1(database_oid, relation_oid, physical_identity);
}

void bridge_after_provider_rel_paths(
        PlannerInfo* root,
        RelOptInfo* rel,
        unsigned int rti,
        RangeTblEntry* rte,
        const ProviderV1* provider) noexcept {
    after_provider_rel_paths_v1(root, rel, rti, rte, provider);
}

inline constinit PlannerBridgeV1 g_planner_bridge_v1{
    static_cast<std::uint32_t>(sizeof(PlannerBridgeV1)),
    bioseq::cache::kProviderAbiVersion,
    kPostgresMajorVersion,
    &bridge_after_provider_rel_paths,
};

inline constinit CacheApiV1 g_cache_api_v1{
    static_cast<std::uint32_t>(sizeof(CacheApiV1)),
    bioseq::cache::kProviderAbiVersion,
    kPostgresMajorVersion,
    &api_register_provider,
    &api_unregister_provider,
    &api_record_relation_write,
    &g_planner_bridge_v1,
};

} // namespace bioseq::cache::pg::provider_registry::detail

namespace bioseq::cache::pg::provider_registry {

bool validate_provider_v1(const ProviderV1* provider) noexcept {
    if (provider == nullptr
        || provider->struct_size < sizeof(ProviderV1)
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

bool register_provider_v1(const ProviderV1* provider) noexcept {
    if (!validate_provider_v1(provider)) {
        return false;
    }

    const std::size_t position =
        detail::lower_bound_provider(provider->provider_id);
    if (position < detail::g_provider_count
        && detail::g_provider_slots[position].provider_id
            == provider->provider_id) {
        return detail::provider_tables_equal(
            detail::g_provider_slots[position].provider, *provider);
    }

    for (std::size_t index = 0; index < detail::g_provider_count; ++index) {
        const ProviderV1& registered = detail::g_provider_slots[index].provider;
        if (registered.provider_scan_methods == provider->provider_scan_methods
            || detail::provider_names_equal(
                registered.provider_name, provider->provider_name)) {
            return false;
        }
    }

    if (detail::g_provider_count == kMaximumRegisteredProvidersV1) {
        return false;
    }
    for (std::size_t index = detail::g_provider_count;
         index > position; --index) {
        detail::copy_provider_slot(
            detail::g_provider_slots[index],
            detail::g_provider_slots[index - 1]);
    }
    detail::capture_provider(detail::g_provider_slots[position], *provider);
    ++detail::g_provider_count;
    detail::advance_generation();
    return true;
}

void unregister_provider_v1(std::uint32_t provider_id) noexcept {
    if (provider_id == 0 || detail::g_provider_count == 0) {
        return;
    }
    const std::size_t position = detail::lower_bound_provider(provider_id);
    if (position == detail::g_provider_count
        || detail::g_provider_slots[position].provider_id != provider_id) {
        return;
    }
    for (std::size_t index = position + 1;
         index < detail::g_provider_count; ++index) {
        detail::copy_provider_slot(
            detail::g_provider_slots[index - 1],
            detail::g_provider_slots[index]);
    }
    --detail::g_provider_count;
    detail::clear_provider_slot(
        detail::g_provider_slots[detail::g_provider_count]);
    detail::advance_generation();
}

const ProviderV1* find_provider_v1(std::uint32_t provider_id) noexcept {
    if (provider_id == 0 || detail::g_provider_count == 0) {
        return nullptr;
    }
    const std::size_t position = detail::lower_bound_provider(provider_id);
    if (position == detail::g_provider_count
        || detail::g_provider_slots[position].provider_id != provider_id) {
        return nullptr;
    }
    return &detail::g_provider_slots[position].provider;
}

ProviderSnapshotV1 snapshot_providers_v1() noexcept {
    ProviderSnapshotV1 snapshot{};
    snapshot.struct_size = static_cast<std::uint32_t>(sizeof(snapshot));
    snapshot.provider_count =
        static_cast<std::uint32_t>(detail::g_provider_count);
    snapshot.generation = detail::g_provider_generation;
    for (std::size_t index = 0; index < detail::g_provider_count; ++index) {
        snapshot.providers[index] =
            &detail::g_provider_slots[index].provider;
    }
    return snapshot;
}

void install_relation_write_observer_v1(
        RelationWriteObserverV1 observer) noexcept {
    detail::g_relation_write_observer = observer;
}

void record_relation_write_v1(
        std::uint32_t database_oid,
        std::uint32_t relation_oid,
        const PhysicalIdentityV1* physical_identity) noexcept {
    const RelationWriteObserverV1 observer =
        detail::g_relation_write_observer;
    if (observer == nullptr) {
        return;
    }

    const PhysicalIdentityV1* validated_identity =
        detail::physical_identity_valid(physical_identity)
        ? physical_identity
        : nullptr;
    observer(database_oid, relation_oid, validated_identity);
}

void install_planner_bridge_observer_v1(
        PlannerBridgeObserverV1 observer) noexcept {
    detail::g_planner_bridge_observer = observer;
}

void after_provider_rel_paths_v1(
        PlannerInfo* root,
        RelOptInfo* rel,
        unsigned int rti,
        RangeTblEntry* rte,
        const ProviderV1* provider) noexcept {
    const PlannerBridgeObserverV1 observer = detail::g_planner_bridge_observer;
    if (observer == nullptr || root == nullptr || rel == nullptr
        || rti == 0 || rte == nullptr || !validate_provider_v1(provider)) {
        return;
    }
    const ProviderV1* const registered =
        find_provider_v1(provider->provider_id);
    if (registered == nullptr
        || !detail::provider_tables_equal(*registered, *provider)) {
        return;
    }
    observer(root, rel, rti, rte, registered);
}

bool initialize_provider_registry_v1() noexcept {
    void** const api_slot = find_rendezvous_variable(
        kCacheApiRendezvousNameV1);
    void** const bridge_slot = find_rendezvous_variable(
        kPlannerBridgeRendezvousNameV1);
    if (api_slot == nullptr || bridge_slot == nullptr
        || (*api_slot != nullptr
            && *api_slot != static_cast<void*>(&detail::g_cache_api_v1))
        || (*bridge_slot != nullptr
            && *bridge_slot
                != static_cast<void*>(&detail::g_planner_bridge_v1))) {
        return false;
    }

    *bridge_slot = static_cast<void*>(&detail::g_planner_bridge_v1);
    *api_slot = static_cast<void*>(&detail::g_cache_api_v1);
    detail::g_cache_api_rendezvous_slot = api_slot;
    detail::g_planner_bridge_rendezvous_slot = bridge_slot;
    return true;
}

void unpublish_provider_registry_v1() noexcept {
    if (detail::g_cache_api_rendezvous_slot != nullptr
        && *detail::g_cache_api_rendezvous_slot
            == static_cast<void*>(&detail::g_cache_api_v1)) {
        *detail::g_cache_api_rendezvous_slot = nullptr;
    }
    if (detail::g_planner_bridge_rendezvous_slot != nullptr
        && *detail::g_planner_bridge_rendezvous_slot
            == static_cast<void*>(&detail::g_planner_bridge_v1)) {
        *detail::g_planner_bridge_rendezvous_slot = nullptr;
    }
    detail::g_cache_api_rendezvous_slot = nullptr;
    detail::g_planner_bridge_rendezvous_slot = nullptr;
}

const CacheApiV1& cache_api_v1() noexcept {
    return detail::g_cache_api_v1;
}

const PlannerBridgeV1& planner_bridge_v1() noexcept {
    return detail::g_planner_bridge_v1;
}

} // namespace bioseq::cache::pg::provider_registry

#if defined(BIOSEQ_CACHE_TESTING)
extern "C" {

PG_FUNCTION_INFO_V1(bioseq_cache_test_provider_registry_owns_copy);

Datum bioseq_cache_test_provider_registry_owns_copy(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg::provider_registry;

    if (!superuser()) {
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
             errmsg("provider registry copy probe requires superuser")));
    }

    const ProviderSnapshotV1 before = snapshot_providers_v1();
    if (before.provider_count == 0 || before.providers[0] == nullptr
        || find_provider_v1(1U) != nullptr) {
        PG_RETURN_BOOL(false);
    }

    const ProviderV1& source = *before.providers[0];
    const std::uint32_t source_id = source.provider_id;
    const auto source_analyze = source.analyze_query;
    char probe_name[] = "registry-copy-probe";
    ProviderV1 candidate = source;
    candidate.provider_id = 1U;
    candidate.provider_name = probe_name;
    candidate.provider_scan_methods =
        reinterpret_cast<const CustomScanMethods*>(&candidate);
    const auto probe_methods = candidate.provider_scan_methods;

    if (!register_provider_v1(&candidate)) {
        PG_RETURN_BOOL(false);
    }

    probe_name[0] = 'X';
    candidate.provider_id = 2U;
    candidate.provider_name = "mutated";
    candidate.provider_scan_methods = nullptr;
    candidate.analyze_query = nullptr;

    const ProviderV1* const stored = find_provider_v1(1U);
    bool valid = stored != nullptr
        && stored != &candidate
        && stored->provider_id == 1U
        && stored->provider_name != probe_name
        && bioseq::cache::pg::provider_registry::detail::provider_names_equal(
            stored->provider_name, "registry-copy-probe")
        && stored->provider_scan_methods == probe_methods
        && stored->analyze_query == source_analyze
        && validate_provider_v1(stored);

    unregister_provider_v1(1U);
    const ProviderV1* const restored = find_provider_v1(source_id);
    valid = valid
        && find_provider_v1(1U) == nullptr
        && restored != nullptr
        && restored->provider_id == source_id
        && restored->analyze_query == source_analyze
        && snapshot_providers_v1().provider_count == before.provider_count;
    PG_RETURN_BOOL(valid);
}

} // extern "C"
#endif

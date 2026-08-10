/*-------------------------------------------------------------------------
 *
 * provider/registry.cppm
 *     Public backend-local V2 provider registry contract.
 *
 * Provider descriptors and names are copied into stable, never-compacted
 * slots. A removed slot becomes a permanent tombstone for the life of the
 * backend. Registration tokens authenticate synthetic unregister requests;
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

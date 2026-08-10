/*-------------------------------------------------------------------------
 *
 * provider/registry.cppm
 *     Public backend-local provider registry contract.
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

using provider = bioseq::cache::bioseq_cache_provider;
using provider_registration =
    bioseq::cache::bioseq_cache_provider_registration;
using provider_directory =
    bioseq::cache::bioseq_cache_provider_directory;
using physical_identity = bioseq::cache::bioseq_cache_physical_identity;

inline constexpr std::uint32_t k_postgres_major_version =
    static_cast<std::uint32_t>(PG_VERSION_NUM / 10000);
inline constexpr std::size_t maximum_registered_providers = 32;
inline constexpr std::size_t maximum_provider_name_bytes = 128;
inline constexpr std::size_t invalid_provider_slot =
    (std::numeric_limits<std::size_t>::max)();

/** Immutable point-in-time copy of active stable provider pointers. */
struct provider_snapshot {
    std::uint32_t struct_size = sizeof(provider_snapshot);
    std::uint32_t provider_count = 0;
    std::uint64_t generation = 0;
    const provider* providers[maximum_registered_providers]{};
};

/** Execution-scoped lease on one stable active provider slot. */
struct provider_lease {
    std::size_t slot_index = invalid_provider_slot;
    std::uint64_t registry_generation = 0;
    std::uint32_t provider_id = 0;
    std::uint64_t owner_cookie = 0;
};

/** One in-flight callback counted against registry mutation. */
struct provider_callback_token {
    std::size_t slot_index = invalid_provider_slot;
    std::uint64_t registry_generation = 0;
    std::uint64_t owner_cookie = 0;
};

/** A null identity requests conservative/coarse write observation. */
using relation_write_observer = void (*)(
    std::uint32_t database_oid,
    std::uint32_t relation_oid,
    const physical_identity* identity) noexcept;

using planner_bridge_observer = void (*)(
    PlannerInfo* root,
    RelOptInfo* rel,
    unsigned int rti,
    RangeTblEntry* rte,
    const provider* provider_descriptor) noexcept;

/** Cache-owned observer for a provider's fully mutated PlannedStmt. */
using finalized_plan_bridge_observer = void (*)(
    PlannedStmt* statement) noexcept;

[[nodiscard]] bool validate_provider(
    const provider* provider_descriptor) noexcept;
[[nodiscard]] bioseq_cache_status register_provider(
    const provider* provider_descriptor,
    provider_registration* registration) noexcept;
[[nodiscard]] bool unregister_provider(
    const provider_registration* registration) noexcept;
[[nodiscard]] const provider* find_provider(
    std::uint32_t provider_id) noexcept;
[[nodiscard]] provider_snapshot snapshot_providers() noexcept;

[[nodiscard]] bool acquire_provider_lease(
    const provider* provider_descriptor,
    provider_lease* lease) noexcept;
[[nodiscard]] const provider* provider_from_lease(
    const provider_lease& lease) noexcept;
[[nodiscard]] bool validate_provider_lease(
    const provider_lease& lease,
    const provider* expected_provider = nullptr) noexcept;
void release_provider_lease(provider_lease* lease) noexcept;

[[nodiscard]] bool begin_provider_callback(
    const provider* provider_descriptor,
    const provider_lease* lease,
    provider_callback_token* token) noexcept;
void end_provider_callback(provider_callback_token* token) noexcept;

void install_relation_write_observer(
    relation_write_observer observer) noexcept;
void record_relation_write(
    std::uint32_t database_oid,
    std::uint32_t relation_oid,
    const physical_identity* identity) noexcept;

void install_planner_bridge_observer(
    planner_bridge_observer observer) noexcept;
void after_provider_rel_paths(
    PlannerInfo* root,
    RelOptInfo* rel,
    unsigned int rti,
    RangeTblEntry* rte,
    const provider* provider_descriptor) noexcept;
void install_finalized_plan_bridge_observer(
    finalized_plan_bridge_observer observer) noexcept;
void after_planned_statement(PlannedStmt* statement) noexcept;

/** Validate publication ownership and the optional peer before mutation. */
[[nodiscard]] bool preflight_provider_registry() noexcept;

/** Publish the cache API, then request an optional Bioseq directory bind. */
[[nodiscard]] bool initialize_provider_registry() noexcept;

[[nodiscard]] const bioseq::cache::bioseq_cache_api& cache_api() noexcept;
[[nodiscard]] const bioseq::cache::bioseq_cache_planner_bridge&
planner_bridge() noexcept;

#if defined(BIOSEQ_CACHE_TESTING)
/** Test-only copy/token/tombstone probe exposed through generated SQL. */
[[nodiscard]] bool provider_registry_owns_copy();
#endif

static_assert(std::is_standard_layout_v<provider_snapshot>);
static_assert(std::is_trivially_copyable_v<provider_snapshot>);
static_assert(std::is_standard_layout_v<provider_lease>);
static_assert(std::is_trivially_copyable_v<provider_lease>);
static_assert(std::is_standard_layout_v<provider_callback_token>);
static_assert(std::is_trivially_copyable_v<provider_callback_token>);

} // namespace bioseq::cache::pg::provider_registry

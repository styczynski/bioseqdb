/**
 * @file
 * @brief Provider registration within one PostgreSQL backend.
 *
 * The registry copies provider descriptions and names into slots whose addresses
 * stay  fixed. Removed slots are never reused. Unregistering requires the matching
 * registration token and no active callbacks or execution references.
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

/** Snapshot of pointers to the providers active when it was taken. */
struct provider_snapshot {
    std::uint32_t struct_size = sizeof(provider_snapshot);
    std::uint32_t provider_count = 0;
    std::uint64_t generation = 0;
    const provider* providers[maximum_registered_providers]{};
};

/** Holds a provider registrution in use so it cannot be removed during execution. */
struct provider_lease {
    std::size_t slot_index = invalid_provider_slot;
    std::uint64_t registry_generation = 0;
    std::uint32_t provider_id = 0;
    std::uint64_t owner_cookie = 0;
};

/** Identifies an active callback that prevents provider registration changes. */
struct provider_callback_token {
    std::size_t slot_index = invalid_provider_slot;
    std::uint64_t registry_generation = 0;
    std::uint64_t owner_cookie = 0;
};

/** A null physical identity requests broader write invalidation. */
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

/** Cache callback invoked after a provider finishes changing the planned statement */
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

/** Check that the  cache API slot is available and inspect the optional provider directory. */
[[nodiscard]] bool preflight_provider_registry() noexcept;

/** Publish, the cache API and ask a compatible provider directory to connect to it. */
[[nodiscard]] bool initialize_provider_registry() noexcept;

[[nodiscard]] const bioseq::cache::bioseq_cache_api& cache_api() noexcept;
[[nodiscard]] const bioseq::cache::bioseq_cache_planner_bridge&
planner_bridge() noexcept;

#if defined(BIOSEQ_CACHE_TESTING)
/** Test that registration copies provider data and removal leaves existing providers intact. */
[[nodiscard]] bool provider_registry_owns_copy();
#endif

static_assert(std::is_standard_layout_v<provider_snapshot>);
static_assert(std::is_trivially_copyable_v<provider_snapshot>);
static_assert(std::is_standard_layout_v<provider_lease>);
static_assert(std::is_trivially_copyable_v<provider_lease>);
static_assert(std::is_standard_layout_v<provider_callback_token>);
static_assert(std::is_trivially_copyable_v<provider_callback_token>);

} // namespace bioseq::cache::pg::provider_registry

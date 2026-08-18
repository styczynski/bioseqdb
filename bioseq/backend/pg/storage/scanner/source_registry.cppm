/**
 * @file scanner/source_registry.cppm
 * @brief Transaction-scoped source registry and domain ownership.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "access/xact.h"
#include "access/relation.h"
#include "access/table.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

export module bioseq.postgres.storage.scanner:source_registry;

import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.configuration.core;
import bioseq.postgres.support;
import bioseq.common.seqan_adapter;              // seq::dna4 in static_assert below
import bioseq.postgres.storage.manager;     // seq_caches + open_sub_table_for_locator
import bioseq.storage.scanner;              // scanner concepts
import bioseq.storage.lazy_sequence;        // lazy_sequence + with_postgres_lazy_sequence
import bioseq.storage.coordinates;
import bioseq.storage.layout;
import bioseq.postgres.storage.physical;
import bioseq.storage.size;        // sequence_size_t
import bioseq.postgres.storage.metadata;    // stripe_metadata/find_stripe_by_row_number
import bioseq.sequences;           // compact_sequence_range, compact_sequence_stdc_instance
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.table.io;            // begin_column_read/read_column_row_by_number
import bioseq.parallelism.pool;

export namespace bioseq::storage {

// ---------------------------------------------------------------------------
// Shared PostgreSQL source registry and transaction-scoped source domains.
// ---------------------------------------------------------------------------

/// Ownership boundary that permits reclaiming unused source domains.
enum class pg_sequence_source_sweep_boundary : std::uint8_t {
    /// Base provider has ended.
    provider_end,
    /// Upper executor owner released a route lease.
    upper_lease_end,
    /// Executor rescan may reuse an unchanged binding.
    rescan,
    /// Standalone index owner has ended.
    standalone_end,
    /// Top-level transaction cleanup is running.
    transaction_end,
    /// Creating subtransaction is ending.
    subtransaction_end,
};

/// Executor owner that created a source provider.
enum class pg_sequence_source_provider_kind : std::uint8_t {
    /// Provider follows a table scan lifecycle.
    scan,
    /// Provider follows standalone index administration.
    standalone_index,
};

namespace sequence_source_detail {
struct pg_sequence_source_domain;
}

/**
 * Transaction-owned registry for one verified relation/storage/snapshot flow.
 * It contains no Relation pointer and is never shared through DSM.
 */
struct pg_sequence_source_registry {
    /// Backend thread on which every registry operation must run.
    pg_sequence_backend_thread_token backend_thread;
    /// User-visible parent relation OID.
    Oid user_relid = InvalidOid;
    /// Database component of the parent physical identity.
    Oid database_oid = InvalidOid;
    /// Tablespace component of the parent physical identity.
    Oid tablespace_oid = InvalidOid;
    /// File component of the parent physical identity.
    Oid main_relfilenode = InvalidOid;
    /// Storage-generation identity authenticated when the provider is created.
    std::uint64_t main_storage_id = 0u;
    /* Bioseq's metapage storage id is the immutable binding generation.  Keep
     * the named copy in the registry key so a rel/attno pair can never reuse a
     * domain after its physical binding has been replaced. */
    std::uint64_t binding_generation = 0u;
    /// Borrowed registered MVCC snapshot governing source-catalog lookup.
    Snapshot authoritative_snapshot = nullptr;
    /// Subtransaction that created this registry.
    SubTransactionId creating_subid = InvalidSubTransactionId;
    /// Backend-local generation authenticating leases.
    std::uint64_t generation = 0u;
    /// Registry-owned linked list of source domains.
    sequence_source_detail::pg_sequence_source_domain* domains = nullptr;
    /// Link in the backend-local registry list.
    pg_sequence_source_registry* next = nullptr;
    /// Number of provider and upper-owner lease tokens.
    std::uint32_t active_leases = 0u;
    /// Whether authenticated leases may resolve new source lookups.
    bool accepting_lookups = false;
    /// Whether the base provider has released its token.
    bool provider_closed = false;
    /// Sticky lease-accounting failure.
    bool lease_fault = false;
    /// Whether top-level transaction cleanup detached the registry.
    bool xact_ended = false;
};

/** A checked upper/provider retain on a registry. */
struct pg_sequence_source_registry_lease {
    /// Borrowed registry control while `owned` is true.
    pg_sequence_source_registry* registry = nullptr;
    /// Generation that must match the current registry generation.
    std::uint64_t provider_generation = 0u;
    /// Whether this object owns one active-lease count.
    bool owned = false;
};

/** Provider control owned by a base scan or standalone index owner. */
struct pg_sequence_source_provider {
    /// Provider-owned initial registry lease.
    pg_sequence_source_registry_lease lease{};
    /// Link in the backend-local provider list.
    pg_sequence_source_provider* next = nullptr;
    /// Executor lifecycle that owns this provider.
    pg_sequence_source_provider_kind kind =
        pg_sequence_source_provider_kind::scan;
    /// Whether provider operations may still use `lease`.
    bool open = false;
};

/** One source-provider route retained by an upper executor owner. */
struct pg_sequence_source_route {
    /// Parent relation axis of the route key.
    Oid relation_oid = InvalidOid;
    /// Independently retained registry lease.
    pg_sequence_source_registry_lease lease{};
};

static_assert(std::is_trivially_copyable_v<pg_sequence_source_route>);

/** Query-lifetime routing for joins, appends and nested upper wrappers.
 *
 * A PostgreSQL self join has independent registries even when every binding
 * names the same physical relation. The provider generation is therefore the
 * route key. `relation_oid` supplies the physical-relation axis. Two routes stay inline
 * for the common binary-join case. Larger legal plans grow in the executor's
 * query MemoryContext, before any new lease is retained.
 */
struct pg_sequence_source_route_set {
    /// Inline storage for the common one- or two-route plan.
    std::array<pg_sequence_source_route, 2u> inline_routes{};
    /// Memory-context-owned storage used after inline capacity is exceeded.
    pg_sequence_source_route* dynamic_routes = nullptr;
    /// Number of entries available in `dynamic_routes`.
    std::size_t dynamic_capacity = 0u;
    /// Number of live route entries.
    std::size_t count = 0u;
    /// Query-lifetime context owning dynamic storage.
    MemoryContext allocation_context = nullptr;
};

#ifdef BIOSEQ_TEST_INDEXES
/// One-shot failure sites covering source-domain construction and cleanup.
enum class sequence_source_failure : std::uint8_t {
  none,
  provider_after_registry_control,
  provider_after_provider_control,
  domain_after_control,
  domain_after_context,
  domain_after_owner,
  domain_after_snapshot,
  domain_after_caches,
  close_before_caches,
  close_before_snapshot,
  close_before_before_locks,
  close_before_locks,
  close_before_after_locks,
  read_before_scanner,
  mmsearch_output_after_promotion,
};

/// Test-only counters for provider, registry, domain and facade lifetimes.
struct sequence_source_stats {
  std::uint64_t registries_created = 0u;
  std::uint64_t registries_destroyed = 0u;
  std::uint64_t providers_created = 0u;
  std::uint64_t providers_destroyed = 0u;
  std::uint64_t scan_providers_created = 0u;
  std::uint64_t standalone_providers_created = 0u;
  std::uint64_t domains_created = 0u;
  std::uint64_t domains_reused = 0u;
  std::uint64_t domains_destroyed = 0u;
  std::uint64_t cache_sets_created = 0u;
  std::uint64_t cache_sets_destroyed = 0u;
  std::uint64_t snapshots_registered = 0u;
  std::uint64_t snapshots_unregistered = 0u;
  std::uint64_t scanner_entries_created = 0u;
  std::uint64_t scanner_entry_hits = 0u;
  std::uint64_t leases_acquired = 0u;
  std::uint64_t leases_released = 0u;
  std::uint64_t provider_closed_with_upper_leases = 0u;
  std::uint64_t last_upper_lease_after_provider_end = 0u;
  std::uint64_t cleanup_commit = 0u;
  std::uint64_t cleanup_abort = 0u;
  std::uint64_t sweeps = 0u;
  std::uint64_t swept_domains = 0u;
  std::uint64_t subxact_promotions = 0u;
  std::uint64_t subxact_aborts = 0u;
  std::uint64_t pre_prepare_rejections = 0u;
  std::uint64_t release_before_locks = 0u;
  std::uint64_t release_locks = 0u;
  std::uint64_t release_after_locks = 0u;
  std::uint64_t wrong_thread_rejections = 0u;
  std::uint64_t refcount_faults = 0u;
  std::uint64_t lease_faults = 0u;
  std::uint64_t fault_injections = 0u;
  std::uint64_t read_fault_injections = 0u;
  std::uint64_t mmsearch_output_context_resets = 0u;
  std::uint64_t mmsearch_output_locator_promotions = 0u;
  std::uint64_t mmsearch_output_pg_error_restores = 0u;
  std::uint64_t mmsearch_output_restore_mismatches = 0u;
  std::uint64_t facade_source_stream_callbacks = 0u;
  std::uint64_t facade_source_resident_probes = 0u;
  std::uint64_t facade_declared_materializations = 0u;
  std::uint64_t facade_completed_materializations = 0u;
  std::uint64_t facade_poison_hits = 0u;
  std::uint64_t live_registries = 0u;
  std::uint64_t live_providers = 0u;
  std::uint64_t live_domains = 0u;
  std::uint64_t live_leases = 0u;
  std::uint64_t live_cache_sets = 0u;
  std::uint64_t live_snapshots = 0u;
};

inline thread_local sequence_source_failure g_pg_sequence_source_failure =
    sequence_source_failure::none;
inline thread_local sequence_source_stats
    g_pg_sequence_source_stats{};

/// Arm one source-domain failure site for the next matching operation.
/// @param point One-shot failure site, or `none` to disable injection.
inline void
arm_pg_sequence_source_failure(sequence_source_failure point) noexcept {
  g_pg_sequence_source_failure = point;
}

/// Clear source-domain and facade failure injection state.
inline void disarm_pg_sequence_source_failure() noexcept {
  g_pg_sequence_source_failure = sequence_source_failure::none;
  disarm_pg_sequence_facade_test_poison();
}

/// Return a snapshot of source-domain lifetime counters.
/// @return Stored counters combined with current live-object cardinalities.
[[nodiscard]] inline sequence_source_stats
pg_sequence_source_stats() noexcept;

/// Reset source-domain and facade counters for an isolated test scenario.
inline void reset_pg_sequence_source_stats() noexcept {
  g_pg_sequence_source_stats = {};
  reset_pg_sequence_facade_test_counters();
}
#endif

namespace sequence_source_detail {

/// Source-domain construction and cleanup state.
enum class domain_lifecycle : std::uint8_t {
  constructing,
  live,
  cleaning,
  faulted,
  invalid,
  released,
};

/// ResourceOwner release stages already completed for one domain.
enum class owner_release_stage_bits : std::uint8_t {
    none = 0u,
    before_locks = 1u,
    locks = 2u,
    after_locks = 4u,
};

/// Convert one release stage into its stored bit mask.
/// @param stage ResourceOwner release stage.
/// @return Underlying single-bit mask.
[[nodiscard]] constexpr std::uint8_t stage_bit(
        owner_release_stage_bits stage) noexcept {
    return static_cast<std::uint8_t>(stage);
}

/// Transaction-owned source domain with a registered snapshot, read cache and
/// staged ResourceOwner cleanup state.
struct pg_sequence_source_domain {
    /// Backend thread on which reads and cleanup must run.
    pg_sequence_backend_thread_token backend_thread;
    /// Borrowed registry while the registry token is owned.
    pg_sequence_source_registry* registry = nullptr;
    /// Link in the owning registry's domain list.
    pg_sequence_source_domain* next = nullptr;
    /// Transaction-child context owning scanners and read-cache allocation.
    MemoryContext payload_context = nullptr;
    /// Transaction-child owner for snapshot, relation, and lock resources.
    ResourceOwner resource_owner = nullptr;
    /// MVCC snapshot registered on `resource_owner`.
    Snapshot registered_snapshot = nullptr;
    /// Read-side fragment cache owned by `payload_context` and `resource_owner`.
    seq_caches* read_caches = nullptr;
    /// One-based parent sequence attribute number.
    AttrNumber column_attno = InvalidAttrNumber;
    /// Persisted alphabet and packed-unit width.
    compact_sequence_stdc_type_params stored_type{};
    /// Physical storage generation authenticated by the registry.
    std::uint64_t binding_generation = 0u;
    /// Subtransaction that created this domain.
    SubTransactionId creating_subid = InvalidSubTransactionId;
    /// Backend-local identity generation.
    std::uint64_t generation = 0u;
    /// Registry token plus retained expanded-value and scanner references.
    std::uint32_t refcount = 0u;
    /// Construction and cleanup lifecycle.
    domain_lifecycle lifecycle = domain_lifecycle::constructing;
    /// ResourceOwner release phases already attempted.
    std::uint8_t released_owner_stages = 0u;
    /// Whether the registry owns one reference count.
    bool registry_token_owned = false;
    /// Hint that only the registry token remains.
    bool reclaimable_hint = false;
    /// Sticky reference-count failure that prevents further reads.
    bool refcount_fault = false;
};

/// Type-erased scanner cached by one source-read session.
struct source_scanner_entry {
    /// Retained domain supplying snapshot and fragment caches.
    pg_sequence_source_domain* domain = nullptr;
    /// Representation-specific scanner owned by the entry context.
    void* concrete_scanner = nullptr;
    /// Type-erased logical-rank read callback.
    void (*read_ranks)(source_scanner_entry&, sequence_transform,
                       std::uint64_t, std::span<std::uint8_t>) = nullptr;
    /// Checked successful-finalization callback.
    void (*finish)(source_scanner_entry&) = nullptr;
    /// Failure cleanup callback that does not propagate errors.
    void (*abort_noexcept)(source_scanner_entry&) noexcept = nullptr;
    /// Whether the concrete scanner's constructor completed.
    bool constructed = false;
    /// Whether this entry owns one domain retain.
    bool domain_retained = false;
};

/// Validated domain and locator input used to construct a scanner entry.
struct source_scanner_factory_argument {
    /// Domain borrowed for construction and retained by the resulting entry.
    pg_sequence_source_domain* domain = nullptr;
    /// Locator that selects the physical fragment range.
    sequence_locator locator{};
    /// Representation used for scanner dispatch.
    compact_sequence_stdc_type_params stored_type{};
};

inline pg_sequence_source_registry *registries = nullptr;
inline pg_sequence_source_provider *providers = nullptr;
inline std::uint64_t next_generation = 1u;
inline bool callbacks_registered = false;

#ifdef BIOSEQ_TEST_INDEXES
/// Combine stored counters with the currently live registry graph.
/// @return Counter snapshot for the current backend thread.
[[nodiscard]] inline sequence_source_stats
collect_pg_sequence_source_stats() noexcept {
  sequence_source_stats result = g_pg_sequence_source_stats;
  const sequence_facade_test_counters facade =
      pg_sequence_facade_test_counters();
  result.facade_source_stream_callbacks = facade.source_stream_callbacks;
  result.facade_source_resident_probes = facade.source_resident_probes;
  result.facade_declared_materializations = facade.declared_materializations;
  result.facade_completed_materializations = facade.completed_materializations;
  result.facade_poison_hits = facade.poison_hits;
  result.live_registries = 0u;
  result.live_providers = 0u;
  result.live_domains = 0u;
  result.live_leases = 0u;
  result.live_cache_sets = 0u;
  result.live_snapshots = 0u;
  for (auto *registry = registries; registry != nullptr;
       registry = registry->next) {
    ++result.live_registries;
    result.live_leases += registry->active_leases;
    for (auto *domain = registry->domains; domain != nullptr;
         domain = domain->next) {
      ++result.live_domains;
      if (domain->read_caches != nullptr) {
        ++result.live_cache_sets;
      }
      if (domain->registered_snapshot != nullptr) {
        ++result.live_snapshots;
      }
    }
  }
  for (auto *provider = providers; provider != nullptr;
       provider = provider->next) {
    ++result.live_providers;
  }
  return result;
}
#endif

#ifdef BIOSEQ_TEST_INDEXES
/// Raise the armed source-domain failure at its exact PostgreSQL leaf.
/// @param point Failure site being entered.
/// @throws PostgreSQL ERROR when `point` is armed.
inline void inject_pg_sequence_source_failure(sequence_source_failure point) {
    if (g_pg_sequence_source_failure != point) {
        return;
    }
    g_pg_sequence_source_failure = sequence_source_failure::none;
    ++g_pg_sequence_source_stats.fault_injections;
    if (point == sequence_source_failure::read_before_scanner) {
        ++g_pg_sequence_source_stats.read_fault_injections;
    }
    ereport(ERROR,
        (errcode(ERRCODE_INTERNAL_ERROR),
         errmsg("injected PostgreSQL sequence source-domain failure"),
         errdetail("failure point %u", static_cast<unsigned>(point))));
}
#endif

/// Compare every canonical locator field as one fixed-layout value.
/// @return Whether all canonical locator bytes are equal.
[[nodiscard]] inline bool same_locator(
        const sequence_locator& left, const sequence_locator& right) noexcept {
    return std::memcmp(&left, &right, sizeof(sequence_locator)) == 0;
}

/// Allocate a nonzero backend-local generation for registries and domains.
/// @return Unique generation below the unsigned 64-bit maximum.
/// @throws PostgreSQL ERROR when generation space is exhausted.
[[nodiscard]] inline std::uint64_t allocate_generation() {
    if (next_generation == 0u
        || next_generation == std::numeric_limits<std::uint64_t>::max()) {
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("bioseq source-domain generation space exhausted")));
    }
    return next_generation++;
}

/// Check that a source domain retains every resource required for reads.
/// @param domain Domain whose lifecycle and owned resources are inspected.
/// @return Whether reads may enter the domain.
[[nodiscard]] inline bool domain_is_readable(
        const pg_sequence_source_domain& domain) noexcept {
    return domain.lifecycle == domain_lifecycle::live
        && !domain.refcount_fault
        && domain.registry_token_owned
        && domain.registered_snapshot != nullptr
        && domain.resource_owner != nullptr
        && domain.read_caches != nullptr;
}

/// Retain a live source domain through the type-erased backend contract.
/// @param opaque Domain pointer, or null for no work.
///
/// Overflow faults the domain instead of acquiring a reference.
inline void retain_domain(void* opaque) noexcept {
    auto* domain = static_cast<pg_sequence_source_domain*>(opaque);
    if (domain == nullptr) {
        return;
    }
    if (domain->refcount == 0u
        || domain->refcount == std::numeric_limits<std::uint32_t>::max()) {
        domain->refcount_fault = true;
        domain->lifecycle = domain_lifecycle::faulted;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.refcount_faults;
#endif
        return;
    }
    ++domain->refcount;
}

/// Release a source-domain reference and destroy only a fully detached domain.
/// @param opaque Domain pointer, or null for no work.
///
/// The final reference may free the control object only after registry
/// ownership has been detached and cleanup marked it invalid or released.
inline void release_domain(void* opaque) noexcept {
    auto* domain = static_cast<pg_sequence_source_domain*>(opaque);
    if (domain == nullptr) {
        return;
    }
    if (domain->refcount_fault || domain->refcount == 0u) {
        domain->refcount_fault = true;
        domain->lifecycle = domain_lifecycle::faulted;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.refcount_faults;
#endif
        return;
    }
    const std::uint32_t remaining = --domain->refcount;
    if (remaining == 1u && domain->registry_token_owned) {
        domain->reclaimable_hint = true;
        return;
    }
    if (remaining != 0u) {
        return;
    }
    if (domain->registry_token_owned
        || (domain->lifecycle != domain_lifecycle::invalid
            && domain->lifecycle != domain_lifecycle::released)) {
        domain->refcount_fault = true;
        domain->lifecycle = domain_lifecycle::faulted;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.refcount_faults;
#endif
        return;
    }
    domain->lifecycle = domain_lifecycle::released;
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_stats.domains_destroyed;
#endif
    domain->backend_thread.~pg_sequence_backend_thread_token();
    pfree(domain);
}

/// Return the authenticated control address and generation for a domain.
/// @param opaque Domain pointer, or null.
/// @return Empty identity for null, otherwise control address and generation.
[[nodiscard]] inline pg_sequence_source_identity domain_identity(
        const void* opaque) noexcept {
    auto const* domain = static_cast<const pg_sequence_source_domain*>(opaque);
    return domain == nullptr ? pg_sequence_source_identity{}
        : pg_sequence_source_identity{
            .control_address = reinterpret_cast<std::uintptr_t>(domain),
            .generation = domain->generation,
        };
}

/// Report that PostgreSQL source domains never cross backend threads.
/// @return Always `false`.
[[nodiscard]] inline bool domain_worker_shareable(const void*) noexcept {
    return false;
}

/// Match one readable domain against its column, packed type and binding.
/// @param domain Candidate registry-owned domain.
/// @param column_attno One-based sequence attribute number.
/// @param stored_type Persisted alphabet and packed-unit width.
/// @return Whether the domain is readable and has the exact binding key.
[[nodiscard]] inline bool domain_key_matches(
        const pg_sequence_source_domain& domain,
        AttrNumber column_attno,
        compact_sequence_stdc_type_params stored_type) noexcept {
    return domain_is_readable(domain)
        && domain.column_attno == column_attno
        && domain.stored_type == stored_type
        && domain.binding_generation != 0u
        && domain.registry != nullptr
        && domain.binding_generation
            == domain.registry->binding_generation;
}

/// Validate lease ownership, generation, lookup state and backend thread.
/// @param lease Candidate provider or upper-owner lease.
/// @return Whether this thread may use it for source lookup.
[[nodiscard]] inline bool registry_lease_is_live(
        const pg_sequence_source_registry_lease& lease) noexcept {
    return lease.owned && lease.registry != nullptr
        && !lease.registry->lease_fault
        && lease.provider_generation == lease.registry->generation
        && lease.registry->accepting_lookups
        && lease.registry->backend_thread.is_current();
}

} // namespace sequence_source_detail

#ifdef BIOSEQ_TEST_INDEXES
/// Return source-domain counters with current live-object cardinalities.
[[nodiscard]] inline sequence_source_stats
pg_sequence_source_stats() noexcept {
    return sequence_source_detail::collect_pg_sequence_source_stats();
}
#endif

/* Output-lifetime diagnostics share the source-domain test counter surface.
 * Production callers retain an unconditional, inlinable no-op. */
/// Record an output memory-context reset in test builds.
inline void note_mmsearch_output_context_reset_for_test() noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_stats
        .mmsearch_output_context_resets;
#endif
}

/// Record whether an output value required locator promotion.
/// @param promoted_locator Whether promotion occurred.
inline void note_mmsearch_output_locator_promotion_for_test(
        const bool promoted_locator) noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    if (promoted_locator) {
        ++g_pg_sequence_source_stats
            .mmsearch_output_locator_promotions;
    }
#else
    (void)promoted_locator;
#endif
}

/// Record whether output state was restored after a PostgreSQL error.
/// @param state_restored Whether the output owner returned to its prior state.
inline void note_mmsearch_output_pg_error_restore_for_test(
        const bool state_restored) noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_stats
        .mmsearch_output_pg_error_restores;
    if (!state_restored) {
        ++g_pg_sequence_source_stats
            .mmsearch_output_restore_mismatches;
    }
#else
    (void)state_restored;
#endif
}

/// Raise the armed failure immediately after output locator promotion.
/// @throws PostgreSQL ERROR when the test failure site is armed.
inline void inject_mmsearch_output_after_promotion_failure_for_test() {
#ifdef BIOSEQ_TEST_INDEXES
    sequence_source_detail::inject_pg_sequence_source_failure(
        sequence_source_failure::mmsearch_output_after_promotion);
#endif
}

namespace sequence_source_detail {

/// Close and destroy read caches during abort without propagating ERROR.
/// @param caches Owned cache pointer cleared on every path.
inline void abort_close_caches_noerror(seq_caches*& caches) noexcept {
    if (caches == nullptr) {
        return;
    }
    PG_TRY();
    {
        close_and_destroy_seq_caches(caches, false);
        caches = nullptr;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.cache_sets_destroyed;
#endif
    }
    PG_CATCH();
    {
        FlushErrorState();
        if (caches != nullptr) {
            destroy_seq_caches(caches);
            caches = nullptr;
#ifdef BIOSEQ_TEST_INDEXES
            ++g_pg_sequence_source_stats.cache_sets_destroyed;
#endif
        }
    }
    PG_END_TRY();
}

/// Unregister a domain snapshot under its ResourceOwner during abort.
/// @param domain Domain whose registered snapshot pointer is cleared.
inline void abort_unregister_snapshot_noerror(
        pg_sequence_source_domain& domain) noexcept {
    if (domain.registered_snapshot == nullptr
        || domain.resource_owner == nullptr) {
        domain.registered_snapshot = nullptr;
        return;
    }
    const ResourceOwner saved_owner = CurrentResourceOwner;
    PG_TRY();
    {
        CurrentResourceOwner = domain.resource_owner;
        UnregisterSnapshotFromOwner(
            domain.registered_snapshot, domain.resource_owner);
        domain.registered_snapshot = nullptr;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.snapshots_unregistered;
#endif
        CurrentResourceOwner = saved_owner;
    }
    PG_CATCH();
    {
        CurrentResourceOwner = saved_owner;
        FlushErrorState();
        /* The child ResourceOwner releases any still-registered snapshot. */
        domain.registered_snapshot = nullptr;
    }
    PG_END_TRY();
}

/// Run one abort-mode ResourceOwner release stage at most once.
/// @param domain Domain owning the child `ResourceOwner`.
/// @param postgres_stage PostgreSQL release phase to execute.
/// @param stage_flag Stored bit marking the attempted phase.
inline void abort_release_owner_stage_noerror(
        pg_sequence_source_domain& domain,
        ResourceReleasePhase postgres_stage,
        owner_release_stage_bits stage_flag) noexcept {
    if (domain.resource_owner == nullptr
        || (domain.released_owner_stages & stage_bit(stage_flag)) != 0u) {
        return;
    }
#ifdef BIOSEQ_TEST_INDEXES
    switch (stage_flag) {
        case owner_release_stage_bits::before_locks:
            ++g_pg_sequence_source_stats.release_before_locks;
            break;
        case owner_release_stage_bits::locks:
            ++g_pg_sequence_source_stats.release_locks;
            break;
        case owner_release_stage_bits::after_locks:
            ++g_pg_sequence_source_stats.release_after_locks;
            break;
        case owner_release_stage_bits::none:
            Assert(false);
            break;
    }
#endif
    PG_TRY();
    {
        ResourceOwnerRelease(
            domain.resource_owner, postgres_stage, false, false);
        domain.released_owner_stages |= stage_bit(stage_flag);
    }
    PG_CATCH();
    {
        FlushErrorState();
        /* Mark the attempted cleanup stage. PostgreSQL's parent abort cleanup is
         * the final owner of a resource whose release callback itself failed. */
        domain.released_owner_stages |= stage_bit(stage_flag);
    }
    PG_END_TRY();
}

/// Delete a fully released child ResourceOwner during abort cleanup.
/// @param domain Domain whose owner pointer is cleared before deletion.
inline void abort_delete_owner_noerror(
        pg_sequence_source_domain& domain) noexcept {
    if (domain.resource_owner == nullptr) {
        return;
    }
    ResourceOwner owner = domain.resource_owner;
    domain.resource_owner = nullptr;
    PG_TRY();
    {
        ResourceOwnerDelete(owner);
    }
    PG_CATCH();
    {
        FlushErrorState();
    }
    PG_END_TRY();
}

/// Delete a domain payload context during abort without propagating ERROR.
/// @param domain Domain whose payload-context pointer is cleared before deletion.
inline void abort_delete_payload_noerror(
        pg_sequence_source_domain& domain) noexcept {
    if (domain.payload_context == nullptr) {
        return;
    }
    MemoryContext payload = domain.payload_context;
    domain.payload_context = nullptr;
    PG_TRY();
    {
        MemoryContextDelete(payload);
    }
    PG_CATCH();
    {
        FlushErrorState();
    }
    PG_END_TRY();
}

/// Complete every remaining abort cleanup step for one source domain.
/// @param domain Domain made invalid after caches, snapshot, owner, and payload
/// context have received abort cleanup.
inline void abort_cleanup_domain_noerror(
        pg_sequence_source_domain& domain) noexcept {
    if (domain.lifecycle == domain_lifecycle::invalid
        || domain.lifecycle == domain_lifecycle::released) {
        return;
    }
    domain.lifecycle = domain_lifecycle::cleaning;
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_stats.cleanup_abort;
#endif
    abort_close_caches_noerror(domain.read_caches);
    abort_unregister_snapshot_noerror(domain);
    abort_release_owner_stage_noerror(
        domain, RESOURCE_RELEASE_BEFORE_LOCKS,
        owner_release_stage_bits::before_locks);
    abort_release_owner_stage_noerror(
        domain, RESOURCE_RELEASE_LOCKS,
        owner_release_stage_bits::locks);
    abort_release_owner_stage_noerror(
        domain, RESOURCE_RELEASE_AFTER_LOCKS,
        owner_release_stage_bits::after_locks);
    abort_delete_owner_noerror(domain);
    abort_delete_payload_noerror(domain);
    domain.lifecycle = domain_lifecycle::invalid;
}

/// Close one source domain at a success or abort sweep boundary.
/// @param domain Domain whose resources and locks are released in PostgreSQL's
/// `BEFORE_LOCKS`, `LOCKS`, and `AFTER_LOCKS` order.
/// @param is_commit Whether ResourceOwner callbacks receive commit semantics.
/// @throws PostgreSQL ERROR when cache, snapshot, ResourceOwner, or context
/// cleanup fails.
inline void cleanup_domain(
        pg_sequence_source_domain& domain, bool is_commit) {
    if (domain.lifecycle == domain_lifecycle::invalid
        || domain.lifecycle == domain_lifecycle::released) {
        return;
    }
    domain.lifecycle = domain_lifecycle::cleaning;
#ifdef BIOSEQ_TEST_INDEXES
    if (is_commit) {
        ++g_pg_sequence_source_stats.cleanup_commit;
    } else {
        ++g_pg_sequence_source_stats.cleanup_abort;
    }
#endif
    ResourceOwner owner = domain.resource_owner;
    const ResourceOwner saved_owner = CurrentResourceOwner;
    PG_TRY();
    {
        CurrentResourceOwner = owner;
        if (domain.read_caches != nullptr) {
#ifdef BIOSEQ_TEST_INDEXES
            inject_pg_sequence_source_failure(
                sequence_source_failure::close_before_caches);
#endif
            close_and_destroy_seq_caches(domain.read_caches, is_commit);
            domain.read_caches = nullptr;
#ifdef BIOSEQ_TEST_INDEXES
            ++g_pg_sequence_source_stats.cache_sets_destroyed;
#endif
        }
        if (domain.registered_snapshot != nullptr) {
#ifdef BIOSEQ_TEST_INDEXES
            inject_pg_sequence_source_failure(
                sequence_source_failure::close_before_snapshot);
#endif
            UnregisterSnapshotFromOwner(
                domain.registered_snapshot, owner);
            domain.registered_snapshot = nullptr;
#ifdef BIOSEQ_TEST_INDEXES
            ++g_pg_sequence_source_stats.snapshots_unregistered;
#endif
        }
        CurrentResourceOwner = saved_owner;
    }
    PG_CATCH();
    {
        CurrentResourceOwner = saved_owner;
        PG_RE_THROW();
    }
    PG_END_TRY();

    if ((domain.released_owner_stages
            & stage_bit(owner_release_stage_bits::before_locks)) == 0u) {
#ifdef BIOSEQ_TEST_INDEXES
        inject_pg_sequence_source_failure(
            sequence_source_failure::close_before_before_locks);
#endif
        ResourceOwnerRelease(
            owner, RESOURCE_RELEASE_BEFORE_LOCKS, is_commit, false);
        domain.released_owner_stages |=
            stage_bit(owner_release_stage_bits::before_locks);
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.release_before_locks;
#endif
    }
    if ((domain.released_owner_stages
            & stage_bit(owner_release_stage_bits::locks)) == 0u) {
#ifdef BIOSEQ_TEST_INDEXES
        inject_pg_sequence_source_failure(
            sequence_source_failure::close_before_locks);
#endif
        ResourceOwnerRelease(
            owner, RESOURCE_RELEASE_LOCKS, is_commit, false);
        domain.released_owner_stages |=
            stage_bit(owner_release_stage_bits::locks);
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.release_locks;
#endif
    }
    if ((domain.released_owner_stages
            & stage_bit(owner_release_stage_bits::after_locks)) == 0u) {
#ifdef BIOSEQ_TEST_INDEXES
        inject_pg_sequence_source_failure(
            sequence_source_failure::close_before_after_locks);
#endif
        ResourceOwnerRelease(
            owner, RESOURCE_RELEASE_AFTER_LOCKS, is_commit, false);
        domain.released_owner_stages |=
            stage_bit(owner_release_stage_bits::after_locks);
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.release_after_locks;
#endif
    }
    ResourceOwnerDelete(owner);
    domain.resource_owner = nullptr;
    MemoryContextDelete(domain.payload_context);
    domain.payload_context = nullptr;
    domain.lifecycle = domain_lifecycle::invalid;
}

/// Remove one domain from its registry without changing its reference count.
/// @param registry Registry whose intrusive list owns the link.
/// @param domain Domain detached when present.
inline void unlink_domain(
        pg_sequence_source_registry& registry,
        pg_sequence_source_domain& domain) noexcept {
    auto** link = &registry.domains;
    while (*link != nullptr) {
        if (*link == &domain) {
            *link = domain.next;
            domain.next = nullptr;
            return;
        }
        link = &(*link)->next;
    }
}

/// Drop the registry-owned domain reference after unlinking.
/// @param domain Detached domain whose registry token is consumed.
inline void drop_registry_token_noexcept(
        pg_sequence_source_domain& domain) noexcept {
    if (!domain.registry_token_owned) {
        domain.refcount_fault = true;
        domain.lifecycle = domain_lifecycle::faulted;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.refcount_faults;
#endif
        return;
    }
    domain.registry_token_owned = false;
    domain.registry = nullptr;
    domain.reclaimable_hint = false;
    release_domain(&domain);
}

/// Reclaim every eligible domain at one explicit ownership boundary.
/// @param registry Registry whose unretained domains may be removed.
/// @param boundary Ownership boundary controlling rescan reuse.
/// @param is_commit Whether checked cleanup receives commit semantics.
/// @throws PostgreSQL ERROR when any eligible domain cannot be cleaned.
inline void sweep_registry(
        pg_sequence_source_registry& registry,
        pg_sequence_source_sweep_boundary boundary,
        bool is_commit) {
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_stats.sweeps;
#endif
    auto* domain = registry.domains;
    while (domain != nullptr) {
        auto* next = domain->next;
        const bool reclaimable = domain->registry_token_owned
            && domain->refcount == 1u
            && !domain->refcount_fault
            && domain->lifecycle == domain_lifecycle::live;
        /* A parameterized rescan starts a fresh callback/read session but may
         * retain the registry's current verified binding.  Keeping that one
         * bounded domain preserves its snapshot/cache and lets the next row
         * hit the matching (attribute,type,binding-generation) key. A stale
         * generation and every terminal/provider boundary still reclaim in
         * the ordinary owner-first order below. */
        const bool reusable_after_rescan =
            boundary == pg_sequence_source_sweep_boundary::rescan
            && !registry.provider_closed && registry.accepting_lookups
            && domain->binding_generation != 0u
            && domain->binding_generation == registry.binding_generation;
        if (reclaimable && !reusable_after_rescan) {
            cleanup_domain(*domain, is_commit);
            unlink_domain(registry, *domain);
            drop_registry_token_noexcept(*domain);
#ifdef BIOSEQ_TEST_INDEXES
            ++g_pg_sequence_source_stats.swept_domains;
#endif
        }
        domain = next;
    }
}

/// Reclaim all registry domains during transaction abort.
/// @param registry Registry whose eligible domains receive abort cleanup.
/// @param all_domains Whether retained domains are also detached.
inline void abort_sweep_registry_noerror(
        pg_sequence_source_registry& registry,
        bool all_domains) noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_stats.sweeps;
#endif
    auto* domain = registry.domains;
    while (domain != nullptr) {
        auto* next = domain->next;
        if (all_domains || domain->refcount == 1u
            || domain->refcount_fault) {
            abort_cleanup_domain_noerror(*domain);
            unlink_domain(registry, *domain);
            drop_registry_token_noexcept(*domain);
#ifdef BIOSEQ_TEST_INDEXES
            ++g_pg_sequence_source_stats.swept_domains;
#endif
        }
        domain = next;
    }
}

/// Unlink one registry from the backend-local registry list.
/// @param registry Registry to detach when present.
inline void remove_registry_from_global(
        pg_sequence_source_registry& registry) noexcept {
    auto** link = &registries;
    while (*link != nullptr) {
        if (*link == &registry) {
            *link = registry.next;
            registry.next = nullptr;
            return;
        }
        link = &(*link)->next;
    }
}

/// Destroy a detached registry control after every lease is released.
/// @param registry Candidate control object. It is freed only when detached,
/// domain-free, and lease-free.
inline void destroy_registry_control(
        pg_sequence_source_registry* registry) noexcept {
    if (registry == nullptr || registry->domains != nullptr
        || registry->active_leases != 0u) {
        return;
    }
    remove_registry_from_global(*registry);
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_stats.registries_destroyed;
#endif
    registry->backend_thread.~pg_sequence_backend_thread_token();
    pfree(registry);
}

/// Destroy a closed registry once its final lease disappears.
/// @param registry Candidate control object, or null.
inline void maybe_destroy_registry_control(
        pg_sequence_source_registry* registry) noexcept {
    if (registry != nullptr && registry->domains == nullptr
        && registry->active_leases == 0u
        && (registry->provider_closed || registry->xact_ended)) {
        destroy_registry_control(registry);
    }
}

/// Unlink one provider from the backend-local provider list.
/// @param provider Provider to detach when present.
inline void remove_provider_from_global(
        pg_sequence_source_provider& provider) noexcept {
    auto** link = &providers;
    while (*link != nullptr) {
        if (*link == &provider) {
            *link = provider.next;
            provider.next = nullptr;
            return;
        }
        link = &(*link)->next;
    }
}

/// Destroy one detached provider control object.
/// @param provider Provider to unlink and free, or null.
inline void destroy_provider_control(
        pg_sequence_source_provider* provider) noexcept {
    if (provider == nullptr) {
        return;
    }
    remove_provider_from_global(*provider);
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_stats.providers_destroyed;
#endif
    pfree(provider);
}

/// Release an authenticated registry lease without propagating ERROR.
/// @param lease Lease cleared on every path.
/// @return Whether generation, count, and backend-thread accounting were valid.
inline bool release_lease_token_noexcept(
        pg_sequence_source_registry_lease& lease) noexcept {
    if (!lease.owned || lease.registry == nullptr) {
        lease = {};
        return true;
    }
    auto* registry = lease.registry;
    if (lease.provider_generation != registry->generation
        || registry->active_leases == 0u
        || !registry->backend_thread.is_current()) {
        registry->lease_fault = true;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.lease_faults;
#endif
        lease = {};
        return false;
    }
    --registry->active_leases;
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_stats.leases_released;
#endif
    lease = {};
    return true;
}

/// Close a provider during abort and preserve parent cleanup authority.
/// @param provider Provider made invalid and freed before return.
inline void close_provider_abort_noerror(
        pg_sequence_source_provider& provider) noexcept {
    auto* registry = provider.lease.registry;
    provider.open = false;
    if (registry != nullptr) {
#ifdef BIOSEQ_TEST_INDEXES
        if (registry->active_leases > 1u) {
            ++g_pg_sequence_source_stats
                .provider_closed_with_upper_leases;
        }
#endif
        registry->provider_closed = true;
        registry->accepting_lookups = false;
    }
    (void)release_lease_token_noexcept(provider.lease);
    if (registry != nullptr) {
        abort_sweep_registry_noerror(*registry, false);
        maybe_destroy_registry_control(registry);
    }
    destroy_provider_control(&provider);
}

/// Close a provider on success and sweep reclaimable domains.
/// @param provider Provider made invalid and freed before return.
/// @param is_commit Whether domain cleanup receives commit semantics.
/// @throws PostgreSQL ERROR for stale provider state, corrupt lease accounting,
/// or domain cleanup failure.
inline void close_provider_checked(
        pg_sequence_source_provider& provider, bool is_commit) {
    auto* registry = provider.lease.registry;
    if (registry == nullptr || !provider.open
        || !registry->backend_thread.is_current()) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq source provider is not live on this backend thread")));
    }
    provider.open = false;
#ifdef BIOSEQ_TEST_INDEXES
    if (registry->active_leases > 1u) {
        ++g_pg_sequence_source_stats
            .provider_closed_with_upper_leases;
    }
#endif
    registry->provider_closed = true;
    if (!release_lease_token_noexcept(provider.lease)) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq source provider lease accounting is corrupt")));
    }
    if (registry->active_leases == 0u) {
        registry->accepting_lookups = false;
    }
    sweep_registry(
        *registry, pg_sequence_source_sweep_boundary::provider_end,
        is_commit);
    maybe_destroy_registry_control(registry);
    destroy_provider_control(&provider);
}

/// Finalize a registry at successful transaction end.
/// @param registry Registry detached after all domains are cleaned.
/// @param is_commit Whether domain cleanup receives commit semantics.
/// @throws PostgreSQL ERROR when domain cleanup fails.
inline void cleanup_registry_at_xact(
        pg_sequence_source_registry& registry, bool is_commit) {
    registry.accepting_lookups = false;
    registry.provider_closed = true;
    auto* domain = registry.domains;
    while (domain != nullptr) {
        auto* next = domain->next;
        cleanup_domain(*domain, is_commit);
        unlink_domain(registry, *domain);
        drop_registry_token_noexcept(*domain);
        domain = next;
    }
    registry.authoritative_snapshot = nullptr;
    registry.xact_ended = true;
    remove_registry_from_global(registry);
    maybe_destroy_registry_control(&registry);
}

/// Tear down a registry at aborted transaction end.
/// @param registry Registry detached after abort cleanup.
inline void abort_registry_at_xact_noerror(
        pg_sequence_source_registry& registry) noexcept {
    registry.accepting_lookups = false;
    registry.provider_closed = true;
    abort_sweep_registry_noerror(registry, true);
    registry.authoritative_snapshot = nullptr;
    registry.xact_ended = true;
    remove_registry_from_global(registry);
    maybe_destroy_registry_control(&registry);
}

/// Dispatch top-level transaction lifecycle events to source registries.
/// @param event PostgreSQL transaction event.
/// @throws PostgreSQL ERROR when prepared transactions still have live domains
/// or successful cleanup fails.
inline void xact_callback(XactEvent event, void*) {
    switch (event) {
        case XACT_EVENT_PRE_COMMIT:
        case XACT_EVENT_PARALLEL_PRE_COMMIT: {
            while (registries != nullptr) {
                cleanup_registry_at_xact(*registries, true);
            }
            break;
        }
        case XACT_EVENT_ABORT:
        case XACT_EVENT_PARALLEL_ABORT: {
            while (registries != nullptr) {
                abort_registry_at_xact_noerror(*registries);
            }
            break;
        }
        case XACT_EVENT_COMMIT:
        case XACT_EVENT_PARALLEL_COMMIT:
            Assert(registries == nullptr);
            break;
        case XACT_EVENT_PRE_PREPARE:
            for (auto* registry = registries; registry != nullptr;
                registry = registry->next) {
                if (registry->domains != nullptr) {
#ifdef BIOSEQ_TEST_INDEXES
                    ++g_pg_sequence_source_stats.pre_prepare_rejections;
#endif
                    ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("cannot PREPARE TRANSACTION with live bioseq source domains"),
                         errhint("materialize locator-backed bioseq values "
                                 "before PREPARE TRANSACTION")));
                }
            }
            while (registries != nullptr) {
                cleanup_registry_at_xact(*registries, true);
            }
            break;
        case XACT_EVENT_PREPARE:
            Assert(registries == nullptr);
            break;
        default:
            Assert(false);
            break;
    }
}

/// Promote or discard source ownership at a subtransaction boundary.
/// @param event PostgreSQL subtransaction event.
/// @param my_subid Ending or current subtransaction identity.
/// @param parent_subid Parent identity used for successful promotion.
inline void subxact_callback(
        SubXactEvent event,
        SubTransactionId my_subid,
        SubTransactionId parent_subid,
        void*) {
    switch (event) {
        case SUBXACT_EVENT_START_SUB:
        case SUBXACT_EVENT_PRE_COMMIT_SUB:
            break;
        case SUBXACT_EVENT_COMMIT_SUB:
            for (auto* registry = registries; registry != nullptr;
                 registry = registry->next) {
                if (registry->creating_subid == my_subid) {
                    registry->creating_subid = parent_subid;
#ifdef BIOSEQ_TEST_INDEXES
                    ++g_pg_sequence_source_stats.subxact_promotions;
#endif
                }
                for (auto* domain = registry->domains; domain != nullptr;
                     domain = domain->next) {
                    if (domain->creating_subid == my_subid) {
                        domain->creating_subid = parent_subid;
#ifdef BIOSEQ_TEST_INDEXES
                        ++g_pg_sequence_source_stats.subxact_promotions;
#endif
                    }
                }
            }
            break;
        case SUBXACT_EVENT_ABORT_SUB: {
#ifdef BIOSEQ_TEST_INDEXES
            ++g_pg_sequence_source_stats.subxact_aborts;
#endif
            auto* registry = registries;
            while (registry != nullptr) {
                auto* next_registry = registry->next;
                if (registry->creating_subid == my_subid) {
                    abort_registry_at_xact_noerror(*registry);
                } else {
                    auto* domain = registry->domains;
                    while (domain != nullptr) {
                        auto* next_domain = domain->next;
                        if (domain->creating_subid == my_subid) {
                            abort_cleanup_domain_noerror(*domain);
                            unlink_domain(*registry, *domain);
                            drop_registry_token_noexcept(*domain);
                        }
                        domain = next_domain;
                    }
                }
                registry = next_registry;
            }
            break;
        }
        default:
            Assert(false);
            break;
    }
}

/// Register backend-local transaction callbacks exactly once.
///
/// Registration is process-backend local and persists for the backend lifetime.
inline void ensure_callbacks_registered() {
    if (callbacks_registered) {
        return;
    }
    RegisterXactCallback(xact_callback, nullptr);
    RegisterSubXactCallback(subxact_callback, nullptr);
    callbacks_registered = true;
}

/// Validate a catalog source row against one registry identity.
/// @param registry Authenticated relation, storage, and snapshot binding.
/// @param column_attno One-based sequence attribute number.
/// @param locator Locator decoded from the parent row.
/// @param stored_type Persisted alphabet and packed-unit width.
/// @param last_padding_letters Unused letters in the final packed unit.
/// @param row Snapshot-visible immutable source row.
/// @return Whether every binding, locator, type, and ready-state field matches.
[[nodiscard]] inline bool source_row_matches_registry(
        const pg_sequence_source_registry& registry,
        AttrNumber column_attno,
        const sequence_locator& locator,
        compact_sequence_stdc_type_params stored_type,
        std::uint16_t last_padding_letters,
        const sequence_source_version_row& row) noexcept {
    return row.state == k_sequence_source_state_ready
        && row.source_epoch == locator.source_epoch
        && row.user_relid == registry.user_relid
        && row.database_oid == registry.database_oid
        && row.tablespace_oid == registry.tablespace_oid
        && row.main_relfilenode == registry.main_relfilenode
        && row.main_storage_id == registry.main_storage_id
        && row.main_storage_id == registry.binding_generation
        && row.column_attno == column_attno
        && same_locator(row.locator, locator)
        && row.alphabet_code == stored_type.alphabet_code
        && row.storage_unit_byte_width
            == stored_type.storage_unit_byte_width
        && row.padding_letters == last_padding_letters;
}

/// Construct one transaction-owned source domain from a catalog row.
/// @param registry Live registry whose authoritative snapshot is registered on
/// a new child `ResourceOwner`.
/// @param column_attno One-based sequence attribute number.
/// @param stored_type Persisted alphabet and packed-unit width.
/// @return Registry-owned live domain whose read cache acquires `AccessShareLock`.
/// @throws PostgreSQL ERROR when context, owner, snapshot, or cache creation fails.
[[nodiscard]] inline pg_sequence_source_domain* create_domain(
        pg_sequence_source_registry& registry,
        AttrNumber column_attno,
        compact_sequence_stdc_type_params stored_type) {
    pg_sequence_source_domain* volatile pending = nullptr;
    pg_sequence_source_domain* volatile result = nullptr;
    const MemoryContext saved_context = CurrentMemoryContext;
    const ResourceOwner saved_owner = CurrentResourceOwner;
    PG_TRY();
    {
        auto* domain = static_cast<pg_sequence_source_domain*>(
            MemoryContextAllocZero(
                TopMemoryContext, sizeof(pg_sequence_source_domain)));
        pending = domain;
        ::new (static_cast<void*>(domain)) pg_sequence_source_domain{};
        domain->backend_thread = pg_sequence_backend_thread_token::capture();
        domain->registry = &registry;
        domain->column_attno = column_attno;
        domain->stored_type = stored_type;
        domain->binding_generation = registry.binding_generation;
        domain->creating_subid = GetCurrentSubTransactionId();
        domain->generation = allocate_generation();
        domain->refcount = 1u;
        domain->registry_token_owned = true;
#ifdef BIOSEQ_TEST_INDEXES
        inject_pg_sequence_source_failure(
            sequence_source_failure::domain_after_control);
#endif
        domain->payload_context = AllocSetContextCreate(
            TopTransactionContext, "bioseq shared sequence source domain",
            ALLOCSET_DEFAULT_SIZES);
#ifdef BIOSEQ_TEST_INDEXES
        inject_pg_sequence_source_failure(
            sequence_source_failure::domain_after_context);
#endif
        domain->resource_owner = ResourceOwnerCreate(
            TopTransactionResourceOwner,
            "bioseq shared sequence source domain");
#ifdef BIOSEQ_TEST_INDEXES
        inject_pg_sequence_source_failure(
            sequence_source_failure::domain_after_owner);
#endif
        CurrentResourceOwner = domain->resource_owner;
        domain->registered_snapshot = RegisterSnapshotOnOwner(
            registry.authoritative_snapshot, domain->resource_owner);
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.snapshots_registered;
        inject_pg_sequence_source_failure(
            sequence_source_failure::domain_after_snapshot);
#endif
        domain->read_caches = alloc_seq_caches(domain->payload_context);
        domain->read_caches->side = seq_caches::access_side::read;
        domain->read_caches->lock_mode = AccessShareLock;
        domain->read_caches->read_state_mctx = domain->payload_context;
        domain->read_caches->read_resource_owner = domain->resource_owner;
        domain->read_caches->main_database_oid = registry.database_oid;
        domain->read_caches->main_tablespace_oid = registry.tablespace_oid;
        domain->read_caches->main_relfilenode = registry.main_relfilenode;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.cache_sets_created;
        inject_pg_sequence_source_failure(
            sequence_source_failure::domain_after_caches);
#endif
        CurrentResourceOwner = saved_owner;
        MemoryContextSwitchTo(saved_context);
        domain->lifecycle = domain_lifecycle::live;
        domain->next = registry.domains;
        registry.domains = domain;
        result = domain;
        pending = nullptr;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.domains_created;
#endif
    }
    PG_CATCH();
    {
        CurrentResourceOwner = saved_owner;
        MemoryContextSwitchTo(saved_context);
        auto* domain = const_cast<pg_sequence_source_domain*>(pending);
        if (domain != nullptr) {
            abort_cleanup_domain_noerror(*domain);
            domain->registry_token_owned = false;
            domain->registry = nullptr;
            domain->refcount = 0u;
            domain->lifecycle = domain_lifecycle::released;
            domain->backend_thread.~pg_sequence_backend_thread_token();
            pfree(domain);
        }
        PG_RE_THROW();
    }
    PG_END_TRY();
    return const_cast<pg_sequence_source_domain*>(result);
}

/// Reuse a matching readable domain or create one under the registry.
/// @return Borrowed domain pointer owned by the registry and any active retains.
/// @throws PostgreSQL ERROR when new-domain construction fails.
[[nodiscard]] inline pg_sequence_source_domain* find_or_create_domain(
        pg_sequence_source_registry& registry,
        AttrNumber column_attno,
        compact_sequence_stdc_type_params stored_type) {
    for (auto* domain = registry.domains; domain != nullptr;
         domain = domain->next) {
        if (domain_key_matches(*domain, column_attno, stored_type)) {
#ifdef BIOSEQ_TEST_INDEXES
            ++g_pg_sequence_source_stats.domains_reused;
#endif
            return domain;
        }
    }
    return create_domain(registry, column_attno, stored_type);
}

/// Allocate a provider, registry and initial authenticated lease.
/// @param owning_relation Open relation whose physical identity is copied.
/// @param authoritative_registered_snapshot Borrowed registered MVCC snapshot
/// that must remain valid for registry lookups.
/// @param kind Executor lifecycle owning the provider.
/// @return Backend-local provider allocated in `TopMemoryContext`.
/// @throws PostgreSQL ERROR when prerequisites, storage identity, or allocation fail.
[[nodiscard]] inline pg_sequence_source_provider* create_provider(
        Relation owning_relation,
        Snapshot authoritative_registered_snapshot,
        pg_sequence_source_provider_kind kind) {
    if (owning_relation == nullptr
        || authoritative_registered_snapshot == nullptr
        || !IsMVCCSnapshot(authoritative_registered_snapshot)
        || authoritative_registered_snapshot->regd_count == 0) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq source registry requires an owning relation and "
                    "registered MVCC snapshot")));
    }
    ensure_callbacks_registered();
    const std::uint64_t storage_id =
        bioseq::postgres::support::invoke_pg_as_cpp([owning_relation] {
            return column_storage_id(
                owning_relation, false);
        });
    if (storage_id == 0u) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq source registry relation has no physical storage identity")));
    }

    pg_sequence_source_registry* volatile pending_registry = nullptr;
    pg_sequence_source_provider* volatile pending_provider = nullptr;
    pg_sequence_source_provider* volatile result = nullptr;
    PG_TRY();
    {
        auto* registry = static_cast<pg_sequence_source_registry*>(
            MemoryContextAllocZero(
                TopMemoryContext, sizeof(pg_sequence_source_registry)));
        pending_registry = registry;
        ::new (static_cast<void*>(registry)) pg_sequence_source_registry{};
        registry->backend_thread =
            pg_sequence_backend_thread_token::capture();
        registry->user_relid = RelationGetRelid(owning_relation);
        registry->database_oid = owning_relation->rd_locator.dbOid;
        registry->tablespace_oid = owning_relation->rd_locator.spcOid;
        registry->main_relfilenode = owning_relation->rd_locator.relNumber;
        registry->main_storage_id = storage_id;
        registry->binding_generation = storage_id;
        registry->authoritative_snapshot =
            authoritative_registered_snapshot;
        registry->creating_subid = GetCurrentSubTransactionId();
        registry->generation = allocate_generation();
        registry->active_leases = 1u;
        registry->accepting_lookups = true;
#ifdef BIOSEQ_TEST_INDEXES
        inject_pg_sequence_source_failure(
            sequence_source_failure::provider_after_registry_control);
#endif

        auto* provider = static_cast<pg_sequence_source_provider*>(
            MemoryContextAllocZero(
                TopMemoryContext, sizeof(pg_sequence_source_provider)));
        pending_provider = provider;
        ::new (static_cast<void*>(provider)) pg_sequence_source_provider{};
        provider->lease = {
            .registry = registry,
            .provider_generation = registry->generation,
            .owned = true,
        };
        provider->kind = kind;
        provider->open = true;
#ifdef BIOSEQ_TEST_INDEXES
        inject_pg_sequence_source_failure(
            sequence_source_failure::provider_after_provider_control);
#endif

        registry->next = registries;
        registries = registry;
        provider->next = providers;
        providers = provider;
        result = provider;
        pending_registry = nullptr;
        pending_provider = nullptr;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.registries_created;
        ++g_pg_sequence_source_stats.providers_created;
        if (kind == pg_sequence_source_provider_kind::scan) {
            ++g_pg_sequence_source_stats.scan_providers_created;
        } else {
            ++g_pg_sequence_source_stats
                .standalone_providers_created;
        }
        ++g_pg_sequence_source_stats.leases_acquired;
#endif
    }
    PG_CATCH();
    {
        auto* provider =
            const_cast<pg_sequence_source_provider*>(pending_provider);
        auto* registry =
            const_cast<pg_sequence_source_registry*>(pending_registry);
        if (provider != nullptr) {
            pfree(provider);
        }
        if (registry != nullptr) {
            registry->backend_thread.~pg_sequence_backend_thread_token();
            pfree(registry);
        }
        PG_RE_THROW();
    }
    PG_END_TRY();
    return const_cast<pg_sequence_source_provider*>(result);
}


}  // namespace sequence_source_detail

}  // namespace bioseq::storage

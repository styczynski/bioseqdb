/**
 * @file scanner/source_registry.cppm
 * @brief Keep sequence readers alive and clean them up with their transaction.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/attnum.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "storage/lockdefs.h"
#include "utils/errcodes.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <span>

export module bioseq.postgres.storage.scanner:source_registry;

export import bioseq.postgres.storage.scanner.source_provider_contract;
export import bioseq.postgres.storage.scanner.source_test_contract;

import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.configuration.core;
import bioseq.postgres.support;
import bioseq.common.seqan_adapter;              
import bioseq.postgres.storage.manager;     // Fragment reader caches.
import bioseq.storage.scanner;              
import bioseq.storage.lazy_sequence;        // Lazy sequence source types and test counters.
import bioseq.storage.coordinates;
import bioseq.storage.layout;
import bioseq.postgres.storage.physical;
import bioseq.storage.size;        
import bioseq.postgres.storage.metadata;    
import bioseq.sequences;           
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.table.io;            
import bioseq.parallelism.pool;

export namespace bioseq::storage {

// Sequence sources shared by readers in this transaction.

#ifdef BIOSEQ_TEST_INDEXES
/// Disable source and lazy-view test failures.
inline void disarm_pg_sequence_source_failure() noexcept {
  g_pg_sequence_source_failure = sequence_source_failure::none;
  disarm_pg_sequence_facade_test_poison();
}

/// @return Test counters, including the numbers of objects currently alive.
[[nodiscard]] inline sequence_source_stats
pg_sequence_source_stats() noexcept;

/// Clear source and lazy-view counters before a test.
inline void reset_pg_sequence_source_stats() noexcept {
  g_pg_sequence_source_stats = {};
  reset_pg_sequence_facade_test_counters();
}
#endif

namespace sequence_source_detail {

/// Scanner kept by a read session, with callbacks for its concrete type.
struct source_scanner_entry {
    /// Read domain held by this entry, supplying its snapshot and fragment cache.
    pg_sequence_source_domain* domain = nullptr;
    /// Scanner allocated in this entry's memory context.
    void* concrete_scanner = nullptr;
    /// Read decoded ranks, applying the requested sequence transformation.
    void (*read_ranks)(source_scanner_entry&, sequence_transform,
                       std::uint64_t, std::span<std::uint8_t>) = nullptr;
    /// Read packed units in the stored sequence order.
    void (*read_packed_units)(source_scanner_entry&, std::uint64_t,
                              std::span<std::uint8_t>) = nullptr;
    /// Release resources after a successful read. May report an error.
    void (*finish)(source_scanner_entry&) = nullptr;
    /// Failure cleanup callback that does not propagate errors.
    void (*abort_noexcept)(source_scanner_entry&) noexcept = nullptr;
    /// Whether the concrete scanner's constructor completed.
    bool constructed = false;
    /// Whether this entry holds a counted reference to the domain.
    bool domain_retained = false;
};

/// Source details used to construct a scanner entry.
struct source_scanner_factory_argument {
    /// Domain borrowed for construction. The resulting entry holds a counted reference.
    pg_sequence_source_domain* domain = nullptr;
    /// Locator that selects the physical fragment range.
    sequence_locator locator{};
    /// Alphabet and packed-unit width used to select a scanner type.
    compact_sequence_stdc_type_params stored_type{};
};

inline pg_sequence_source_registry *registries = nullptr;
inline pg_sequence_source_provider *providers = nullptr;
inline std::uint64_t next_generation = 1u;
inline bool callbacks_registered = false;

#ifdef BIOSEQ_TEST_INDEXES
/// Combine test counters with counts from the live registry and provider lists.
/// @return Counters for the current backend.
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

/// @return Whether every byte of the two fixed-size locators matches.
[[nodiscard]] inline bool same_locator(
        const sequence_locator& left, const sequence_locator& right) noexcept {
    return std::memcmp(&left, &right, sizeof(sequence_locator)) == 0;
}

/// Allocate a distinct nonzero generation number in this backend.
/// @throws PostgreSQL ERROR before the counter reaches the unsigned 64-bit limit.
/// @return Distinct nonzero generation below the unsigned 64-bit maximum.
[[nodiscard]] inline std::uint64_t allocate_generation() {
    if (next_generation == 0u
        || next_generation == std::numeric_limits<std::uint64_t>::max()) {
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("bioseq source-domain generation space exhausted")));
    }
    return next_generation++;
}

/// @return Whether the domain is live, has valid reference accounting and
/// still holds its registry reference, snapshot, ResourceOwner and read cache.
/// @param domain Domain whose state and resources are checked.
[[nodiscard]] inline bool domain_is_readable(
        const pg_sequence_source_domain& domain) noexcept {
    return domain.lifecycle == domain_lifecycle::live
        && !domain.refcount_fault
        && domain.registry_token_owned
        && domain.registered_snapshot != nullptr
        && domain.resource_owner != nullptr
        && domain.read_caches != nullptr;
}

/// Add a reference to a domain. A null pointer does nothing.
/// A zero or overflowing count marks the domain as failed and prevents reads.
/// @param opaque Domain pointer, or null.
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

/// Release a domain reference. A null pointer does nothing.
/// The last reference frees the domain only after registry removal and
/// resource cleanup. Inconsistent counts mark the domain as failed.
/// @param opaque Domain pointer, or null.
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

/// @return Domain address and generation, or an empty identity for null.
/// @param opaque Domain pointer, or null.
[[nodiscard]] inline pg_sequence_source_identity domain_identity(
        const void* opaque) noexcept {
    auto const* domain = static_cast<const pg_sequence_source_domain*>(opaque);
    return domain == nullptr ? pg_sequence_source_identity{}
        : pg_sequence_source_identity{
            .control_address = reinterpret_cast<std::uintptr_t>(domain),
            .generation = domain->generation,
        };
}

/// @return False. PostgreSQL read domains stay on their backend thread.
[[nodiscard]] inline bool domain_worker_shareable(const void*) noexcept {
    return false;
}

/// Check for a usable reader for this column and packed type.
/// The domain's storage generation must still match its registry.
/// @param domain Domain to check.
/// @param column_attno One-based sequence column number.
/// @param stored_type Stored alphabet and packed-unit width.
/// @return Whether the domain is readable and matches the requested source.
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

} // namespace sequence_source_detail

#ifdef BIOSEQ_TEST_INDEXES
/// @return Test counters and the numbers of source objects currently alive.
[[nodiscard]] inline sequence_source_stats
pg_sequence_source_stats() noexcept {
    return sequence_source_detail::collect_pg_sequence_source_stats();
}
#endif

namespace sequence_source_detail {

/// Close and destroy a read cache during abort, suppressing PostgreSQL errors.
/// Clear the pointer even if closing the cache fails.
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

/// Unregister the domain's snapshot during abort and clear its pointer.
/// Restore the caller's ResourceOwner even if PostgreSQL reports an error.
/// @param domain Domain whose snapshot pointer is cleared.
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

/// Attempt one ResourceOwner cleanup stage during abort.
/// Record the attempt even if it fails, so later cleanup does not repeat it.
/// @param postgres_stage PostgreSQL release stage.
/// @param stage_flag Bit used to record this stage.
/// @param domain Domain that owns the child ResourceOwner.
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
        /* Leave resources whose release failed to PostgreSQL's parent cleanup.
         * Record this attempt so the domain will not repeat the failed callback. */
        domain.released_owner_stages |= stage_bit(stage_flag);
    }
    PG_END_TRY();
}

/// Delete the child ResourceOwner during abort, suppressing PostgreSQL errors.
/// Clear the pointer before deletion.
/// @param domain Domain whose ResourceOwner pointer is cleared.
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

/// Delete the domain's memory context during abort, suppressing PostgreSQL errors.
/// Clear the pointer before deletion.
/// @param domain Domain whose payload-context pointer is cleared.
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

/// Attempt all remaining resource cleanup and mark this domain invalid.
/// Abort cleanup suppresses PostgreSQL errors so the original failure survives.
/// @param domain Domain made invalid after all cleanup attempts.
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

/// Release a domain's readers, snapshot and remaining PostgreSQL resources.
/// ResourceOwner cleanup runs before locks, for locks, then after locks.
/// @param is_commit Whether cleanup callbacks should use commit behavior.
/// @throws PostgreSQL ERROR if a cleanup step fails.
/// @param domain Domain whose readers and resources are released.
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

/// Remove a domain from the registry list without releasing its reference.
/// @param registry Registry whose list is searched.
/// @param domain Domain to unlink if present.
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

/// Release the registry's reference after removing the domain from its list.
/// @param domain Domain already removed from its registry list.
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

/// Clean up domains whose only remaining reference belongs to this registry.
/// A rescan keeps matching read resources for reuse while the provider stays open.
/// @param boundary Event that triggered this cleanup.
/// @param is_commit Whether resource cleanup uses commit behavior.
/// @throws PostgreSQL ERROR if cleanup fails.
/// @param registry Registry whose unused domains may be removed.
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
        /* A rescan can reuse the registered snapshot and reader cache while the
         * provider remains open and the table's storage ID is unchanged. */
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

/// Clean up unused or failed domains during abort.
/// @param all_domains Also invalidate and detach domains still held by readers.
/// @param registry Registry whose domains receive abort cleanup.
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

/// Unlink and free a registry that has no domains or counted references.
/// A null pointer or a registry still in use does nothing.
/// @param registry Registry to check, or null.
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

/// Free a closed registry once its domains and references are gone.
/// @param registry Registry to check, or null.
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

/// Unlink and free a provider. A null pointer does nothing.
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

/// Release a registry reference and clear its token.
/// @return False if its generation, count or backend thread is wrong.
/// Invalid accounting marks the registry as failed. No error is raised here.
/// @param lease Reference cleared on every path.
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

/// Close and free a provider during abort. Stop new lookups and clean up
/// unused or failed domains, leaving resources still in use for later cleanup.
/// @param provider Provider closed and freed before return.
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

/// Close and free a provider, then release unused read domains.
/// References held by parent plan nodes allow those nodes to keep reading.
/// @param is_commit Whether domain cleanup uses commit behavior.
/// @throws PostgreSQL ERROR for an invalid provider, broken reference counts
/// or a cleanup failure.
/// @param provider Provider closed and freed on success.
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

/// Clean all domains and detach the registry at transaction end.
/// Counted references can keep its control object alive after resources close.
/// @param is_commit Whether cleanup uses commit behavior.
/// @throws PostgreSQL ERROR if domain cleanup fails.
/// @param registry Registry detached after cleaning its domains.
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

/// Invalidate all domains and detach the registry during transaction abort.
/// Cleanup errors are suppressed.
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

/// Clean up source registries when a transaction commits or aborts.
/// Reject PREPARE TRANSACTION while any read domains remain, since their
/// PostgreSQL resources cannot survive as prepared transaction state.
/// @throws PostgreSQL ERROR for live domains at prepare or failed commit cleanup.
/// @param event PostgreSQL transaction event.
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

/// On subtransaction commit, transfer its registries and domains to the parent.
/// On abort, invalidate those created by the ending subtransaction.
/// @param my_subid Subtransaction that is ending.
/// @param parent_subid Parent that takes ownership after commit.
/// @param event PostgreSQL subtransaction event.
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

/// Register transaction and subtransaction cleanup callbacks once per backend.
inline void ensure_callbacks_registered() {
    if (callbacks_registered) {
        return;
    }
    RegisterXactCallback(xact_callback, nullptr);
    RegisterSubXactCallback(subxact_callback, nullptr);
    callbacks_registered = true;
}

/// Check that a visible, ready source row describes this table column and locator.
/// Its storage generation, alphabet, packed-unit width and padding must match
/// the reader's expected values.
/// @param registry Expected table, storage and snapshot details.
/// @param column_attno One-based sequence column number.
/// @param locator Locator read from the parent row.
/// @param stored_type Stored alphabet and packed-unit width.
/// @param last_padding_letters Unused letters in the final packed unit.
/// @param row Source row already read under the required snapshot.
/// @return Whether every source field and the ready state match.
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

/// Create shared read resources for one column and packed type.
/// Register the registry's snapshot on a child ResourceOwner and create a
/// fragment cache that opens tables with AccessShareLock. The registry holds
/// the initial reference to the domain.
/// @throws PostgreSQL ERROR if resource creation fails. Partial allocations
/// are cleaned up before the error is rethrown.
/// @param registry Registry supplying the snapshot and table details.
/// @param column_attno One-based sequence column number.
/// @param stored_type Stored alphabet and packed-unit width.
/// @return Live domain held by the registry.
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

/// Reuse a live domain for this column, type and storage generation, or create it.
/// @return Borrowed domain held by the registry.
/// @throws PostgreSQL ERROR if creation fails.
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

/// Create a provider and registry with one counted reference.
/// @param owning_relation Open table whose identifiers and storage ID are copied.
/// @param authoritative_registered_snapshot Borrowed registered MVCC snapshot
/// that must remain alive for source lookups.
/// @param kind Operation responsible for closing the provider.
/// @return Provider allocated in TopMemoryContext for this backend.
/// @throws An error if the table or snapshot is invalid, the storage ID is
/// missing, or allocation fails.
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

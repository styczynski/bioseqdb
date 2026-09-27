/**
 * @file scanner/source_scanner/provider.cppm
 * @brief Source-provider and registry-lease lifecycle.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "utils/errcodes.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <cstdint>
#include <limits>
#include <new>

export module bioseq.postgres.storage.scanner:source_scanner_provider;

export import bioseq.postgres.storage.scanner.snapshot_rebind;

import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.configuration.core;
import bioseq.postgres.support;
import bioseq.common.seqan_adapter;     // seq::dna4 in static_assert below
import bioseq.postgres.storage.manager; // seq_caches + open_sub_table_for_locator
import bioseq.storage.scanner;          // scanner concepts
import bioseq.storage.lazy_sequence;    // lazy_sequence + with_postgres_lazy_sequence
import bioseq.storage.coordinates;
import bioseq.storage.layout;
import bioseq.postgres.storage.physical;
import bioseq.storage.size;              // sequence_size_t
import bioseq.postgres.storage.metadata; // stripe_metadata/find_stripe_by_row_number
import bioseq.sequences;                 // compact_sequence_range, compact_sequence_stdc_instance
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.table.io; // begin_column_read/read_column_row_by_number
import bioseq.parallelism.pool;
import bioseq.postgres.storage.scanner.fragment_scanner;
import :source_registry;

export import :source_scanner_entry;

export namespace bioseq::storage {
/// Create a source provider owned by a table scan.
/// @param owning_relation Open relation whose physical identity is copied.
/// @param authoritative_registered_snapshot Registered MVCC snapshot that must
/// remain registered until the provider and all derived leases are closed.
/// @return Backend-local provider that must be passed to
/// `close_pg_sequence_source_provider`.
/// @throws PostgreSQL ERROR when relation or snapshot preconditions fail.
[[nodiscard]] inline pg_sequence_source_provider*
create_pg_sequence_source_provider(Relation owning_relation,
                                   Snapshot authoritative_registered_snapshot) {
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [owning_relation, authoritative_registered_snapshot] {
            return sequence_source_detail::create_provider(owning_relation,
                                                           authoritative_registered_snapshot,
                                                           pg_sequence_source_provider_kind::scan);
        });
}

/**
 * Create a scan-owned provider through the scanner owner's out-of-line ABI.
 *
 * The ordinary inline API above remains available to scanner-local callers.
 * Narrow cross-module factories use this boundary so their implementation
 * units do not instantiate the registry and PostgreSQL exception bridge.
 */
[[nodiscard]] pg_sequence_source_provider*
create_pg_sequence_source_provider_owner_boundary(
    Relation owning_relation,
    Snapshot authoritative_registered_snapshot)
{
    return create_pg_sequence_source_provider(owning_relation,
                                              authoritative_registered_snapshot);
}

/// Create a source provider owned by standalone index administration.
/// @param owning_relation Open relation whose physical identity is copied.
/// @param authoritative_registered_snapshot Registered MVCC snapshot that must
/// remain registered until the provider and all derived leases are closed.
/// @return Backend-local provider that must be passed to
/// `close_pg_sequence_source_provider`.
/// @throws PostgreSQL ERROR when relation or snapshot preconditions fail.
[[nodiscard]] inline pg_sequence_source_provider*
create_standalone_pg_sequence_source_provider(Relation owning_relation,
                                              Snapshot authoritative_registered_snapshot) {
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [owning_relation, authoritative_registered_snapshot] {
            return sequence_source_detail::create_provider(
                owning_relation,
                authoritative_registered_snapshot,
                pg_sequence_source_provider_kind::standalone_index);
        });
}

/// Acquire an independent upper-owner lease from another live lease.
/// @param parent Live lease owned by the current backend thread.
/// @return Independently owned lease that the caller must release.
/// @throws PostgreSQL ERROR when the lease is stale or its reference count overflows.
[[nodiscard]] inline pg_sequence_source_registry_lease
acquire_pg_sequence_source_registry_lease(const pg_sequence_source_registry_lease& parent) {
    using namespace sequence_source_detail;
    if (!registry_lease_is_live(parent)) {
        if (parent.registry != nullptr && !parent.registry->backend_thread.is_current()) {
#ifdef BIOSEQ_TEST_INDEXES
            ++g_pg_sequence_source_stats.wrong_thread_rejections;
#endif
            return {};
        }
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq source registry lease is not live")));
    }
    auto& registry = *parent.registry;
    if (registry.active_leases == std::numeric_limits<std::uint32_t>::max()) {
        registry.lease_fault = true;
        registry.accepting_lookups = false;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.lease_faults;
#endif
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq source registry lease count overflow")));
    }
    ++registry.active_leases;
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_stats.leases_acquired;
#endif
    return {
        .registry = &registry,
        .provider_generation = registry.generation,
        .owned = true,
    };
}

/// Acquire an independent upper-owner lease from an open provider.
/// @param provider Open provider owned by the current backend thread.
/// @return Independently owned lease that the caller must release.
/// @throws PostgreSQL ERROR when the provider or its lease is not live.
[[nodiscard]] inline pg_sequence_source_registry_lease
acquire_pg_sequence_source_registry_lease(const pg_sequence_source_provider* provider) {
    if (provider == nullptr || !provider->open) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq source provider is not open")));
    }
    return acquire_pg_sequence_source_registry_lease(provider->lease);
}

/// Release one lease, sweep at the named boundary and report thread validity.
/// @param lease Owned lease cleared on a valid release attempt.
/// @param boundary Lifecycle boundary recorded for domain reclamation.
/// @param is_commit Whether checked commit cleanup is permitted.
/// @return Whether lease accounting and backend-thread validation succeeded.
/// @throws PostgreSQL ERROR when commit-mode domain cleanup fails.
[[nodiscard]] inline bool
release_pg_sequence_source_registry_lease(pg_sequence_source_registry_lease& lease,
                                          pg_sequence_source_sweep_boundary boundary,
                                          bool is_commit) {
    using namespace sequence_source_detail;
    auto* registry = lease.registry;
    if (registry == nullptr || !lease.owned) {
        lease = {};
        return true;
    }
    if (!registry->backend_thread.is_current()) {
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.wrong_thread_rejections;
#endif
        return false;
    }
    const bool released = release_lease_token_noexcept(lease);
    if (!released) {
        return false;
    }
    if (registry->provider_closed && registry->active_leases == 0u) {
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.last_upper_lease_after_provider_end;
#endif
        registry->accepting_lookups = false;
    }
    if (is_commit) {
        sweep_registry(*registry, boundary, true);
    } else {
        abort_sweep_registry_noerror(*registry, false);
    }
    maybe_destroy_registry_control(registry);
    return true;
}

/// Abort-release one lease without propagating PostgreSQL failures.
/// @param lease Owned lease cleared by the abort path.
/// @param boundary Lifecycle boundary accepted for API symmetry.
inline void release_pg_sequence_source_registry_lease_noexcept(
    pg_sequence_source_registry_lease& lease, pg_sequence_source_sweep_boundary boundary) noexcept {
    using namespace sequence_source_detail;
    auto* registry = lease.registry;
    if (registry == nullptr || !lease.owned) {
        lease = {};
        return;
    }
    if (!registry->backend_thread.is_current()) {
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.wrong_thread_rejections;
#endif
        lease = {};
        return;
    }
    (void)release_lease_token_noexcept(lease);
    if (registry->provider_closed && registry->active_leases == 0u) {
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.last_upper_lease_after_provider_end;
#endif
        registry->accepting_lookups = false;
    }
    (void)boundary;
    abort_sweep_registry_noerror(*registry, false);
    maybe_destroy_registry_control(registry);
}

/// Close one provider through the checked or abort cleanup route.
/// @param provider Provider to close, or null for no work. The pointer is
/// invalid after this call.
/// @param is_commit Whether checked commit cleanup is permitted.
/// @throws PostgreSQL ERROR when commit-mode cleanup detects invalid state.
inline void close_pg_sequence_source_provider(pg_sequence_source_provider* provider,
                                              bool is_commit) {
    if (provider == nullptr) {
        return;
    }
    if (is_commit) {
        sequence_source_detail::close_provider_checked(*provider, true);
    } else {
        sequence_source_detail::close_provider_abort_noerror(*provider);
    }
}

/// Sweep reclaimable domains while retaining the caller's registry lease.
/// @param lease Live lease retained by the caller.
/// @param boundary Lifecycle boundary governing reclamation telemetry.
/// @param is_commit Whether checked commit cleanup is permitted.
/// @throws PostgreSQL ERROR when the lease is stale or commit cleanup fails.
inline void
sweep_reclaimable_pg_sequence_source_domains(const pg_sequence_source_registry_lease& lease,
                                             pg_sequence_source_sweep_boundary boundary,
                                             bool is_commit = true) {
    using namespace sequence_source_detail;
    if (!registry_lease_is_live(lease)) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("cannot sweep a closed bioseq source registry")));
    }
    if (is_commit) {
        sweep_registry(*lease.registry, boundary, true);
    } else {
        abort_sweep_registry_noerror(*lease.registry, false);
    }
}

} // namespace bioseq::storage

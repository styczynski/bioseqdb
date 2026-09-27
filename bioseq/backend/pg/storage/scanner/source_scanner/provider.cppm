/**
 * @file scanner/source_scanner/provider.cppm
 * @brief Open sequence source providers and manage counted registry references.
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
import bioseq.common.seqan_adapter;     
import bioseq.postgres.storage.manager; 
import bioseq.storage.scanner;          // Source provider and read-session types.
import bioseq.storage.lazy_sequence;    
import bioseq.storage.coordinates;
import bioseq.storage.layout;
import bioseq.postgres.storage.physical;
import bioseq.storage.size;              
import bioseq.postgres.storage.metadata; 
import bioseq.sequences;                 
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.table.io; 
import bioseq.parallelism.pool;
import bioseq.postgres.storage.scanner.fragment_scanner;
import :source_registry;

export import :source_scanner_entry;

export namespace bioseq::storage {
/// Open a sequence source provider for a table scan.
/// @param owning_relation Open table whose identifiers are copied.
/// @param authoritative_registered_snapshot Keep this MVCC snapshot registered
/// until the provider and all counted registry references have closed.
/// @return Provider to release with `close_pg_sequence_source_provider`.
/// @throws C++ exception if validation or allocation fails.
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
 * Open a scan provider through an out-of-line function. Other modules can
 * call it without compiling registry and PostgreSQL exception-handling code.
 */
[[nodiscard]] pg_sequence_source_provider*
create_pg_sequence_source_provider_owner_boundary(
    Relation owning_relation,
    Snapshot authoritative_registered_snapshot)
{
    return create_pg_sequence_source_provider(owning_relation,
                                              authoritative_registered_snapshot);
}

/// Open a sequence source provider for a standalone index operation.
/// @param owning_relation Open table whose identifiers are copied.
/// @param authoritative_registered_snapshot Keep this MVCC snapshot registered
/// until the provider and all counted registry references have closed.
/// @return Provider to release with `close_pg_sequence_source_provider`.
/// @throws C++ exception if validation or allocation fails.
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

/// Acquire another counted reference to a live source registry.
/// @param parent Live reference owned by the current backend thread.
/// @return Reference the caller must release, or an empty reference when
/// called from the wrong backend thread.
/// @throws PostgreSQL ERROR for a stale reference or count overflow.
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

/// Acquire a registry reference from an open provider.
/// @param provider Open provider owned by the current backend thread.
/// @return Reference the caller must release, or an empty reference on the wrong thread.
/// @throws PostgreSQL ERROR if the provider is closed or its reference is invalid.
[[nodiscard]] inline pg_sequence_source_registry_lease
acquire_pg_sequence_source_registry_lease(const pg_sequence_source_provider* provider) {
    if (provider == nullptr || !provider->open) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq source provider is not open")));
    }
    return acquire_pg_sequence_source_registry_lease(provider->lease);
}

/// Release a registry reference and clean up unused read domains.
/// @param lease Token cleared after release. A wrong-thread call leaves it intact.
/// @param boundary Cleanup event, including rescan where readers may be reused.
/// @param is_commit Report cleanup errors when true. Suppress them during abort.
/// @return Whether the reference count and backend-thread checks passed.
/// @throws PostgreSQL ERROR if commit cleanup fails.
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

/// Release a registry reference during abort, suppressing PostgreSQL errors.
/// Clear the token even on the wrong thread, leaving backend cleanup to
/// reclaim its resources.
/// @param lease Reference cleared by the abort path.
/// @param boundary Unused.
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

/// Close and free a source provider. A null pointer does nothing.
/// @param is_commit Report cleanup errors when true. Suppress them during abort.
/// @throws PostgreSQL ERROR if commit cleanup fails or detects invalid state.
/// @param provider Provider to close, or null. Invalid after a successful return.
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

/// Clean up unused read domains while keeping this registry reference.
/// @param boundary Cleanup event. A rescan can keep matching readers for reuse.
/// @param is_commit Report cleanup errors when true. Suppress them during abort.
/// @throws PostgreSQL ERROR for an invalid reference or failed commit cleanup.
/// @param lease Live registry reference held by the caller.
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

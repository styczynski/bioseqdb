/**
 * @file scanner/source_scanner/provider.cppm
 * @brief Source-provider and registry-lease lifecycle.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
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

export module bioseq.postgres.storage.scanner:source_scanner_provider;

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
import :fragment_scanner;
import :source_registry;

export import :source_scanner_entry;

export namespace bioseq::storage {
/// Create a source provider owned by a table scan.
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

/// Create a source provider owned by standalone index administration.
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

/** Rebind a scan provider to the registered snapshot restored from its
 * parallel DSM coordinate.  Upper owners may already retain leases on this
 * registry, so the registry is updated in place rather than replaced.  No
 * source domain may have observed the preceding snapshot. */
inline void
rebind_pg_sequence_source_provider_snapshot(pg_sequence_source_provider* provider,
                                            Snapshot authoritative_registered_snapshot) {
    using namespace sequence_source_detail;
    if (provider == nullptr || !provider->open ||
        provider->kind != pg_sequence_source_provider_kind::scan ||
        !registry_lease_is_live(provider->lease)) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq scan source provider cannot rebind its snapshot")));
    }
    auto* registry = provider->lease.registry;
    if (authoritative_registered_snapshot == nullptr ||
        !IsMVCCSnapshot(authoritative_registered_snapshot) ||
        authoritative_registered_snapshot->regd_count == 0 ||
        registry->authoritative_snapshot == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq scan source provider requires a registered MVCC snapshot")));
    }
    if (registry->domains != nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq scan source provider cannot rebind after source activation")));
    }
    registry->authoritative_snapshot = authoritative_registered_snapshot;
}

/// Acquire an independent upper-owner lease from another live lease.
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

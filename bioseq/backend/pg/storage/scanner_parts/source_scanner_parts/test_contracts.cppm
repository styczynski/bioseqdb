/**
 * @file test_contracts.cppm
 * @brief Deterministic source-scanner failure and lifecycle contracts.
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

export module bioseq.postgres.storage.scanner:source_scanner_test_contracts;

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

export import :source_scanner_promotion;

export namespace bioseq::storage {
#ifdef BIOSEQ_TEST_INDEXES
/**
 * Validate on a real TBB arena worker that the source callback rejects
 * before any PostgreSQL API, cache, snapshot, or logging side effect.
 */
[[nodiscard]] inline bool validate_pg_sequence_source_wrong_thread() {
    sequence_source_detail::pg_sequence_source_domain domain{};
    domain.backend_thread = pg_sequence_backend_thread_token::capture();
    pg_sequence_read_session session{};
    session.backend_thread = pg_sequence_backend_thread_token::capture();
    struct validation_argument {
        sequence_source_detail::pg_sequence_source_domain* domain;
        pg_sequence_read_session* session;
    } argument{&domain, &session};
    const bool rejected = bioseq::parallelism::global_pool::instance().invoke_worker_probe_noexcept(
        &argument, +[](void* opaque) noexcept {
            auto* validation = static_cast<validation_argument*>(opaque);
            const sequence_locator locator{};
            const compact_sequence_stdc_type_params stored_type{};
            return sequence_source_detail::read_domain_ranks(validation->domain,
                                                             *validation->session,
                                                             locator,
                                                             stored_type,
                                                             sequence_transform::identity,
                                                             0u,
                                                             std::span<std::uint8_t>{}) ==
                   pg_sequence_source_read_status::wrong_thread;
        });
    if (rejected) {
        /* Worker diagnostics are thread-local; mirror the observed rejection
         * once on the backend so SQL can assert it without reading worker TLS. */
        ++g_pg_sequence_source_stats.wrong_thread_rejections;
    }
    return rejected;
}

/** Exercise fail-closed retain accounting and merge rollback. */
[[nodiscard]] inline bool validate_pg_sequence_source_fault_contract() noexcept {
    sequence_source_detail::pg_sequence_source_domain domain{};
    domain.lifecycle = sequence_source_detail::domain_lifecycle::live;
    domain.refcount = std::numeric_limits<std::uint32_t>::max();
    domain.registry_token_owned = true;
    sequence_source_detail::retain_domain(&domain);

    pg_sequence_source_registry retained{};
    retained.backend_thread = pg_sequence_backend_thread_token::capture();
    retained.user_relid = static_cast<Oid>(1u);
    retained.generation = 1u;
    retained.active_leases = 1u;
    retained.accepting_lookups = true;
    pg_sequence_source_registry added{};
    added.backend_thread = pg_sequence_backend_thread_token::capture();
    added.user_relid = static_cast<Oid>(2u);
    added.generation = 2u;
    added.active_leases = 1u;
    added.accepting_lookups = true;
    pg_sequence_source_registry overflow{};
    overflow.backend_thread = pg_sequence_backend_thread_token::capture();
    overflow.user_relid = static_cast<Oid>(3u);
    overflow.generation = 3u;
    overflow.active_leases = std::numeric_limits<std::uint32_t>::max();
    overflow.accepting_lookups = true;

    pg_sequence_source_route_set destination{};
    initialize_pg_sequence_source_routes(destination, CurrentMemoryContext);
    destination.inline_routes[0] = pg_sequence_source_route{
        .relation_oid = retained.user_relid,
        .lease =
            pg_sequence_source_registry_lease{
                .registry = &retained,
                .provider_generation = retained.generation,
                .owned = true,
            },
    };
    destination.count = 1u;
    pg_sequence_source_route_set source{};
    source.inline_routes[0] = pg_sequence_source_route{
        .relation_oid = added.user_relid,
        .lease =
            pg_sequence_source_registry_lease{
                .registry = &added,
                .provider_generation = added.generation,
                .owned = true,
            },
    };
    source.inline_routes[1] = pg_sequence_source_route{
        .relation_oid = overflow.user_relid,
        .lease =
            pg_sequence_source_registry_lease{
                .registry = &overflow,
                .provider_generation = overflow.generation,
                .owned = true,
            },
    };
    source.count = 2u;

    volatile bool merge_rejected = false;
    PG_TRY();
    {
        (void)merge_pg_sequence_source_routes(destination, source);
    }
    PG_CATCH();
    {
        FlushErrorState();
        merge_rejected = true;
    }
    PG_END_TRY();
    const auto* destination_entries = pg_sequence_source_routes_data(destination);
    const bool overflow_and_merge_contract =
        domain.refcount_fault &&
        domain.lifecycle == sequence_source_detail::domain_lifecycle::faulted && merge_rejected &&
        overflow.lease_fault && !overflow.accepting_lookups && destination.count == 1u &&
        destination_entries[0].lease.registry == &retained && destination_entries[0].lease.owned &&
        retained.active_leases == 1u && added.active_leases == 1u;
    if (!overflow_and_merge_contract || sequence_source_detail::registries != nullptr ||
        sequence_source_detail::providers != nullptr || !ActiveSnapshotSet() ||
        GetActiveSnapshot() == nullptr || !IsMVCCSnapshot(GetActiveSnapshot())) {
        return false;
    }

    /* Build one real, readable source control, retain it as a piece
     * tree leaf does, and drive the production transaction cleanup callback.
     * The registry token is dropped by cleanup while the retained control
     * survives with refcount one, so the subsequent read reaches the expected
     * transaction-ended guard rather than a test-only approximation. */
    const auto stats_before_xact_end = sequence_source_detail::collect_pg_sequence_source_stats();
    MemoryContext saved_context = CurrentMemoryContext;
    pg_sequence_source_registry* volatile pending_registry = nullptr;
    pg_sequence_source_registry* volatile live_registry = nullptr;
    sequence_source_detail::pg_sequence_source_domain* volatile retained_domain = nullptr;
    volatile bool retained_reference_owned = false;
    volatile bool readable_before_cleanup = false;
    volatile bool cleanup_completed = false;
    volatile bool setup_or_cleanup_failed = false;
    PG_TRY();
    {
        sequence_source_detail::ensure_callbacks_registered();
        auto* registry = static_cast<pg_sequence_source_registry*>(
            MemoryContextAllocZero(TopMemoryContext, sizeof(pg_sequence_source_registry)));
        ::new (static_cast<void*>(registry)) pg_sequence_source_registry{};
        pending_registry = registry;
        registry->backend_thread = pg_sequence_backend_thread_token::capture();
        registry->binding_generation = sequence_source_detail::allocate_generation();
        registry->authoritative_snapshot = GetActiveSnapshot();
        registry->creating_subid = GetCurrentSubTransactionId();
        registry->generation = sequence_source_detail::allocate_generation();
        registry->accepting_lookups = true;
        registry->next = sequence_source_detail::registries;
        sequence_source_detail::registries = registry;
        live_registry = registry;
        pending_registry = nullptr;
        ++g_pg_sequence_source_stats.registries_created;

        auto* source_domain = sequence_source_detail::create_domain(
            *registry, static_cast<AttrNumber>(1), compact_sequence_stdc_type_params{});
        retained_domain = source_domain;
        readable_before_cleanup = sequence_source_detail::domain_is_readable(*source_domain);
        sequence_source_detail::retain_domain(source_domain);
        retained_reference_owned = true;

        sequence_source_detail::xact_callback(XACT_EVENT_PRE_COMMIT, nullptr);
        live_registry = nullptr;
        cleanup_completed =
            sequence_source_detail::registries == nullptr &&
            source_domain->lifecycle == sequence_source_detail::domain_lifecycle::invalid &&
            source_domain->refcount == 1u && !source_domain->registry_token_owned &&
            source_domain->registry == nullptr;
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_context);
        FlushErrorState();
        setup_or_cleanup_failed = true;
        auto* registry = const_cast<pg_sequence_source_registry*>(live_registry);
        if (registry != nullptr) {
            sequence_source_detail::abort_registry_at_xact_noerror(*registry);
            live_registry = nullptr;
        }
        auto* pending = const_cast<pg_sequence_source_registry*>(pending_registry);
        if (pending != nullptr) {
            pending->backend_thread.~pg_sequence_backend_thread_token();
            pfree(pending);
            pending_registry = nullptr;
        }
    }
    PG_END_TRY();
    MemoryContextSwitchTo(saved_context);

    volatile bool transaction_end_error_observed = false;
    if (!setup_or_cleanup_failed && cleanup_completed && readable_before_cleanup) {
        pg_sequence_read_session session{};
        session.backend_thread = pg_sequence_backend_thread_token::capture();
        PG_TRY();
        {
            (void)sequence_source_detail::read_domain_ranks_on_backend(
                const_cast<sequence_source_detail::pg_sequence_source_domain*>(retained_domain),
                session,
                sequence_locator{},
                compact_sequence_stdc_type_params{},
                sequence_transform::identity,
                0u,
                std::span<std::uint8_t>{});
        }
        PG_CATCH();
        {
            MemoryContextSwitchTo(saved_context);
            ErrorData* error = CopyErrorData();
            transaction_end_error_observed =
                error != nullptr && error->sqlerrcode == ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE &&
                error->message != nullptr &&
                std::strcmp(error->message, "bioseq source transaction has ended") == 0;
            FlushErrorState();
            if (error != nullptr)
                FreeErrorData(error);
        }
        PG_END_TRY();
        MemoryContextSwitchTo(saved_context);
    }

    auto* source_domain =
        const_cast<sequence_source_detail::pg_sequence_source_domain*>(retained_domain);
    if (retained_reference_owned && source_domain != nullptr) {
        sequence_source_detail::release_domain(source_domain);
        retained_reference_owned = false;
        retained_domain = nullptr;
    }
    const auto stats_after_xact_end = sequence_source_detail::collect_pg_sequence_source_stats();
    return !setup_or_cleanup_failed && readable_before_cleanup && cleanup_completed &&
           transaction_end_error_observed &&
           stats_after_xact_end.registries_created ==
               stats_before_xact_end.registries_created + 1u &&
           stats_after_xact_end.registries_destroyed ==
               stats_before_xact_end.registries_destroyed + 1u &&
           stats_after_xact_end.domains_created == stats_before_xact_end.domains_created + 1u &&
           stats_after_xact_end.domains_destroyed == stats_before_xact_end.domains_destroyed + 1u &&
           stats_after_xact_end.cache_sets_created ==
               stats_before_xact_end.cache_sets_created + 1u &&
           stats_after_xact_end.cache_sets_destroyed ==
               stats_before_xact_end.cache_sets_destroyed + 1u &&
           stats_after_xact_end.snapshots_registered ==
               stats_before_xact_end.snapshots_registered + 1u &&
           stats_after_xact_end.snapshots_unregistered ==
               stats_before_xact_end.snapshots_unregistered + 1u &&
           stats_after_xact_end.cleanup_commit == stats_before_xact_end.cleanup_commit + 1u &&
           stats_after_xact_end.release_before_locks ==
               stats_before_xact_end.release_before_locks + 1u &&
           stats_after_xact_end.release_locks == stats_before_xact_end.release_locks + 1u &&
           stats_after_xact_end.release_after_locks ==
               stats_before_xact_end.release_after_locks + 1u &&
           stats_after_xact_end.refcount_faults == stats_before_xact_end.refcount_faults &&
           stats_after_xact_end.lease_faults == stats_before_xact_end.lease_faults &&
           stats_after_xact_end.leases_acquired == stats_before_xact_end.leases_acquired &&
           stats_after_xact_end.leases_released == stats_before_xact_end.leases_released &&
           stats_after_xact_end.live_registries == stats_before_xact_end.live_registries &&
           stats_after_xact_end.live_providers == stats_before_xact_end.live_providers &&
           stats_after_xact_end.live_domains == stats_before_xact_end.live_domains &&
           stats_after_xact_end.live_leases == stats_before_xact_end.live_leases &&
           stats_after_xact_end.live_cache_sets == stats_before_xact_end.live_cache_sets &&
           stats_after_xact_end.live_snapshots == stats_before_xact_end.live_snapshots;
}

/** Validate that self-join aliases retain independent routes while the
 * relation-only lookup refuses to guess between them. */
[[nodiscard]] inline bool validate_pg_sequence_source_route_aliases() noexcept {
    constexpr Oid relation_oid = static_cast<Oid>(1u);
    pg_sequence_source_registry first{};
    first.backend_thread = pg_sequence_backend_thread_token::capture();
    first.user_relid = relation_oid;
    first.generation = 41u;
    first.active_leases = 1u;
    first.accepting_lookups = true;
    pg_sequence_source_registry second{};
    second.backend_thread = pg_sequence_backend_thread_token::capture();
    second.user_relid = relation_oid;
    second.generation = 42u;
    second.active_leases = 1u;
    second.accepting_lookups = true;

    pg_sequence_source_route_set routes{};
    routes.inline_routes[0] = pg_sequence_source_route{
        .relation_oid = relation_oid,
        .lease =
            pg_sequence_source_registry_lease{
                .registry = &first,
                .provider_generation = first.generation,
                .owned = true,
            },
    };
    routes.inline_routes[1] = pg_sequence_source_route{
        .relation_oid = relation_oid,
        .lease =
            pg_sequence_source_registry_lease{
                .registry = &second,
                .provider_generation = second.generation,
                .owned = true,
            },
    };
    routes.count = 2u;

    return find_pg_sequence_source_route(routes, relation_oid) == nullptr &&
           find_pg_sequence_source_route(routes, relation_oid, first.generation) ==
               &routes.inline_routes[0].lease &&
           find_pg_sequence_source_route(routes, relation_oid, second.generation) ==
               &routes.inline_routes[1].lease &&
           find_pg_sequence_source_route(routes, relation_oid, 43u) == nullptr;
}

/** Install one inert-but-live token and drive the real PRE_PREPARE switch. */
inline void validate_pg_sequence_source_pre_prepare() {
    using namespace sequence_source_detail;
    ensure_callbacks_registered();
    auto* registry = static_cast<pg_sequence_source_registry*>(
        MemoryContextAllocZero(TopMemoryContext, sizeof(pg_sequence_source_registry)));
    ::new (static_cast<void*>(registry)) pg_sequence_source_registry{};
    registry->backend_thread = pg_sequence_backend_thread_token::capture();
    registry->creating_subid = GetCurrentSubTransactionId();
    registry->generation = allocate_generation();
    registry->binding_generation = 1u;
    registry->accepting_lookups = true;

    pg_sequence_source_domain* volatile pending_domain = nullptr;
    PG_TRY();
    {
        auto* domain = static_cast<pg_sequence_source_domain*>(
            MemoryContextAllocZero(TopMemoryContext, sizeof(pg_sequence_source_domain)));
        pending_domain = domain;
        ::new (static_cast<void*>(domain)) pg_sequence_source_domain{};
        domain->backend_thread = pg_sequence_backend_thread_token::capture();
        domain->registry = registry;
        domain->creating_subid = registry->creating_subid;
        domain->generation = allocate_generation();
        domain->binding_generation = registry->binding_generation;
        domain->refcount = 1u;
        domain->registry_token_owned = true;
        domain->lifecycle = domain_lifecycle::live;
        registry->domains = domain;
        registry->next = registries;
        registries = registry;
        pending_domain = nullptr;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.registries_created;
        ++g_pg_sequence_source_stats.domains_created;
#endif
    }
    PG_CATCH();
    {
        auto* domain = const_cast<pg_sequence_source_domain*>(pending_domain);
        if (domain != nullptr) {
            domain->backend_thread.~pg_sequence_backend_thread_token();
            pfree(domain);
        }
        registry->backend_thread.~pg_sequence_backend_thread_token();
        pfree(registry);
        PG_RE_THROW();
    }
    PG_END_TRY();
    xact_callback(XACT_EVENT_PRE_PREPARE, nullptr);
    ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq PRE_PREPARE source-domain validation did not reject")));
}
#endif

} // namespace bioseq::storage

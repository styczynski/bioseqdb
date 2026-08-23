/**
 * @file scanner/source_scanner/promotion.cppm
 * @brief Locator promotion through authenticated source routes.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/attnum.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "fmgr.h"
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

export module bioseq.postgres.storage.scanner:source_scanner_promotion;

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

export import :source_scanner_routes;

export namespace bioseq::storage {
/// Check whether a registry lease belongs to the current backend thread.
/// @param lease Lease whose backend token is inspected.
/// @return Whether it names a registry bound to this thread.
[[nodiscard]] inline bool pg_sequence_source_registry_is_current_thread(
    const pg_sequence_source_registry_lease& lease) noexcept {
    return lease.registry != nullptr && lease.registry->backend_thread.is_current();
}

/**
 * Validate the exact statement provider identity required by typed dispatch.
 *
 * This is deliberately out of line in the scanner owner so importing
 * scheduler modules do not instantiate the scanner's PostgreSQL bridge and
 * standard-library implementation details.
 */
[[gnu::noinline]] void
validate_exact_pg_sequence_source_provider(pg_sequence_source_provider* provider,
                                           Oid user_relid,
                                           Oid database_oid,
                                           Oid tablespace_oid,
                                           Oid main_relfilenode,
                                           Snapshot snapshot,
                                           MemoryContext parent_mctx,
                                           AttrNumber column_attno) {
    auto* registry = provider != nullptr ? provider->lease.registry : nullptr;
    const bool exact_provider =
        provider != nullptr && provider->open &&
        provider->kind == pg_sequence_source_provider_kind::scan && provider->lease.owned &&
        registry != nullptr && provider->lease.provider_generation == registry->generation &&
        registry->accepting_lookups && !registry->provider_closed && !registry->lease_fault &&
        !registry->xact_ended && registry->backend_thread.is_current() &&
        registry->user_relid == user_relid && registry->database_oid == database_oid &&
        registry->tablespace_oid == tablespace_oid &&
        registry->main_relfilenode == main_relfilenode &&
        registry->authoritative_snapshot == snapshot && snapshot != nullptr &&
        IsMVCCSnapshot(snapshot) && snapshot->regd_count > 0 && column_attno > 0 &&
        parent_mctx != nullptr;
    if (!exact_provider) {
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg_internal(
                         "%s", "locator sequence has no exact statement source-provider route")));
        });
        pg_unreachable();
    }
}

/// Validate a locator against its immutable source and return a source-backed
/// value allocated in the requested result context.
/// @param lease Live authenticated registry lease for the parent relation.
/// @param column_attno One-based sequence attribute number.
/// @param locator_datum Locator-stub datum to validate.
/// @param result_context Memory context that owns the returned expanded datum.
/// @param trace Optional trace attachment forwarded to the expanded value.
/// @param producer_value_id Optional trace identity for the source value.
/// @return Source-backed PostgreSQL datum owned by `result_context`.
/// @throws PostgreSQL ERROR when lease, locator geometry, snapshot-visible
/// source identity, or physical storage identity fails validation.
///
/// The expanded datum retains its source domain, but reads remain confined to
/// the registry's backend thread and authoritative transaction snapshot.
[[nodiscard]] inline Datum
promote_pg_sequence_locator_impl(const pg_sequence_source_registry_lease& lease,
                                 AttrNumber column_attno,
                                 Datum locator_datum,
                                 MemoryContext result_context,
                                 slice_trace_attachment* trace = nullptr,
                                 std::uint32_t producer_value_id = 0u) {
    using namespace sequence_source_detail;
    if (!registry_lease_is_live(lease) || column_attno <= 0 || result_context == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq locator promotion has no live source-registry lease")));
    }
    auto& registry = *lease.registry;
    auto* header = bioseq::postgres::support::invoke_pg_as_cpp([locator_datum] {
        return reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(locator_datum));
    });
    const pg_sequence_descriptor descriptor =
        classify_and_describe_sequence(PointerGetDatum(header));
    if (descriptor.kind != pg_sequence_datum_kind::locator) {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("bioseq source promotion requires a locator stub")));
    }
    const sequence_locator locator = decode_locator_from_stub(header);
    if (locator.logical_length >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq locator logical length exceeds bigint")));
    }
    const compact_sequence_stdc_type_params stored_type{
        .alphabet_code = descriptor.alphabet_code,
        .storage_unit_byte_width = descriptor.storage_unit_byte_width,
    };
    const auto last_padding_letters = static_cast<std::uint16_t>(header->padding_letters);
    if (!locator_geometry_is_valid(locator, stored_type, last_padding_letters)) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq locator geometry does not match its sequence representation")));
    }
    const auto source_row = bioseq::postgres::support::invoke_pg_as_cpp(
        [source_epoch = locator.source_epoch,
         authoritative_snapshot = registry.authoritative_snapshot] {
            return sequence_source_version_manager::lookup(source_epoch,
                                                           authoritative_snapshot,
                                                           /*require_ready=*/true);
        });
    if (!source_row.has_value() ||
        !source_row_matches_registry(
            registry, column_attno, locator, stored_type, last_padding_letters, *source_row)) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq locator no longer matches its immutable source"),
                 errdetail("relation, column, snapshot, storage identity, source "
                           "epoch, locator, or type metadata changed")));
    }

    auto* domain = find_or_create_domain(registry, column_attno, stored_type);
    const pg_sequence_source_handle handle{
        .state = domain,
        .ops = &source_backend_ops(),
    };
    return bioseq::postgres::support::invoke_pg_as_cpp([handle,
                                                        locator,
                                                        stored_type,
                                                        last_padding_letters,
                                                        result_context,
                                                        trace,
                                                        producer_value_id] {
        MemoryContextSwitchTo(result_context);
        return make_source_backed_pg_sequence(
            handle, locator, stored_type, last_padding_letters, trace, producer_value_id);
    });
}

/// Promote a locator through one authenticated registry lease.
/// @return Source-backed datum owned by `result_context`.
/// @throws PostgreSQL ERROR when promotion validation fails.
[[nodiscard]] inline Datum
promote_pg_sequence_locator(const pg_sequence_source_registry_lease& lease,
                            AttrNumber column_attno,
                            Datum locator_datum,
                            MemoryContext result_context,
                            slice_trace_attachment* trace = nullptr,
                            std::uint32_t producer_value_id = 0u) {
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [&lease, column_attno, locator_datum, result_context, trace, producer_value_id] {
            return promote_pg_sequence_locator_impl(
                lease, column_attno, locator_datum, result_context, trace, producer_value_id);
        });
}

/// Promote a locator through an open source provider.
/// @param provider Open provider owned by the current backend thread.
/// @return Source-backed datum owned by `result_context`.
/// @throws PostgreSQL ERROR when the provider is closed or promotion fails.
[[nodiscard]] inline Datum promote_pg_sequence_locator(pg_sequence_source_provider* provider,
                                                       AttrNumber column_attno,
                                                       Datum locator_datum,
                                                       MemoryContext result_context,
                                                       slice_trace_attachment* trace = nullptr,
                                                       std::uint32_t producer_value_id = 0u) {
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [provider, column_attno, locator_datum, result_context, trace, producer_value_id] {
            if (provider == nullptr || !provider->open) {
                ereport(ERROR,
                        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                         errmsg("bioseq locator promotion has no live owning provider")));
            }
            return promote_pg_sequence_locator(provider->lease,
                                               column_attno,
                                               locator_datum,
                                               result_context,
                                               trace,
                                               producer_value_id);
        });
}

/**
 * Promote through an open provider without exposing the inline scanner body to
 * cross-module coordinators.
 */
[[nodiscard]] [[gnu::noinline]] Datum
promote_pg_sequence_source_provider_locator(pg_sequence_source_provider* provider,
                                            AttrNumber column_attno,
                                            Datum locator_datum,
                                            MemoryContext result_context) {
    return promote_pg_sequence_locator(provider, column_attno, locator_datum, result_context);
}

/**
 * Promote through an already validated provider using the canonical scanner
 * implementation, without exporting its inline implementation to the caller.
 */
[[nodiscard]] [[gnu::noinline]] Datum
promote_validated_pg_sequence_source_provider_locator(pg_sequence_source_provider* provider,
                                                      AttrNumber column_attno,
                                                      Datum locator_datum,
                                                      MemoryContext result_context) {
    return promote_pg_sequence_locator(provider, column_attno, locator_datum, result_context);
}

/// Promote a locator through the route selected by relation and generation.
/// @param routes Borrowed authenticated route set.
/// @param relation_oid Parent relation identity.
/// @param provider_generation Exact scan-binding generation.
/// @return Source-backed datum owned by `result_context`.
/// @throws PostgreSQL ERROR when no exact live route exists or promotion fails.
[[nodiscard]] inline Datum promote_pg_sequence_locator(const pg_sequence_source_route_set& routes,
                                                       Oid relation_oid,
                                                       std::uint64_t provider_generation,
                                                       AttrNumber column_attno,
                                                       Datum locator_datum,
                                                       MemoryContext result_context,
                                                       slice_trace_attachment* trace = nullptr,
                                                       std::uint32_t producer_value_id = 0u) {
    return bioseq::postgres::support::invoke_pg_as_cpp([&routes,
                                                        relation_oid,
                                                        provider_generation,
                                                        column_attno,
                                                        locator_datum,
                                                        result_context,
                                                        trace,
                                                        producer_value_id] {
        const auto* lease =
            find_pg_sequence_source_route(routes, relation_oid, provider_generation);
        if (lease == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("bioseq locator has no matching source-provider route"),
                     errdetail("relation %u and provider generation %llu do "
                               "not identify one live registry",
                               relation_oid,
                               static_cast<unsigned long long>(provider_generation))));
        }
        return promote_pg_sequence_locator(
            *lease, column_attno, locator_datum, result_context, trace, producer_value_id);
    });
}

/// Promote a locator only when its relation has one unambiguous live route.
/// @param routes Borrowed authenticated route set.
/// @param relation_oid Parent relation identity.
/// @return Source-backed datum owned by `result_context`.
/// @throws PostgreSQL ERROR when the relation has zero or multiple live routes
/// or promotion fails.
[[nodiscard]] inline Datum promote_pg_sequence_locator(const pg_sequence_source_route_set& routes,
                                                       Oid relation_oid,
                                                       AttrNumber column_attno,
                                                       Datum locator_datum,
                                                       MemoryContext result_context,
                                                       slice_trace_attachment* trace = nullptr,
                                                       std::uint32_t producer_value_id = 0u) {
    return bioseq::postgres::support::invoke_pg_as_cpp([&routes,
                                                        relation_oid,
                                                        column_attno,
                                                        locator_datum,
                                                        result_context,
                                                        trace,
                                                        producer_value_id] {
        const auto* lease = find_pg_sequence_source_route(routes, relation_oid);
        if (lease == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("bioseq locator has no unique source-provider route"),
                     errdetail("relation %u has no live route or is represented "
                               "by multiple scan bindings",
                               relation_oid)));
        }
        return promote_pg_sequence_locator(
            *lease, column_attno, locator_datum, result_context, trace, producer_value_id);
    });
}

} // namespace bioseq::storage

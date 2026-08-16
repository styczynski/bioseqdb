/**
 * @file scanner/source_scanner/promotion.cppm
 * @brief Locator promotion through authenticated source routes.
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
import :fragment_scanner;
import :source_registry;

export import :source_scanner_routes;

export namespace bioseq::storage {
/// Check whether a registry lease belongs to the current backend thread.
[[nodiscard]] inline bool pg_sequence_source_registry_is_current_thread(
    const pg_sequence_source_registry_lease& lease) noexcept {
    return lease.registry != nullptr && lease.registry->backend_thread.is_current();
}

/// Validate a locator against its immutable source and return a source-backed
/// value allocated in the requested result context.
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

/// Promote a locator through the route selected by relation and generation.
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

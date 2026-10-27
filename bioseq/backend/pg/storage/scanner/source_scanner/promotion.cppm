/**
 * @file scanner/source_scanner/promotion.cppm
 * @brief Turn stored locators into sequence values that can read their fragments.
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
import bioseq.common.seqan_adapter;
import bioseq.postgres.storage.manager; // Source version catalog lookup.
import bioseq.storage.scanner;          // Source registry and read-domain interfaces.
import bioseq.storage.lazy_sequence;    // Expanded values that read sequence fragments on demand.
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

export import :source_scanner_routes;

export namespace bioseq::storage {
/// @return Whether this reference points to a registry on the current thread
/// This checks the thrxad only, not whether  the reference is still live.
/// @param lease Reference whose backend thread is checked.
[[nodiscard]] inline bool pg_sequence_source_registry_is_current_thread(
    const pg_sequence_source_registry_lease& lease) noexcept {
    return lease.registry != nullptr && lease.registry->backend_thread.is_current();
}

/**
 * Check that a scan provider matches the requested table and statement snapshot.
 * Keep this out of line so scheduler modules can call it without compiling
 * the scanner's PostgreSQL error handling code.
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

/// Validate a stored locator and create a value that reads its fragments on demand.
/// Look up its source version using the registry's snapshot, then check the
/// table, column, storage ID and sequence metadata against  that catalog row.
/// @param lease Live reference to the parent table's source registry.
/// @param column_attno One-based sequence column number.
/// @param locator_datum Stored locator stub.
/// @param result_context Context owning the returned expanded sequence, value.
/// @param trace Optional trace attached to that value.
/// @param producer_value_id Optional trace ID for the source value.
/// @return Expanded sequence value owned by `result_context`.
///
/// The result holds a counted read-domain reference. Use it on the registry's
/// backend thread while its transaction and snapshot remain valid.
/// @throws An error if source validation  or vaue creation fails.
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
        .encoding = descriptor.encoding,
        .space = descriptor.space,
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

/// Create a sequence value from a locator using a counted registry reference
/// @return Expanded value owned by `result_context`.
/// @throws C++ exception if validation or creation fails.
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

/// Create a sequence value from a locator using an open provider.
/// @return Expanded value owned by `result_context`.
/// @throws C++ exception if the provider is closed or source validation fails.
/// @param provider Open provider on the current backend thread.
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

/** Open a locator as a sequence value without compiling scanner code in the caller. */
[[nodiscard]] [[gnu::noinline]] Datum
promote_pg_sequence_source_provider_locator(pg_sequence_source_provider* provider,
                                            AttrNumber column_attno,
                                            Datum locator_datum,
                                            MemoryContext result_context) {
    return promote_pg_sequence_locator(provider, column_attno, locator_datum, result_context);
}

/**
 * Open a  locator through a provider already checked by the caller.
 * Source validation still runs when the expanded sequence value is created.
 */
[[nodiscard]] [[gnu::noinline]] Datum
promote_validated_pg_sequence_source_provider_locator(pg_sequence_source_provider* provider,
                                                      AttrNumber column_attno,
                                                      Datum locator_datum,
                                                      MemoryContext result_context) {
    return promote_pg_sequence_locator(provider, column_attno, locator_datum, result_context);
}

/// Open a locator through the route for this table and provider generation.
/// @param provider_generation Distinguishes separate scans of the same table.
/// @return Expanded sequence value owned by `result_context`.
/// @throws C++ exception if the route is missing, invalid or source validation fails.
/// @param routes Set of source regstry references.
/// @param relation_oid Parent table OID.
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

/// Open  a locator when exactly one source registry matches its table.
/// @return Expanded sequence value owned by `result_context`.
/// @throws C++ exception if there is no unique live route or source, validation fails.
/// @param routes Set of source registry references.
/// @param relation_oid Parent table OID.
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

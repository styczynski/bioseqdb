/**
 * @file reads_sidecars.cppm
 * @brief Fragmented sequence reads and co-sharded sidecar storage.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/genam.h"
#include "access/xact.h"
#include "executor/spi.h"
#include "storage/lmgr.h"
#include "utils/snapmgr.h"
#include "utils/resowner.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/indexing.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_opclass_d.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_type_d.h"
#include "commands/extension.h"
#include "commands/defrem.h"
#include "commands/sequence.h"
#include "commands/vacuum.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "parser/analyze.h"
#include "parser/parser.h"
#include "rewrite/rewriteHandler.h"
#include "utils/tuplestore.h"
}

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.postgres.storage.manager:sequence_storage_reads_sidecars;

import bioseq.common.logging;
import bioseq.postgres.support;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.configuration.core;
import bioseq.postgres.configuration.storage;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.physical;
import bioseq.postgres.table.io;
import bioseq.postgres.types.compact_sequence;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.indexing.storage;
import bioseq.common.seqan_adapter;  // ceil_log2 used in letters_per_unit_for
import bioseq.storage.size;
import bioseq.storage.coordinates;
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.storage.manager.sql_rows;
import bioseq.postgres.storage.manager.sql_row_codecs;
import bioseq.postgres.sql.generated.core.storage_manager_sequence_storage_manager.entity_handles;
// The PG-free bucket math and sequence_locator value live in their own
// module so the unit-test TU can link them without the Postgres backend.
// Re-exported here so existing `import bioseq_storage` consumers are unchanged.
import bioseq.storage.layout;
import :sequence_storage_catalog;
import :sequence_storage_layout_cache;
import :sequence_storage_catalog_ddl;
import :sequence_storage_streamed_writes;

namespace bioseq::storage {

/// Open a random-access column reader for one sequence shard.
/// @param shard Open relation whose relcache reference remains caller-owned.
/// @param payload_mctx Memory context that owns the returned reader state.
/// @param snapshot Explicit snapshot used for fragment visibility.
/// @return Reader state that must eventually be passed to `end_column_read`.
[[nodiscard]] inline column_read_state*
begin_sequence_payload_read(Relation shard,
                            MemoryContext payload_mctx,
                            Snapshot snapshot) {
    bioseq::postgres::support::memory_context_scope ctx{payload_mctx};
    TupleDesc td = RelationGetDescr(shard);
    auto attr_needed = bioseq::postgres::support::postgres_bitmapset::
        make_member_range(td->natts);
    bioseq::postgres::support::postgres_integer_list projected;
    for (int i = 0; i < td->natts; ++i) {
        if (!TupleDescAttr(td, i)->attisdropped && attr_needed.has_member(i)) {
            projected.append(static_cast<AttrNumber>(i + 1));
        }
    }
    column_read_state* read_state = begin_column_read(
        shard, td, projected.release(), /*whereClauseList=*/NIL,
        payload_mctx, snapshot, /*randomAccess=*/true, nullptr);
    flush_pending_writes_for_read(read_state);
    attr_needed.clear();
    return read_state;
}

/// Materialize all fragments named by a locator as one flat sequence varlena.
/// @tparam p_length_type Unsigned type used for configured sequence lengths.
/// @tparam p_bucket_minimum_length_bits Smallest configured bucket exponent.
/// @tparam p_bucket_maximum_length_bits Largest configured bucket exponent.
/// @param db_oid Database identity used to resolve the physical shard.
/// @param spc_oid Tablespace identity used to resolve the physical shard.
/// @param main_relfilenode Main-relation file identity used in the shard key.
/// @param column_attno One-based sequence column attribute number.
/// @param loc Locator whose logical length is measured in sequence letters.
/// @param snapshot Explicit snapshot governing every fragment read.
/// @param out_mctx Memory context that owns the returned varlena.
/// @param cache Caller-owned relation and reader cache. It must outlive this call.
/// @return Flat sequence storage allocated in `out_mctx`.
/// @throws PostgreSQL ERROR if geometry is corrupt, a fragment is absent, or
/// the materialized varlena would exceed PostgreSQL allocation limits.
export template<class p_length_type,
         std::size_t p_bucket_minimum_length_bits,
         std::size_t p_bucket_maximum_length_bits>
bioseq::pg_types::seq*
sequence_storage_manager<
    p_length_type,
    p_bucket_minimum_length_bits,
    p_bucket_maximum_length_bits>::fetch_payload(
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    std::int16_t column_attno, const sequence_locator& loc,
    Snapshot snapshot,
    MemoryContext out_mctx, seq_caches* cache)
{
    if (cache == nullptr) {
        bioseq_log<logging_level::error, logging_target::postgres>( "bioseq storage: fetch_payload requires a seq_caches");
    }
    if (snapshot == nullptr) {
        bioseq_log<logging_level::error, logging_target::postgres>(
            "bioseq storage: fetch_payload requires an explicit snapshot");
    }

    validate_locator_flat_materialization_length(loc);
    const bool registered_mvcc_snapshot =
        IsMVCCSnapshot(snapshot) && snapshot->regd_count != 0;
    // SnapshotAny is backend-lifetime static state. Other non-MVCC snapshots
    // (especially caller-stack Dirty snapshots) get a per-call reader so no
    // cache entry can outlive their owner.
    // An unregistered MVCC value can still be used for this synchronous call,
    // but cannot be retained beyond it.
    const bool retain_read_state =
        registered_mvcc_snapshot || snapshot == SnapshotAny;

    // Empty sequence (num_fragments == 0). The locator is the
    // only on-disk artefact; there are no sub-table rows to fetch. Return a
    // minimal empty bioseq::pg_types::seq in `out_mctx`.
    if (loc.fragment_count == 0) {
        bioseq::postgres::support::memory_context_scope ctx{out_mctx};
        bioseq::pg_types::seq* out = static_cast<bioseq::pg_types::seq*>(palloc0(bioseq::pg_types::seq::header_bytes()));
        SET_VARSIZE(out, bioseq::pg_types::seq::header_bytes());
        // alphabet/width/bp_size/padding are all zero — caller-side stub
        // hydration sees an empty payload (compact_sequence_range handles
        // size=0 correctly).
        return out;
    }

    Relation shard = open_sub_table_for_locator(db_oid, spc_oid, main_relfilenode,
        column_attno, loc, AccessShareLock, cache);
    const std::uint64_t cache_key = seq_caches::key_for(
        column_attno,
        static_cast<std::int16_t>(loc.bucket_index),
        static_cast<std::int32_t>(loc.shard_index));
    seq_caches::entry& entry = cache->entries[cache_key];
    entry.rel = shard;

    MemoryContext payload_mctx =
        cache->read_state_mctx != nullptr ? cache->read_state_mctx
                                          : CurrentMemoryContext;
    column_read_state* read_state = nullptr;
    bool transient_read_state = false;
    if (retain_read_state) {
        // Key reuse by the caller-owned source snapshot, not the
        // read-state pointer: flushing may replace read_state->snapshot with
        // its own refreshed registered copy while the owner remains the same.
        if (entry.read_state != nullptr
            && entry.read_snapshot_source != snapshot) {
            end_column_read(entry.read_state);
            entry.read_state = nullptr;
            entry.read_snapshot_source = nullptr;
        }
        if (entry.read_state == nullptr) {
            entry.read_state = begin_sequence_payload_read(
                shard, payload_mctx, snapshot);
            entry.read_snapshot_source = snapshot;
        }
        read_state = entry.read_state;
    } else {
        read_state = begin_sequence_payload_read(
            shard, payload_mctx, snapshot);
        transient_read_state = true;
    }

    // Derive fragment shape from logical alphabet and width on
    // first sub-table row.
    Datum first_vals[1]  = { 0 };
    bool  first_nulls[1] = { true };
    if (!read_column_row_by_number(read_state, loc.first_fragment_row,
                                    first_vals, first_nulls)) {
        bioseq_log<logging_level::error, logging_target::postgres>(
               "bioseq storage: missing first sub-table row {} "
               "(locator bucket={} shard={} num_fragments={})",
               loc.first_fragment_row,
               static_cast<unsigned>(loc.bucket_index),
               loc.shard_index, loc.fragment_count);
    }
    if (first_nulls[0]) {
        bioseq_log<logging_level::error, logging_target::postgres>( "bioseq storage: first sub-table row {} is NULL",
               loc.first_fragment_row);
    }
    bioseq::pg_types::seq* first_raw = reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(first_vals[0]));
    const Size first_raw_bytes = static_cast<Size>(VARSIZE(first_raw));
    if (first_raw_bytes < bioseq::pg_types::seq::header_bytes()) {
        validate_sequence_fragment_frame(
            first_raw_bytes, bioseq::pg_types::seq::header_bytes(),
            loc.first_fragment_row);
    }
    if (is_locator_stub(first_raw)) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq fragment row is itself a locator stub"),
             errdetail("fragment row=%llu",
                 static_cast<unsigned long long>(loc.first_fragment_row))));
    }

    const std::uint8_t logical_alphabet_code =
        static_cast<std::uint8_t>(first_raw->alphabet_code);
    const std::uint8_t logical_byte_width =
        static_cast<std::uint8_t>(first_raw->storage_unit_byte_width);
    if (logical_alphabet_code >= pgseq_supported_alphabets::alphabet_count) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq fragment row has an unsupported alphabet"),
             errdetail("fragment row=%llu alphabet=%u",
                 static_cast<unsigned long long>(loc.first_fragment_row),
                 static_cast<unsigned>(logical_alphabet_code))));
    }
    if (logical_byte_width != 1u && logical_byte_width != 2u
        && logical_byte_width != 4u && logical_byte_width != 8u) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq fragment row has an unsupported storage width"),
             errdetail("fragment row=%llu width=%u",
                 static_cast<unsigned long long>(loc.first_fragment_row),
                 static_cast<unsigned>(logical_byte_width))));
    }

    const auto alpha_code = static_cast<bioseq::pg_types::seq::alphabet_code_t>(logical_alphabet_code);
    const std::size_t lpu = detail::letters_per_unit_for(alpha_code, logical_byte_width);
    if (lpu == 0) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq fragment row has an unsupported sequence type"),
             errdetail("fragment row=%llu alphabet=%u width=%u",
                 static_cast<unsigned long long>(loc.first_fragment_row),
                 static_cast<unsigned>(alpha_code),
                 static_cast<unsigned>(logical_byte_width))));
    }
    const std::size_t bytes_per_unit = static_cast<std::size_t>(logical_byte_width);
    const Size fragment_storage_bytes = checked_mul_size(
        checked_size_cast(detail::k_fragment_storage_units), bytes_per_unit);
    const Size expected_fragment_bytes = checked_add_size(
        bioseq::pg_types::seq::header_bytes(), fragment_storage_bytes);
    validate_sequence_fragment_frame(
        first_raw_bytes, expected_fragment_bytes,
        loc.first_fragment_row);

    // Compute exact payload/flat sizes without overflowing before either
    // PostgreSQL's Size or MaxAllocSize boundary can reject the request.
    const std::uint64_t total_units_u64 = checked_ceil_div(
        loc.logical_length, static_cast<std::uint64_t>(lpu));
    const Size total_units = checked_size_cast(total_units_u64);
    const Size total_payload_bytes = checked_mul_size(
        total_units, bytes_per_unit);
    const Size assembled_varlena_bytes =
        checked_materialization_allocation_size(
            checked_round_up_to_uint64(total_payload_bytes),
            bioseq::pg_types::seq::header_bytes(), "flat sequence");

    const std::uint64_t expected_fragments =
        expected_sequence_fragment_count(
            loc.logical_length, static_cast<std::uint64_t>(lpu),
            detail::k_fragment_storage_units);
    if (expected_fragments != loc.fragment_count) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq locator fragment geometry is inconsistent"),
             errdetail("expected fragments=%llu locator fragments=%u",
                 static_cast<unsigned long long>(expected_fragments),
                 loc.fragment_count)));
    }
    const std::uint64_t fragment_bp =
        static_cast<std::uint64_t>(detail::k_fragment_storage_units)
        * static_cast<std::uint64_t>(lpu);
    const std::uint64_t expected_last_padding =
        (static_cast<std::uint64_t>(lpu)
            - loc.logical_length % static_cast<std::uint64_t>(lpu))
        % static_cast<std::uint64_t>(lpu);
    const std::uint64_t first_expected_bp = loc.fragment_count == 1u
        ? loc.logical_length
        : fragment_bp;
    const std::uint64_t first_expected_padding = loc.fragment_count == 1u
        ? expected_last_padding
        : 0u;
    validate_sequence_fragment_metadata(
        first_raw, logical_alphabet_code, logical_byte_width,
        first_expected_bp, first_expected_padding,
        loc.first_fragment_row);

    bioseq::postgres::support::memory_context_scope ctx{out_mctx};
    bioseq::pg_types::seq* out = static_cast<bioseq::pg_types::seq*>(palloc0(assembled_varlena_bytes));
    SET_VARSIZE(out, assembled_varlena_bytes);
    out->alphabet_code           = static_cast<bioseq::pg_types::seq::alphabet_code_t>(logical_alphabet_code);
    out->storage_unit_byte_width = logical_byte_width;
    out->bp_size                 = static_cast<bioseq::pg_types::seq::bp_size_t>(loc.logical_length);
    // Last-fragment padding is also the padding of the assembled varlena.

    std::uint8_t* const out_data = reinterpret_cast<std::uint8_t*>(out->data);

    // Copy the already-fetched and fully validated first fragment.
    const Size first_copy_bytes =
        std::min(fragment_storage_bytes, total_payload_bytes);
    if (first_copy_bytes > 0) {
        std::memcpy(out_data, first_raw->data, first_copy_bytes);
    }
    out->padding_letters =
        static_cast<std::uint16_t>(expected_last_padding);

    for (std::uint32_t fragment_no = 1; fragment_no < loc.fragment_count; ++fragment_no) {
        // Full hydration of a multi-Gbp target into one varlena walks
        // tens of thousands of fragment rows. Make it cancellable and observable.
        CHECK_FOR_INTERRUPTS();
        if (bioseq::settings::diagnostics::logging::enabled && (fragment_no & 0x3FFF) == 0) {
            bioseq_log_progress<logging_level::debug>("[fetch_payload] hydrated {}/{} fragments",
                            fragment_no, loc.fragment_count);
        }
        const std::uint64_t row_no = loc.first_fragment_row + fragment_no;
        Datum vals[1]  = { 0 };
        bool  nulls[1] = { true };
        if (!read_column_row_by_number(read_state, row_no, vals, nulls)) {
            bioseq_log<logging_level::error, logging_target::postgres>(
                   "bioseq storage: missing sub-table fragment row {} "
                   "(locator bucket={} shard={} fragment={}/{})",
                   row_no,
                   static_cast<unsigned>(loc.bucket_index),
                   loc.shard_index, fragment_no, loc.fragment_count);
        }
        if (nulls[0]) {
            bioseq_log<logging_level::error, logging_target::postgres>( "bioseq storage: sub-table row {} is NULL", row_no);
        }
        bioseq::pg_types::seq* raw = reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(vals[0]));
        const Size raw_bytes = static_cast<Size>(VARSIZE(raw));
        if (raw_bytes < bioseq::pg_types::seq::header_bytes()) {
            validate_sequence_fragment_frame(
                raw_bytes, bioseq::pg_types::seq::header_bytes(), row_no);
        }
        if (is_locator_stub(raw)) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq fragment row is itself a locator stub"),
                 errdetail("fragment row=%llu",
                     static_cast<unsigned long long>(row_no))));
        }
        validate_sequence_fragment_frame(
            raw_bytes, expected_fragment_bytes, row_no);
        const bool is_last = fragment_no + 1u == loc.fragment_count;
        const std::uint64_t expected_bp = is_last
            ? loc.logical_length
                - static_cast<std::uint64_t>(fragment_no) * fragment_bp
            : fragment_bp;
        validate_sequence_fragment_metadata(
            raw, logical_alphabet_code, logical_byte_width,
            expected_bp, is_last ? expected_last_padding : 0u,
            row_no);
        const Size dst_offset = checked_mul_size(
            checked_size_cast(fragment_no), fragment_storage_bytes);
        if (dst_offset > total_payload_bytes) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq locator fragment offset exceeds its payload")));
        }
        const Size copy_bytes =
            std::min(fragment_storage_bytes,
                     total_payload_bytes - dst_offset);
        if (copy_bytes > 0) {
            std::memcpy(out_data + dst_offset, raw->data, copy_bytes);
        }
    }

    if (transient_read_state) {
        end_column_read(read_state);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Bulk fetch.
// ---------------------------------------------------------------------------

/// Materialize a batch of locators after ordering reads by shard and row.
/// @tparam p_length_type Unsigned type used for configured sequence lengths.
/// @tparam p_bucket_minimum_length_bits Smallest configured bucket exponent.
/// @tparam p_bucket_maximum_length_bits Largest configured bucket exponent.
/// @param db_oid Database identity used to resolve physical shards.
/// @param spc_oid Tablespace identity used to resolve physical shards.
/// @param main_relfilenode Main-relation file identity used in shard keys.
/// @param column_attno One-based sequence column attribute number.
/// @param bucket_idx Compatibility value. Each request locator supplies its bucket.
/// @param requests Requests whose `out_slot` values address `out_payloads`.
/// @param out_payloads Destination slots for pointers allocated in `out_mctx`.
/// @param sort_scratch Mutable storage with at least `requests.size()` elements.
/// @param snapshot Explicit snapshot governing every fragment read.
/// @param out_mctx Memory context owning each returned sequence.
/// @param cache Caller-owned relation and reader cache.
/// @throws PostgreSQL ERROR if scratch space is short or any request fails.
export template<class p_length_type,
         std::size_t p_bucket_minimum_length_bits,
         std::size_t p_bucket_maximum_length_bits>
void
sequence_storage_manager<
    p_length_type,
    p_bucket_minimum_length_bits,
    p_bucket_maximum_length_bits>::fetch_payload_bulk(
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    std::int16_t column_attno, std::int16_t bucket_idx,
    std::span<const fetch_request> requests,
    std::span<bioseq::pg_types::seq*>            out_payloads,
    std::span<fetch_request>       sort_scratch,
    Snapshot                       snapshot,
    MemoryContext                  out_mctx,
    seq_caches*                    cache)
{
    (void)bucket_idx;  // The bucket is implicit in each locator; carried for symmetry.
    if (snapshot == nullptr) {
        bioseq_log<logging_level::error, logging_target::postgres>(
            "bioseq storage: fetch_payload_bulk requires an explicit snapshot");
    }
    if (requests.empty()) {
        return;
    }
    if (sort_scratch.size() < requests.size()) {
        bioseq_log<logging_level::error, logging_target::postgres>(
               "bioseq storage: fetch_payload_bulk sort_scratch too small "
               "(have {} need {})", sort_scratch.size(), requests.size());
    }
    std::copy(requests.begin(), requests.end(), sort_scratch.begin());

    // Group physical reads by shard and then by fragment row.
    std::sort(sort_scratch.begin(), sort_scratch.begin() + requests.size(),
        [](const fetch_request& a, const fetch_request& b) noexcept {
            return std::tie(a.locator.shard_index, a.locator.first_fragment_row)
                 < std::tie(b.locator.shard_index, b.locator.first_fragment_row);
        });

    for (std::size_t i = 0; i < requests.size(); ++i) {
        const fetch_request& req = sort_scratch[i];
        out_payloads[req.out_slot] = fetch_payload(
            db_oid, spc_oid, main_relfilenode,
            column_attno, req.locator, snapshot, out_mctx, cache);
    }
}

// ---------------------------------------------------------------------------
// DROP cascade.
// ---------------------------------------------------------------------------

/// Validate the internal catalogs needed before a main-relation drop.
/// @tparam p_length_type Unsigned type used for configured sequence lengths.
/// @tparam p_bucket_minimum_length_bits Smallest configured bucket exponent.
/// @tparam p_bucket_maximum_length_bits Largest configured bucket exponent.
/// @throws PostgreSQL ERROR if either catalog is absent or has an invalid schema.
///
/// Relcache references are closed while both `AccessShareLock` locks remain
/// owned by the current transaction.
export template<class p_length_type,
         std::size_t p_bucket_minimum_length_bits,
         std::size_t p_bucket_maximum_length_bits>
void
sequence_storage_manager<
    p_length_type,
    p_bucket_minimum_length_bits,
    p_bucket_maximum_length_bits>::validate_drop_catalog_contracts()
{
    // Acquire both catalog locks in the same order as the mutating OAT_DROP
    // cleanup and retain them to transaction end. ALTER/DROP of either catalog
    // therefore cannot race between this preflight and PostgreSQL's relation
    // deletion. This function deliberately performs no row deletion, command
    // counter increment, relfilenode bookkeeping, or recursive utility call:
    // an ERROR here must leave every target in a multi-table DROP untouched.
    Relation registry_guard = fragment_registry_detail::open_catalog(
        /*allow_missing_during_extension_teardown=*/false);

    const Oid source_catalog_oid = source_version_detail::relation_oid();
    if (!OidIsValid(source_catalog_oid)) {
        internal_catalog_detail::missing_outside_teardown(
            "bioseq_internal.sequence_source_version");
    }
    Relation source_guard = bioseq::postgres::support::try_open_postgres_relation(
        source_catalog_oid, AccessShareLock);
    if (source_guard == nullptr) {
        internal_catalog_detail::missing_outside_teardown(
            "bioseq_internal.sequence_source_version");
    }
    source_version_detail::validate_catalog_schema(source_guard);

    // Release relcache pins but retain the AccessShare locks. OAT_DROP still
    // revalidates under its own locks as defence in depth.
    table_close(source_guard, NoLock);
    table_close(registry_guard, NoLock);
}

/// Drop fragment and sidecar tables registered for one main relation.
/// @tparam p_length_type Unsigned type used for configured sequence lengths.
/// @tparam p_bucket_minimum_length_bits Smallest configured bucket exponent.
/// @tparam p_bucket_maximum_length_bits Largest configured bucket exponent.
/// @param db_oid Database component of the main-relation identity.
/// @param spc_oid Tablespace component of the main-relation identity.
/// @param main_relfilenode File component of the main-relation identity.
/// @param allow_missing_during_extension_teardown Whether a missing registry is
/// accepted while PostgreSQL removes the extension.
/// @throws PostgreSQL ERROR for catalog validation or SPI failures reported by
/// PostgreSQL.
export template<class p_length_type,
         std::size_t p_bucket_minimum_length_bits,
         std::size_t p_bucket_maximum_length_bits>
void
sequence_storage_manager<
    p_length_type,
    p_bucket_minimum_length_bits,
    p_bucket_maximum_length_bits>::drop_sub_tables_for_main(
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    const bool allow_missing_during_extension_teardown)
{
    // Keep the validated registry relation locked across the manager calls
    // below. Those calls return an empty set when the
    // relation disappears; the guard makes that benign only for the explicit
    // extension-teardown context and closes the validation-to-use race.
    Relation registry_guard = fragment_registry_detail::open_catalog(
        allow_missing_during_extension_teardown);
    if (registry_guard == nullptr) {
        return;
    }
    sequence_source_version_manager::delete_for_main(
        db_oid, spc_oid, main_relfilenode,
        allow_missing_during_extension_teardown);
    const auto rows = sequence_sub_table_manager::list_shards_for_main(db_oid, spc_oid, main_relfilenode);
    if (rows.empty()) {
        table_close(registry_guard, AccessShareLock);
        return;
    }

    constexpr std::size_t k_drop_batch_size = 32;
    StringInfo buf = makeStringInfo();
    for (std::size_t i = 0; i < rows.size(); i += k_drop_batch_size) {
        resetStringInfo(buf);
        appendStringInfoString(buf, "DROP TABLE IF EXISTS ");
        const std::size_t end = std::min(i + k_drop_batch_size, rows.size());
        bool wrote_one = false;
        for (std::size_t j = i; j < end; ++j) {
            if (wrote_one) appendStringInfoString(buf, ", ");
            const std::string sub_name = format_sub_table_name(
                rows[j].database_oid, rows[j].tablespace_oid,
                rows[j].main_relfilenode, rows[j].column_attno,
                rows[j].bucket_idx, rows[j].shard_idx);
            appendStringInfo(buf, "bioseq_internal.%s",
                             quote_identifier(sub_name.c_str()));
            wrote_one = true;
        }
        appendStringInfoString(buf, ";");
        if (SPI_connect() == SPI_OK_CONNECT) {
            SPI_execute(buf->data, false, 0);
            SPI_finish();
        }
    }

    for (std::size_t i = 0; i < rows.size(); i += k_drop_batch_size) {
        resetStringInfo(buf);
        appendStringInfoString(buf, "DROP TABLE IF EXISTS ");
        const std::size_t end = std::min(i + k_drop_batch_size, rows.size());
        bool wrote_one = false;
        for (std::size_t j = i; j < end; ++j) {
            if (wrote_one) appendStringInfoString(buf, ", ");
            const std::string sidecar_name = format_sidecar_name(
                rows[j].database_oid, rows[j].tablespace_oid,
                rows[j].main_relfilenode, rows[j].column_attno,
                rows[j].bucket_idx, rows[j].shard_idx);
            appendStringInfo(buf, "bioseq_internal.%s",
                             quote_identifier(sidecar_name.c_str()));
            wrote_one = true;
        }
        appendStringInfoString(buf, ";");
        if (SPI_connect() == SPI_OK_CONNECT) {
            SPI_execute(buf->data, false, 0);
            SPI_finish();
        }
    }

    sequence_sub_table_manager::delete_shards_for_main(db_oid, spc_oid, main_relfilenode);
    table_close(registry_guard, AccessShareLock);
}

// Explicit instantiation of the default alias so non-template TUs see the
// out-of-line definitions.
template struct sequence_storage_manager<sequence_size_t, 8, 42>;

// ---------------------------------------------------------------------------
// Per-shard sidecar I/O.
// ---------------------------------------------------------------------------

/// Resolve the sidecar relation registered for one physical sequence shard.
/// @return Relation OID, or `InvalidOid` when the shard has no registry row.
static Oid lookup_sidecar_relid(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                                std::int16_t column_attno,
                                std::int16_t bucket_idx, std::int32_t shard_idx)
{
    auto row = sequence_sub_table_manager::lookup_shard(db_oid, spc_oid, main_relfilenode,
                                   column_attno, bucket_idx, shard_idx);
    if (!row.has_value()) {
        return InvalidOid;
    }
    return row->sidecar_relid;
}

/// Insert an owning copy of a locator's sidecar bytes into its shard table.
/// @param column_attno One-based sequence column attribute number.
/// @param loc Locator whose first fragment row is the sidecar key.
/// @param blob Bytes copied into a PostgreSQL `bytea` before insertion.
/// @param cache Reserved cache argument. This implementation does not retain it.
///
/// The function acquires and releases `RowExclusiveLock`. It returns without
/// writing when the registry row or physical sidecar relation is absent.
void sidecar_write_blob(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                        std::int16_t column_attno, const sequence_locator& loc,
                        std::span<const std::uint8_t> blob, seq_caches* /*cache*/)
{
    const Oid sidecar_relid = lookup_sidecar_relid(
        db_oid, spc_oid, main_relfilenode, column_attno,
        static_cast<std::int16_t>(loc.bucket_index),
        static_cast<std::int32_t>(loc.shard_index));
    if (!OidIsValid(sidecar_relid)) {
        return;
    }
    Relation rel = bioseq::postgres::support::try_open_postgres_relation(sidecar_relid, RowExclusiveLock);
    if (rel == nullptr) {
        return;
    }
    bytea* blob_varlena = reinterpret_cast<bytea*>(
        palloc(VARHDRSZ + blob.size()));
    SET_VARSIZE(blob_varlena, VARHDRSZ + blob.size());
    if (!blob.empty()) {
        std::memcpy(VARDATA(blob_varlena), blob.data(), blob.size());
    }
    TupleDesc td = RelationGetDescr(rel);
    Datum values[2] = {
        Int64GetDatum(static_cast<std::int64_t>(loc.first_fragment_row)),
        PointerGetDatum(blob_varlena),
    };
    bool nulls[2] = { false, false };
    HeapTuple tup = heap_form_tuple(td, values, nulls);
    CatalogTupleInsert(rel, tup);
    heap_freetuple(tup);
    pfree(blob_varlena);
    relation_close(rel, RowExclusiveLock);
}

/// Read an owning copy of a locator's sidecar bytes.
/// @param column_attno One-based sequence column attribute number.
/// @param loc Locator whose first fragment row is the sidecar key.
/// @param cache Reserved cache argument. This implementation does not retain it.
/// @return Blob bytes, or no value when the relation or keyed row is absent or
/// the stored blob is SQL NULL.
///
/// The relation is held with `AccessShareLock` only for the duration of the scan.
std::optional<std::vector<char>>
sidecar_read_blob(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                  std::int16_t column_attno, const sequence_locator& loc,
                  seq_caches* /*cache*/)
{
    const Oid sidecar_relid = lookup_sidecar_relid(
        db_oid, spc_oid, main_relfilenode, column_attno,
        static_cast<std::int16_t>(loc.bucket_index),
        static_cast<std::int32_t>(loc.shard_index));
    if (!OidIsValid(sidecar_relid)) {
        return std::nullopt;
    }
    Relation rel = bioseq::postgres::support::try_open_postgres_relation(sidecar_relid, AccessShareLock);
    if (rel == nullptr) {
        return std::nullopt;
    }
    const Oid pk_index_oid = bioseq::postgres::support::find_primary_key_oid(rel);

    ScanKeyData keys[1];
    ScanKeyInit(&keys[0], 1 /*sub_rowno*/,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(loc.first_fragment_row)));
    SysScanDesc scan = systable_beginscan(rel, pk_index_oid,
                                          /*indexOK=*/ pk_index_oid != InvalidOid,
                                          /*snapshot=*/nullptr, 1, keys);
    std::optional<std::vector<char>> out;
    HeapTuple tup = systable_getnext(scan);
    if (HeapTupleIsValid(tup)) {
        bool is_null = false;
        Datum d = heap_getattr(tup, 2 /*blob*/, RelationGetDescr(rel), &is_null);
        if (!is_null) {
            bytea* flat = reinterpret_cast<bytea*>(PG_DETOAST_DATUM(d));
            const std::size_t data_size = VARSIZE_ANY_EXHDR(flat);
            std::vector<char> v(data_size);
            if (data_size > 0) {
                std::memcpy(v.data(), VARDATA_ANY(flat), data_size);
            }
            out = std::move(v);
            if (reinterpret_cast<void*>(flat) !=
                reinterpret_cast<void*>(DatumGetPointer(d))) {
                pfree(flat);
            }
        }
    }
    systable_endscan(scan);
    relation_close(rel, AccessShareLock);
    return out;
}

/// Read a batch of sidecar blobs after ordering requests by shard and row.
/// @param bucket_idx Compatibility value. Each request locator supplies its bucket.
/// @param requests Requests whose `out_slot` values address `out_blobs`.
/// @param out_blobs Owning result slots indexed by each request's `out_slot`.
/// @param sort_scratch Mutable storage with at least `requests.size()` elements.
/// @param out_mctx Reserved memory-context argument. Results use C++ ownership.
/// @param cache Cache forwarded to each scalar read.
/// @throws PostgreSQL ERROR if scratch storage is shorter than the request span.
void sidecar_read_blob_bulk(
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    std::int16_t column_attno, std::int16_t /*bucket_idx*/,
    std::span<const sequence_storage_manager<sequence_size_t, 8, 42>::fetch_request> requests,
    std::span<std::optional<std::vector<char>>> out_blobs,
    std::span<sequence_storage_manager<sequence_size_t, 8, 42>::fetch_request> sort_scratch,
    MemoryContext /*out_mctx*/,
    seq_caches* cache)
{
    if (requests.empty()) {
        return;
    }
    if (sort_scratch.size() < requests.size()) {
        bioseq_log<logging_level::error, logging_target::postgres>(
               "bioseq storage: sidecar_read_blob_bulk sort_scratch too small "
               "(have {} need {})", sort_scratch.size(), requests.size());
    }
    std::copy(requests.begin(), requests.end(), sort_scratch.begin());
    // Keep the comparator readable while preserving the manager's request type.
    using fetch_request_type = sequence_storage_manager<sequence_size_t, 8, 42>::fetch_request;
    // Group physical reads by shard and then by sidecar key.
    std::sort(sort_scratch.begin(), sort_scratch.begin() + requests.size(),
        [](const fetch_request_type& a, const fetch_request_type& b) noexcept {
            return std::tie(a.locator.shard_index, a.locator.first_fragment_row)
                 < std::tie(b.locator.shard_index, b.locator.first_fragment_row);
        });
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const fetch_request_type& req = sort_scratch[i];
        out_blobs[req.out_slot] = sidecar_read_blob(
            db_oid, spc_oid, main_relfilenode,
            column_attno, req.locator, cache);
    }
}

// ---------------------------------------------------------------------------

}  // namespace bioseq::storage

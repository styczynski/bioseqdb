/**
 * @file index_parts/hydration.cppm
 * @brief Defines bucketed locator hydration on the PostgreSQL execution thread.
 */
module;

#include <bioseq/pg_compat.h>
extern "C" {
#include "executor/instrument.h"
#include "utils/snapmgr.h"
}

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The chunked-scanner dispatch path is always compiled as a permanent part of
// the fragmented pipeline tree.
// The `bioseq.testing.chunked_scanner.enabled` GUC routes row sequences through a
// synthetic chunked scanner end-to-end. The
// `bioseq.testing.in_memory_scanner.enabled` GUC forces the materialise-then-
// in_memory baseline. Without either GUC the production path promotes locator
// values through the statement-owned source provider and runs from its
// canonical lazy facade.
module bioseq.postgres.scheduler.dispatch_buffer:hydration;

import bioseq.postgres.scheduler.dispatch_buffer;

import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.postgres.configuration.core;
import bioseq.indexing.type;
import bioseq.postgres.storage.indexing.handles;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.storage.lazy_sequence; // allocation observer for route-aware sequence payload tracing
import bioseq.postgres.scheduler.dispatch_processor; // fixed-shape batch_processor, heavy pipeline in its own module
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.pipeline.minimap_port;
import bioseq.sequences.cigar;
import bioseq.pipeline.pipe;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.storage.manager;
import bioseq.storage.scanner;
import bioseq.storage.layout;
import bioseq.postgres.storage.scanner; // statement-owned source provider for locator targets
import bioseq.parallelism.memory_admission;
import bioseq.postgres.scheduler.supervisor;
import bioseq.postgres.column.table_access;

namespace bioseq::scheduler {

/** @brief Implements the hydrate one column contract declared by the stable buffer facade. */
void tuple_dispatch_buffer::hydrate_one_column(
    std::span<const size_t> rows,
    bioseq::storage::seq_caches* cache,
    std::int16_t column_attno,
    std::array<std::vector<storage_manager_type::fetch_request>,
               storage_manager_type::bucket_count>& per_bucket,
    std::vector<bioseq::pg_types::seq*>& out_payloads,
    bool is_query) {
    for (auto& bucket_requests : per_bucket) {
        bucket_requests.clear();
    }
    // Compute letters_per_unit and storage_unit_byte_width from
    // the batch's (alphabet, width) typmod so the MaxAllocSize gate below
    // works for ANY alphabet / storage_unit_byte_width pair (dna4/uint8,
    // dna5/uint64, aa20/uint32, …) — NOT just the 2-bits-per-letter dna4
    // case. The dispatch happens once per column (not per row).
    //
    // Explicit captures (no `[&]` capture-all) keep the user-
    // visible rule on the hot path is no capture-all lambdas.
    std::size_t lpu_for_column = 0;
    std::size_t bytes_per_unit_for_col = 0;
    const std::uint8_t column_alphabet_code =
        is_query ? mmsearch_state.query_alphabet_code : mmsearch_state.alphabet_code;
    const std::uint8_t column_storage_unit_byte_width =
        is_query ? mmsearch_state.query_storage_unit_byte_width
                 : mmsearch_state.storage_unit_byte_width;
    bioseq::pg_types::seq::dispatch_instance(
        column_alphabet_code,
        column_storage_unit_byte_width,
        [&lpu_for_column, &bytes_per_unit_for_col]<class p_sequence>() noexcept {
            using traits = typename compact_sequence_range<p_sequence>::traits;
            lpu_for_column = traits::letters_per_unit;
            bytes_per_unit_for_col = sizeof(typename traits::storage_unit);
        });
    if (lpu_for_column == 0 || bytes_per_unit_for_col == 0) {
        bioseq_log<logging_level::error, logging_target::postgres>(
            "hydrate_one_column: dispatch_instance returned zero lpu / "
            "bytes_per_unit for (alphabet={}, width={})",
            static_cast<unsigned>(column_alphabet_code),
            static_cast<unsigned>(column_storage_unit_byte_width));
    }
    const size_t batch_size = buffered_entries.size();
    bool has_requests = false;
    for (const size_t row_index : rows) {
        if (row_index >= batch_size) {
            continue;
        }
        const auto& locator = is_query ? buffered_entries[row_index].per_row_query_loc
                                       : buffered_entries[row_index].seq_loc;
        if (!locator.has_value()) {
            continue;
        }
        // D-VARLENA bridge: skip hydration when the
        // assembled varlena would exceed MaxAllocSize. The scanner_pg
        // dispatch path reads such sequences fragment-by-fragment without
        // materialising. Projection of `SELECT seq` on these rows raises
        // bioseq_log<logging_level::error, logging_target::postgres>() from access_method.cppm —
        // that's the documented Oversized payloads remain deferred to the source-backed route.
        //
        // Size = ceil(total_bp / letters_per_unit) * bytes_per_unit + header,
        // where lpu and bytes_per_unit come from the column's (alphabet,
        // width) typmod — generic across all supported alphabets.
        const std::uint64_t total_units =
            lpu_for_column == 0
                ? std::uint64_t{0}
                : (static_cast<std::uint64_t>(locator->logical_length) + lpu_for_column - 1) /
                      lpu_for_column;
        const std::uint64_t projected_payload_bytes =
            total_units * static_cast<std::uint64_t>(bytes_per_unit_for_col);
        const std::uint64_t projected_varlena =
            static_cast<std::uint64_t>(bioseq::pg_types::seq::header_bytes()) +
            projected_payload_bytes;
        if (projected_varlena > static_cast<std::uint64_t>(MaxAllocSize)) {
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: hydrate_one_column row={} attno={} too large "
                "({} bp, lpu={}, byte_width={} ⇒ {} bytes > MaxAllocSize); "
                "deferring to scanner_pg",
                row_index,
                static_cast<int>(column_attno),
                locator->logical_length,
                lpu_for_column,
                bytes_per_unit_for_col,
                projected_varlena);
            continue;
        }
        per_bucket[locator->bucket_index].push_back(
            storage_manager_type::fetch_request{row_index, *locator});
        has_requests = true;
    }
    if (!has_requests) {
        return;
    }
    out_payloads.assign(batch_size, nullptr);
    for (std::uint8_t bucket_index = 0; bucket_index < storage_manager_type::bucket_count;
         ++bucket_index) {
        auto& requests = per_bucket[bucket_index];
        if (requests.empty()) {
            continue;
        }
        storage_manager_type::fetch_payload_bulk(
            user_main_database_oid,
            user_main_tablespace_oid,
            user_main_relfilenode,
            column_attno,
            static_cast<std::int16_t>(bucket_index),
            std::span<const storage_manager_type::fetch_request>{requests.data(), requests.size()},
            std::span<bioseq::pg_types::seq*>{out_payloads.data(), out_payloads.size()},
            std::span<storage_manager_type::fetch_request>{fetch_bulk_sort_scratch.data(),
                                                           fetch_bulk_sort_scratch.size()},
            scan_snapshot,
            batch_payload_ctx,
            cache);
        explain_bucket_touched_mask |= (std::uint64_t{1} << bucket_index);
        ++explain_storage_bulk_fetch_calls;
    }
    for (const size_t row_index : rows) {
        if (row_index >= batch_size) {
            continue;
        }
        if (out_payloads[row_index] == nullptr) {
            continue;
        }
        if (is_query) {
            bioseq::pg_types::seq* source_query = out_payloads[row_index];
            if (source_query->alphabet_code != mmsearch_state.query_alphabet_code ||
                source_query->storage_unit_byte_width !=
                    mmsearch_state.query_storage_unit_byte_width) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATATYPE_MISMATCH),
                         errmsg("bioseq mmsearch hydrated query typmod changed after planning")));
            }
            bioseq::pg_types::seq* dispatch_query = source_query;
            if (source_query->storage_unit_byte_width != mmsearch_state.storage_unit_byte_width) {
                MemoryContext old_context = MemoryContextSwitchTo(batch_payload_ctx);
                dispatch_query = ::bioseq::simd::stdc_rewidth_to_type_params(
                    *source_query,
                    compact_sequence_stdc_type_params{mmsearch_state.alphabet_code,
                                                      mmsearch_state.storage_unit_byte_width},
                    pg_alloc);
                MemoryContextSwitchTo(old_context);
                if (dispatch_query == nullptr) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATATYPE_MISMATCH),
                             errmsg("bioseq mmsearch hydrated query rewidth rejected an "
                                    "incompatible alphabet")));
                }
            }
            buffered_entries[row_index].hydrated_per_row_query_hdr = dispatch_query;
        } else {
            buffered_entries[row_index].hydrated_seq_hdr = out_payloads[row_index];
        }
        TupleTableSlot* scan_slot_copy = buffered_entries[row_index].scan_slot_copy;
        const bool preserve_raw_output =
            is_query ? buffered_entries[row_index].query_from_evaluated_expression
                     : buffered_entries[row_index].target_from_evaluated_expression;
        if (scan_slot_copy != nullptr && !preserve_raw_output) {
            const int attribute_index = column_attno - 1;
            if (attribute_index >= 0 &&
                attribute_index < scan_slot_copy->tts_tupleDescriptor->natts) {
                bioseq::postgres::support::pg_slot_datums_view{scan_slot_copy}.set_raw(
                    column_attno,
                    bioseq::postgres::support::pg_datum::from_pointer(out_payloads[row_index]));
            }
        }
    }
}

/** @brief Implements the hydrate locators by bucket contract declared by the stable buffer facade.
 */
void tuple_dispatch_buffer::hydrate_locators_by_bucket(std::span<const size_t> rows) {
    const size_t batch_size = buffered_entries.size();
    bool any_locator = false;
    for (const size_t row_index : rows) {
        if (row_index >= batch_size) {
            continue;
        }
        if (buffered_entries[row_index].seq_loc.has_value() ||
            buffered_entries[row_index].per_row_query_loc.has_value()) {
            any_locator = true;
            break;
        }
    }
    if (!any_locator) {
        return;
    }
    if (batch_payload_ctx == nullptr) {
        batch_payload_ctx = AllocSetContextCreate(
            executor_query_mctx, "tuple_dispatch_buffer payload arena", ALLOCSET_DEFAULT_SIZES);
    }
    if (fetch_bulk_sort_scratch.size() < batch_size) {
        fetch_bulk_sort_scratch.resize(batch_size);
    }

    bioseq::storage::seq_caches* cache = bioseq::storage::alloc_seq_caches(batch_payload_ctx);
    cache->side = bioseq::storage::seq_caches::access_side::read;
    cache->lock_mode = AccessShareLock;
    cache->read_state_mctx = batch_payload_ctx;
    cache->main_database_oid = user_main_database_oid;
    cache->main_tablespace_oid = user_main_tablespace_oid;
    cache->main_relfilenode = user_main_relfilenode;

    hydrate_one_column(rows,
                       cache,
                       static_cast<std::int16_t>(mmsearch_state.seq_attno),
                       per_bucket_target_requests,
                       hydrated_target_payloads,
                       /*is_query=*/false);

    if (per_row_mode()) {
        hydrate_one_column(rows,
                           cache,
                           static_cast<std::int16_t>(query_source_attno()),
                           per_bucket_query_requests,
                           hydrated_query_payloads,
                           /*is_query=*/true);
    }

    cache->close_all(/*commit=*/false);
    bioseq::storage::destroy_seq_caches(cache);
}

} // namespace bioseq::scheduler

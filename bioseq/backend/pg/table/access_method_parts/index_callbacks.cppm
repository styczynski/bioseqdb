/**
 * @file
 * @brief Build and validate PostgreSQL indexes on Bioseq tables.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/genam.h"
#include "access/skey.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "commands/progress.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "port.h"
#include "storage/itemptr.h"
#include "utils/backend_progress.h"
#include "utils/errcodes.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplesort.h"
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.postgres.table:access_method_index_callbacks;

import bioseq.postgres.storage.physical;
import bioseq.postgres.storage.compression;
import bioseq.common.logging;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.metadata.sql_entities;
import bioseq.postgres.storage.metadata.sql_rows;
import bioseq.postgres.vectorization.types;
import bioseq.postgres.table.types;
import bioseq.postgres.configuration.core;
import bioseq.postgres.table.io;
import bioseq.postgres.support;
import bioseq.postgres.support.sql_boundary;
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.chunked_storage.sql_entities;
import bioseq.postgres.storage.manager;
import bioseq.postgres.storage.manager.sequence_storage.sql_entities;
import bioseq.postgres.storage.manager.sql_rows;
import bioseq.postgres.storage.scanner;
import bioseq.storage.layout;
import bioseq.postgres.table.am_cache;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.column.table_access;
import :access_identity;
import :access_common;
import :access_hooks;
import :access_method_scan_callbacks;

/** Report the table's virtual block count as index-build progress. */
static void
report_total_virtual_blocks(Relation relation, Snapshot snapshot, int progress_arr_index);
/** Count virtual blocks covered by visible stripe metadata. */
static BlockNumber column_virtual_block_count(Relation relation, Snapshot snapshot);
/** Return the highest row TID recorded by visible stripe metadata. */
static ItemPointerData highest_column_item_pointer(Relation relation, Snapshot snapshot);
/** Pass rows that satisfy the index predicate to the index-build callback. */
static double
build_index_from_column_rows(TableScanDesc scan,
                             Relation index_relation,
                             IndexInfo* index_info,
                             bool progress,
                             IndexBuildCallback index_callback,
                             void* index_callback_state,
                             EState* estate,
                             ExprState* predicate,
                             bioseq::storage::pg_sequence_source_provider* source_provider);
/** @brief Insert visible rows absent from the sorted tuple-identifier stream. */
static void
validate_missing_index_rows(TableScanDesc scan,
                            Relation index_relation,
                            IndexInfo* index_info,
                            EState* estate,
                            ExprState* predicate,
                            ValidateIndexState* state,
                            bioseq::storage::pg_sequence_source_provider* source_provider);
/** @brief Advance a sorted tuple-identifier stream to the first entry at or after a target. */
static ItemPointerData skip_item_pointers_before(Tuplesortstate* tuple_sort,
                                                 ItemPointer target_item_pointer);

/** Convert C++ failures into PostgreSQL errors, keeping existing PostgreSQL error details. */
template <class p_function>
decltype(auto) run_index_source_cpp_checked(const char* failure_message, p_function&& function) {
    ErrorData* exact_error = nullptr;
    std::array<char, 512> detail{};
    bool cpp_error = false;
    bool unknown_cpp_error = false;
    try {
        if constexpr (std::is_void_v<std::invoke_result_t<p_function&>>) {
            function();
            return;
        } else {
            return function();
        }
    } catch (bioseq::postgres::support::postgres_error& error) {
        exact_error = error.release_error_data();
    } catch (const std::exception& error) {
        strlcpy(detail.data(),
                error.what() == nullptr ? "std::exception without detail" : error.what(),
                detail.size());
        cpp_error = true;
    } catch (...) {
        unknown_cpp_error = true;
    }

    // C++ cleanup has finished before PostgreSQL raises the error. Reuse
    // existing ErrorData, or report a C++ exception as an internal error.
    if (exact_error != nullptr) {
        ReThrowError(exact_error);
    }
    if (cpp_error) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("%s", failure_message),
                 errdetail("%s", detail.data())));
    }
    if (unknown_cpp_error) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("%s", failure_message),
                 errdetail("unknown C++ exception")));
    }
    pg_unreachable();
}

/** Turn sequence locators into readable values before evaluating index expressions. */
static void promote_index_source_locators(Relation relation,
                                          TupleTableSlot* slot,
                                          bioseq::storage::pg_sequence_source_provider* provider,
                                          MemoryContext result_context) {
    if (provider == nullptr)
        return;
    const auto& amc = bioseq::storage::get_table_access_cache(relation);
    for (const std::int16_t attno : amc.sequence_attribute_numbers) {
        const int index = static_cast<int>(attno) - 1;
        if (index < 0 || index >= slot->tts_tupleDescriptor->natts || slot->tts_isnull[index])
            continue;
        auto* header = reinterpret_cast<const bioseq::pg_types::seq*>(
            DatumGetPointer(slot->tts_values[index]));
        if (!bioseq::storage::is_locator_stub(header))
            continue;
        slot->tts_values[index] = run_index_source_cpp_checked(
            "bioseq expression-index source promotion failed",
            [provider, attno, slot, index, result_context] {
                return bioseq::storage::promote_pg_sequence_locator(
                    provider, attno, slot->tts_values[index], result_context);
            });
    }
}

/** Open sequence readers using the index scan's snapshot. */
[[nodiscard]] static bioseq::storage::pg_sequence_source_provider*
create_index_source_provider(Relation relation, Snapshot snapshot) {
    return run_index_source_cpp_checked(
        "bioseq expression-index source provider failed", [relation, snapshot] {
            return bioseq::storage::create_standalone_pg_sequence_source_provider(relation,
                                                                                  snapshot);
        });
}

/** Build an index by scanning all visible Bioseq rows under one snapshot. */
double bioseq_index_build_range_scan(Relation bioseq_column_relation,
                                     Relation index_relation,
                                     IndexInfo* index_info,
                                     bool allow_sync,
                                     bool anyvisible,
                                     bool progress,
                                     BlockNumber start_blockno,
                                     BlockNumber numblocks,
                                     IndexBuildCallback callback,
                                     void* callback_state,
                                     TableScanDesc scan) {
    if (start_blockno != 0 || numblocks != InvalidBlockNumber) {
        /* This callback needs a full scan. BRIN-style block ranges are unsupported. */
        bioseq_log<logging_level::error, logging_target::postgres>(
            "BRIN indexes on columnar tables are not supported");
    }

    if (scan) {
        /* Parallel index builds cannot pass an existing table scan here. */
        bioseq_log<logging_level::error, logging_target::postgres>(
            "parallel scans on columnar are not supported");
    }

    /* Disable the page cache for the duration of the index build. */
    bioseq::postgres::support::page_cache_disable_scope cache_guard{
        bioseq::settings::column_cache::enabled};

    /* Use the same snapshot for table rows, source metadata, and sequences. */
    Snapshot snapshot = {0};
    bool owns_snapshot_registration = false;

    /* Keep the transaction snapshot registered until our scan ends. Bootstrap
     * processing uses SnapshotAny. */
    if (!IsBootstrapProcessingMode()) {
        snapshot = RegisterSnapshot(GetTransactionSnapshot());
        owns_snapshot_registration = true;
    } else {
        snapshot = SnapshotAny;
    }

    int nkeys = 0;
    ScanKeyData* scan_key = nullptr;
    bool allow_access_strategy = true;
    scan = table_beginscan_strat(
        bioseq_column_relation, snapshot, nkeys, scan_key, allow_access_strategy, allow_sync);
    bioseq::storage::pg_sequence_source_provider* volatile source_provider = nullptr;
    if (owns_snapshot_registration) {
        auto* bioseq_scan = reinterpret_cast<column_scan_handle>(scan);
        bioseq_scan->defers_sequence_hydration = true;
        source_provider = create_index_source_provider(bioseq_column_relation, snapshot);
    }

    if (progress) {
        report_total_virtual_blocks(bioseq_column_relation, snapshot, PROGRESS_SCAN_BLOCKS_TOTAL);
    }

    /* Prepare the predicate used by a partial index. */
    EState* estate = CreateExecutorState();
    ExprContext* econtext = GetPerTupleExprContext(estate);
    econtext->ecxt_scantuple = table_slot_create(bioseq_column_relation, nullptr);
    ExprState* predicate = ExecPrepareQual(index_info->ii_Predicate, estate);

    double volatile reltuples = 0;
    PG_TRY();
    {
        reltuples = build_index_from_column_rows(scan,
                                                 index_relation,
                                                 index_info,
                                                 progress,
                                                 callback,
                                                 callback_state,
                                                 estate,
                                                 predicate,
                                                 source_provider);
        ResetExprContext(econtext);
        if (source_provider != nullptr) {
            bioseq::storage::close_pg_sequence_source_provider(source_provider,
                                                               /*is_commit=*/true);
            source_provider = nullptr;
        }
    }
    PG_CATCH();
    {
        if (source_provider != nullptr) {
            bioseq::storage::close_pg_sequence_source_provider(source_provider,
                                                               /*is_commit=*/false);
            source_provider = nullptr;
        }
        PG_RE_THROW();
    }
    PG_END_TRY();
    table_endscan(scan);

    if (progress) {
        /* Include the final virtual block in the completed count. */
        report_total_virtual_blocks(bioseq_column_relation, snapshot, PROGRESS_SCAN_BLOCKS_DONE);
    }

    if (owns_snapshot_registration) {
        UnregisterSnapshot(snapshot);
    }

    ExecDropSingleTupleTableSlot(econtext->ecxt_scantuple);
    FreeExecutorState(estate);
    index_info->ii_ExpressionsState = NIL;
    index_info->ii_PredicateState = nullptr;

    return static_cast<double>(reltuples);
}

/** Report the virtual block count as total or completed index-build work.
 *
 * Bioseq row numbers map to virtual TIDs. Their block numbers give PostgreSQL
 * a progress measure even though column data is stored in stripes.
 */
static void
report_total_virtual_blocks(Relation relation, Snapshot snapshot, int progress_arr_index) {
    
    Assert(progress_arr_index == PROGRESS_SCAN_BLOCKS_TOTAL ||
           progress_arr_index == PROGRESS_SCAN_BLOCKS_DONE);
    BlockNumber nvirtual_blocks = column_virtual_block_count(relation, snapshot);
    pgstat_progress_update_param(progress_arr_index, nvirtual_blocks);
}

/** Return the virtual block count covered by visible stripe metadata. */
static BlockNumber column_virtual_block_count(Relation relation, Snapshot snapshot) {
    ItemPointerData highest_item_pointer = highest_column_item_pointer(relation, snapshot);
    if (!ItemPointerIsValid(&highest_item_pointer)) {
        /* table is empty according to our snapshot */
        return 0;
    }

    /* Virtual block numbers start at zero. */
    return ItemPointerGetBlockNumber(&highest_item_pointer) + 1;
}

/** Return the highest TID in visible stripes, or an invalid TID for an empty table. */
static ItemPointerData highest_column_item_pointer(Relation relation, Snapshot snapshot) {
    stripe_metadata* stripe_with_highest_row_number =
        find_highest_row_number_stripe(relation, snapshot);
    if (stripe_with_highest_row_number == nullptr ||
        stripe_highest_row_number(stripe_with_highest_row_number) == 0) {
        /* table is empty according to our snapshot */
        ItemPointerData invalid_item_ptr;
        ItemPointerSetInvalid(&invalid_item_ptr);
        return invalid_item_ptr;
    }

    uint64_t highest_row_number = stripe_highest_row_number(stripe_with_highest_row_number);
    return row_number_to_tid(highest_row_number);
}

/** Build index tuples from a table scan and return the number of indexed rows. */
static double
build_index_from_column_rows(TableScanDesc scan,
                             Relation index_relation,
                             IndexInfo* index_info,
                             bool progress,
                             IndexBuildCallback index_callback,
                             void* index_callback_state,
                             EState* estate,
                             ExprState* predicate,
                             bioseq::storage::pg_sequence_source_provider* source_provider) {
    double reltuples = 0;

    BlockNumber last_reported_block_number = InvalidBlockNumber;

    ExprContext* econtext = GetPerTupleExprContext(estate);
    TupleTableSlot* slot = econtext->ecxt_scantuple;
    while (bioseq_getnextslot(scan, ForwardScanDirection, slot)) {
        CHECK_FOR_INTERRUPTS();

        BlockNumber current_block_number = ItemPointerGetBlockNumber(&slot->tts_tid);
        if (progress && last_reported_block_number != current_block_number) {
            /* Row TIDs increase during this scan, so their block numbers can track
             * completed work. */
            Assert(last_reported_block_number == InvalidBlockNumber ||
                   current_block_number >= last_reported_block_number);
            pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE, current_block_number);
            last_reported_block_number = current_block_number;
        }

        MemoryContextReset(econtext->ecxt_per_tuple_memory);
        promote_index_source_locators(
            scan->rs_rd, slot, source_provider, econtext->ecxt_per_tuple_memory);

        if (predicate != nullptr && !ExecQual(predicate, econtext)) {
            /* for partial indexes, discard tuples that don't satisfy the predicate */
            continue;
        }

        std::array<Datum, INDEX_MAX_KEYS> index_values;
        std::array<bool, INDEX_MAX_KEYS> index_nulls;
        ItemPointerData item_pointer_data = slot->tts_tid;

        /* The scan has already excluded rows hidden by its snapshot and row mask. */
        bool tuple_is_alive = true;
        const auto previous_scope = exchange_slice_materialization_scope({
            .attachment = nullptr,
            .boundary_id = 0u,
            .producer_value_id = 0u,
            .mechanism = materialization_mechanism::packed_full,
            .reason = materialization_reason::index_tuple,
            .local_sink = nullptr,
        });
        PG_TRY();
        {
            FormIndexDatum(index_info, slot, estate, index_values.data(), index_nulls.data());
            index_callback(index_relation,
                           &item_pointer_data,
                           index_values.data(),
                           index_nulls.data(),
                           tuple_is_alive,
                           index_callback_state);
            restore_slice_materialization_scope(previous_scope);
        }
        PG_CATCH();
        {
            restore_slice_materialization_scope(previous_scope);
            PG_RE_THROW();
        }
        PG_END_TRY();

        reltuples++;
    }

    return reltuples;
}

/** Validate a concurrent index build and insert missing rows that satisfy its predicate. */
void bioseq_index_validate_scan(Relation bioseq_column_relation,
                                Relation index_relation,
                                IndexInfo* index_info,
                                Snapshot snapshot,
                                ValidateIndexState* validate_index_state) {
    report_total_virtual_blocks(bioseq_column_relation, snapshot, PROGRESS_SCAN_BLOCKS_TOTAL);

    /* Prepare the predicate used by a partial index. */
    EState* estate = CreateExecutorState();
    ExprContext* econtext = GetPerTupleExprContext(estate);
    econtext->ecxt_scantuple = table_slot_create(bioseq_column_relation, nullptr);
    ExprState* predicate = ExecPrepareQual(index_info->ii_Predicate, estate);

    int nkeys = 0;
    ScanKeyData* scan_key = nullptr;
    bool allow_access_strategy = true;
    bool allow_sync_scan = false;
    TableScanDesc scan = table_beginscan_strat(
        bioseq_column_relation, snapshot, nkeys, scan_key, allow_access_strategy, allow_sync_scan);
    bioseq::storage::pg_sequence_source_provider* volatile source_provider = nullptr;
    if (snapshot != nullptr && IsMVCCSnapshot(snapshot) && snapshot->regd_count > 0) {
        auto* bioseq_scan = reinterpret_cast<column_scan_handle>(scan);
        bioseq_scan->defers_sequence_hydration = true;
        source_provider = create_index_source_provider(bioseq_column_relation, snapshot);
    }

    PG_TRY();
    {
        validate_missing_index_rows(scan,
                                    index_relation,
                                    index_info,
                                    estate,
                                    predicate,
                                    validate_index_state,
                                    source_provider);
        ResetExprContext(econtext);
        if (source_provider != nullptr) {
            bioseq::storage::close_pg_sequence_source_provider(source_provider, /*is_commit=*/true);
            source_provider = nullptr;
        }
    }
    PG_CATCH();
    {
        if (source_provider != nullptr) {
            bioseq::storage::close_pg_sequence_source_provider(source_provider,
                                                               /*is_commit=*/false);
            source_provider = nullptr;
        }
        PG_RE_THROW();
    }
    PG_END_TRY();

    table_endscan(scan);

    /* Include the final virtual block in the completed count. */
    report_total_virtual_blocks(bioseq_column_relation, snapshot, PROGRESS_SCAN_BLOCKS_DONE);

    ExecDropSingleTupleTableSlot(econtext->ecxt_scantuple);
    FreeExecutorState(estate);
    index_info->ii_ExpressionsState = NIL;
    index_info->ii_PredicateState = nullptr;
}

/** Insert rows missing from the sorted index-TID list, subject to the index predicate. */
static void
validate_missing_index_rows(TableScanDesc scan,
                            Relation index_relation,
                            IndexInfo* index_info,
                            EState* estate,
                            ExprState* predicate,
                            ValidateIndexState* validate_index_state,
                            bioseq::storage::pg_sequence_source_provider* source_provider) {
    BlockNumber last_reported_block_number = InvalidBlockNumber;

    bool index_tuple_sort_empty = false;
    ItemPointerData indexed_item_pointer_data;
    ItemPointerSetInvalid(&indexed_item_pointer_data);

    ExprContext* econtext = GetPerTupleExprContext(estate);
    TupleTableSlot* slot = econtext->ecxt_scantuple;
    while (bioseq_getnextslot(scan, ForwardScanDirection, slot)) {
        CHECK_FOR_INTERRUPTS();

        ItemPointer bioseq_column_item_pointer = &slot->tts_tid;
        BlockNumber current_block_number = ItemPointerGetBlockNumber(bioseq_column_item_pointer);
        if (last_reported_block_number != current_block_number) {
            /* Row TIDs increase during this scan, so their block numbers can track
             * completed work. */
            Assert(last_reported_block_number == InvalidBlockNumber ||
                   current_block_number >= last_reported_block_number);
            pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE, current_block_number);
            last_reported_block_number = current_block_number;
        }

        validate_index_state->htups += 1;

        if (!index_tuple_sort_empty &&
            (!ItemPointerIsValid(&indexed_item_pointer_data) ||
             ItemPointerCompare(&indexed_item_pointer_data, bioseq_column_item_pointer) < 0)) {
            /* Advance the index TIDs until they reach or pass this table row. */
            indexed_item_pointer_data = skip_item_pointers_before(validate_index_state->tuplesort,
                                                                  bioseq_column_item_pointer);
            index_tuple_sort_empty = !ItemPointerIsValid(&indexed_item_pointer_data);
        }

        if (!index_tuple_sort_empty &&
            ItemPointerCompare(&indexed_item_pointer_data, bioseq_column_item_pointer) == 0) {
            /* tuple is already covered by the index, skip */
            continue;
        }
        Assert(index_tuple_sort_empty ||
               ItemPointerCompare(&indexed_item_pointer_data, bioseq_column_item_pointer) > 0);

        MemoryContextReset(econtext->ecxt_per_tuple_memory);
        promote_index_source_locators(
            scan->rs_rd, slot, source_provider, econtext->ecxt_per_tuple_memory);

        if (predicate != nullptr && !ExecQual(predicate, econtext)) {
            /* for partial indexes, discard tuples that don't satisfy the predicate */
            continue;
        }

        std::array<Datum, INDEX_MAX_KEYS> index_values;
        std::array<bool, INDEX_MAX_KEYS> index_nulls;

        Relation bioseq_column_relation = scan->rs_rd;
        IndexUniqueCheck index_unique_check =
            index_info->ii_Unique ? UNIQUE_CHECK_YES : UNIQUE_CHECK_NO;
        const auto previous_scope = exchange_slice_materialization_scope({
            .attachment = nullptr,
            .boundary_id = 0u,
            .producer_value_id = 0u,
            .mechanism = materialization_mechanism::packed_full,
            .reason = materialization_reason::index_tuple,
            .local_sink = nullptr,
        });
        PG_TRY();
        {
            FormIndexDatum(index_info, slot, estate, index_values.data(), index_nulls.data());
            index_insert(index_relation,
                         index_values.data(),
                         index_nulls.data(),
                         bioseq_column_item_pointer,
                         bioseq_column_relation,
                         index_unique_check,
                         false,
                         index_info);
            restore_slice_materialization_scope(previous_scope);
        }
        PG_CATCH();
        {
            restore_slice_materialization_scope(previous_scope);
            PG_RE_THROW();
        }
        PG_END_TRY();

        validate_index_state->tups_inserted += 1;
    }
}

/** Consume sorted TIDs until reaching or passing the target.
 *
 * Return an invalid TID when the stream ends. The stream must contain no
 * null values.
 */
static ItemPointerData skip_item_pointers_before(Tuplesortstate* tuple_sort,
                                                 ItemPointer target_item_pointer) {
    ItemPointerData ts_item_pointer_data;
    ItemPointerSetInvalid(&ts_item_pointer_data);

    while (!ItemPointerIsValid(&ts_item_pointer_data) ||
           ItemPointerCompare(&ts_item_pointer_data, target_item_pointer) < 0) {
        bool forward_direction = true;
        Datum* abbrev = nullptr;
        Datum ts_datum;
        bool ts_datum_is_null;
        if (!tuplesort_getdatum(
                tuple_sort, forward_direction, false, &ts_datum, &ts_datum_is_null, abbrev)) {
            ItemPointerSetInvalid(&ts_item_pointer_data);
            break;
        }

        Assert(!ts_datum_is_null);
        itemptr_decode(&ts_item_pointer_data, DatumGetInt64(ts_datum));

#ifndef USE_FLOAT8_BYVAL

        /* tuplesort_getdatum returns allocated storage when int8 is passed
         * by reference. Free it after decoding the TID. */
        pfree(DatumGetPointer(ts_datum));
#endif
    }

    return ts_item_pointer_data;
}

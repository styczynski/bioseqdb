// Educational PostgreSQL 17 table AM. Storage and visibility remain heap's.
// SPDX-License-Identifier: Apache-2.0
extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "access/tableam.h"
#include "catalog/pg_am_d.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
}

#if PG_VERSION_NUM < 170000 || PG_VERSION_NUM >= 180000
#error "This example targets PostgreSQL 17 server headers."
#endif

extern "C" {
PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(traceheap_handler);
PG_FUNCTION_INFO_V1(traceheap_stats);
PG_FUNCTION_INFO_V1(traceheap_reset);
}

namespace {
struct Counters {
    uint64 scans_started = 0;
    uint64 scans_ended = 0;
    uint64 rescans = 0;
    uint64 scan_rows = 0;
    uint64 inserts = 0;
    uint64 bulk_rows = 0;
    uint64 updates = 0;
    uint64 deletes = 0;
    uint64 vacuums = 0;
};

// Each backend process has its own counters. They are not transactional.
Counters counters;
TableAmRoutine methods;
const TableAmRoutine* heap_methods = nullptr;

Oid toast_method(Relation) {
    // The auxiliary TOAST table needs its ordinary heap index. Inheriting
    // heap's callback would return our parent relation's traceheap AM OID.
    return HEAP_TABLE_AM_OID;
}

TableScanDesc traced_begin(Relation rel, Snapshot snapshot, int nkeys,
                           ScanKeyData* key, ParallelTableScanDesc parallel,
                           uint32 flags) {
    auto scan = heap_methods->scan_begin(rel, snapshot, nkeys, key, parallel, flags);
    ++counters.scans_started;
    return scan;
}

void traced_end(TableScanDesc scan) {
    heap_methods->scan_end(scan);
    ++counters.scans_ended;
}

void traced_rescan(TableScanDesc scan, ScanKeyData* key, bool set_params,
                    bool allow_strat, bool allow_sync, bool allow_pagemode) {
    heap_methods->scan_rescan(scan, key, set_params, allow_strat, allow_sync, allow_pagemode);
    ++counters.rescans;
}

bool traced_next(TableScanDesc scan, ScanDirection direction, TupleTableSlot* slot) {
    const bool found = heap_methods->scan_getnextslot(scan, direction, slot);
    if (found) ++counters.scan_rows;
    return found;
}

void traced_insert(Relation rel, TupleTableSlot* slot, CommandId cid,
                   int options, BulkInsertStateData* state) {
    heap_methods->tuple_insert(rel, slot, cid, options, state);
    ++counters.inserts;
}

void traced_multi_insert(Relation rel, TupleTableSlot** slots, int nslots,
                         CommandId cid, int options, BulkInsertStateData* state) {
    heap_methods->multi_insert(rel, slots, nslots, cid, options, state);
    counters.bulk_rows += nslots;
}

TM_Result traced_update(Relation rel, ItemPointer old_tid, TupleTableSlot* slot,
                        CommandId cid, Snapshot snapshot, Snapshot crosscheck,
                        bool wait, TM_FailureData* failure, LockTupleMode* lockmode,
                        TU_UpdateIndexes* update_indexes) {
    const auto result = heap_methods->tuple_update(
        rel, old_tid, slot, cid, snapshot, crosscheck, wait, failure,
        lockmode, update_indexes);
    if (result == TM_Ok) ++counters.updates;
    return result;
}

TM_Result traced_delete(Relation rel, ItemPointer tid, CommandId cid,
                        Snapshot snapshot, Snapshot crosscheck, bool wait,
                        TM_FailureData* failure, bool changing_partition) {
    const auto result = heap_methods->tuple_delete(
        rel, tid, cid, snapshot, crosscheck, wait, failure, changing_partition);
    if (result == TM_Ok) ++counters.deletes;
    return result;
}

[[noreturn]] void unsupported(const char* operation) {
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("traceheap does not implement %s", operation),
             errhint("Use an ordinary heap table when this operation is required.")));
    pg_unreachable();
}

double reject_index_build(Relation, Relation, IndexInfo*, bool, bool, bool,
                          BlockNumber, BlockNumber, IndexBuildCallback,
                          void*, TableScanDesc) {
    unsupported("index creation");
}

void reject_index_validation(Relation, Relation, IndexInfo*, Snapshot,
                             ValidateIndexState*) {
    unsupported("index validation");
}

void reject_rewrite(Relation, Relation, Relation, bool, TransactionId,
                    TransactionId*, MultiXactId*, double*, double*, double*) {
    unsupported("VACUUM FULL or a table rewrite through the clustering callback");
}

void traced_vacuum(Relation rel, VacuumParams* params, BufferAccessStrategy strategy) {
    heap_methods->relation_vacuum(rel, params, strategy);
    ++counters.vacuums;
}
} // namespace

extern "C" Datum traceheap_handler(PG_FUNCTION_ARGS) {
    if (heap_methods == nullptr) {
        heap_methods = GetHeapamTableAmRoutine();
        methods = *heap_methods;
        methods.scan_begin = traced_begin;
        methods.scan_end = traced_end;
        methods.scan_rescan = traced_rescan;
        methods.scan_getnextslot = traced_next;
        methods.tuple_insert = traced_insert;
        methods.multi_insert = traced_multi_insert;
        methods.tuple_update = traced_update;
        methods.tuple_delete = traced_delete;
        methods.relation_vacuum = traced_vacuum;
        methods.relation_toast_am = toast_method;
        // Heap's index builder calls heap_getnext(), which checks that the
        // relation has the native heap routine identity. Copying that callback
        // does not make it usable by another AM. Reject these paths explicitly.
        methods.index_build_range_scan = reject_index_build;
        methods.index_validate_scan = reject_index_validation;
        methods.relation_copy_for_cluster = reject_rewrite;
    }
    PG_RETURN_POINTER(&methods);
}

extern "C" Datum traceheap_stats(PG_FUNCTION_ARGS) {
    char* json = psprintf(
        "{\"scans_started\":" UINT64_FORMAT ",\"scans_ended\":" UINT64_FORMAT
        ",\"rescans\":" UINT64_FORMAT ",\"scan_rows\":" UINT64_FORMAT
        ",\"inserts\":" UINT64_FORMAT ",\"bulk_rows\":" UINT64_FORMAT
        ",\"updates\":" UINT64_FORMAT ",\"deletes\":" UINT64_FORMAT
        ",\"vacuums\":" UINT64_FORMAT "}",
        counters.scans_started, counters.scans_ended, counters.rescans, counters.scan_rows,
        counters.inserts, counters.bulk_rows, counters.updates, counters.deletes,
        counters.vacuums);
    PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in, CStringGetDatum(json)));
}

extern "C" Datum traceheap_reset(PG_FUNCTION_ARGS) {
    counters = Counters{};
    PG_RETURN_VOID();
}

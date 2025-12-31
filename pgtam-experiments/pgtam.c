#include "postgres.h"
#include "fmgr.h"
#include "access/tableam.h"
#include "access/heapam.h"
#include "nodes/execnodes.h"
#include "catalog/index.h"
#include "commands/vacuum.h"
#include "utils/builtins.h"
#include "executor/tuptable.h"

FILE* fd;
#define DEBUG_FUNC() fprintf(fd, "in %s\n", __func__)

struct Column {
  int value;
};

struct Row {
  struct Column* columns;
  size_t ncolumns;
};

#define MAX_ROWS 100
struct Table {
  char* name;
  struct Row* rows;
  size_t nrows;
};

#define MAX_TABLES 100
struct Database {
  struct Table* tables;
  size_t ntables;
};

struct Database* database;

static void get_table(struct Table** table, Relation relation) {
  char* this_name = NameStr(relation->rd_rel->relname);
  for (size_t i = 0; i < database->ntables; i++) {
    if (strcmp(database->tables[i].name, this_name) == 0) {
      *table = &database->tables[i];
      return;
    }
  }
}

PG_MODULE_MAGIC;

const TableAmRoutine memam_methods;

static const TupleTableSlotOps* memam_slot_callbacks(
  Relation relation
) {
  DEBUG_FUNC();
  return &TTSOpsVirtual;
}

struct MemScanDesc {
  TableScanDescData rs_base; // Base class from access/relscan.h.

  // Custom data.
  uint32 cursor;
};

static TableScanDesc memam_beginscan(
  Relation relation,
  Snapshot snapshot,
  int nkeys,
  struct ScanKeyData *key,
  ParallelTableScanDesc parallel_scan,
  uint32 flags
) {
  struct MemScanDesc* scan;
  DEBUG_FUNC();
  scan = (struct MemScanDesc*)malloc(sizeof(struct MemScanDesc));
  scan->rs_base.rs_rd = relation;
  scan->rs_base.rs_snapshot = snapshot;
  scan->rs_base.rs_nkeys = nkeys;
  scan->rs_base.rs_flags = flags;
  scan->rs_base.rs_parallel = parallel_scan;
  scan->cursor = 0;
  return (TableScanDesc)scan;
}

static void memam_rescan(
  TableScanDesc sscan,
  struct ScanKeyData *key,
  bool set_params,
  bool allow_strat,
  bool allow_sync,
  bool allow_pagemode
) {
  DEBUG_FUNC();
}

static void memam_endscan(TableScanDesc sscan) {
  DEBUG_FUNC();
  free(sscan);
}

static bool memam_getnextslot(
  TableScanDesc sscan,
  ScanDirection direction,
  TupleTableSlot *slot
) {
  struct MemScanDesc* mscan = NULL;
  struct Table* table = NULL;
  DEBUG_FUNC();
  
  mscan = (struct MemScanDesc*)sscan;
  ExecClearTuple(slot);

  get_table(&table, mscan->rs_base.rs_rd);
  if (table == NULL || mscan->cursor == table->nrows) {
    return false;
  }

  slot->tts_values[0] = Int32GetDatum(table->rows[mscan->cursor].columns[0].value);
  slot->tts_isnull[0] = false;
  ExecStoreVirtualTuple(slot);
  mscan->cursor++;
  return true;
}

static IndexFetchTableData* memam_index_fetch_begin(Relation rel) {
  DEBUG_FUNC();
  return NULL;
}

static void memam_index_fetch_reset(IndexFetchTableData *scan) {}

static void memam_index_fetch_end(IndexFetchTableData *scan) {}

static bool memam_index_fetch_tuple(
  struct IndexFetchTableData *scan,
  ItemPointer tid,
  Snapshot snapshot,
  TupleTableSlot *slot,
  bool *call_again,
  bool *all_dead
) {
  DEBUG_FUNC();
  return false;
}

static void memam_tuple_insert(
  Relation relation,
  TupleTableSlot *slot,
  CommandId cid,
  int options,
  BulkInsertState bistate
) {
  TupleDesc desc = RelationGetDescr(relation);
  struct Table* table = NULL;
  struct Column column = {};
  struct Row row = {};

  DEBUG_FUNC();

  get_table(&table, relation);
  if (table == NULL) {
    elog(ERROR, "table not found");
    return;
  }

  if (table->nrows == MAX_ROWS) {
    elog(ERROR, "cannot insert more rows");
    return;
  }

  row.ncolumns = desc->natts;
  Assert(slot->tts_nvalid == row.ncolumns);
  Assert(row.ncolumns > 0);

  row.columns = (struct Column*)malloc(sizeof(struct Column) * row.ncolumns);
  for (size_t i = 0; i < row.ncolumns; i++) {
    Assert(desc->attrs[i].atttypid == INT4OID);
    column.value = DatumGetInt32(slot->tts_values[i]);
    row.columns[i] = column;
  }

  table->rows[table->nrows] = row;
  table->nrows++;
}

static void memam_tuple_insert_speculative(
  Relation relation,
  TupleTableSlot *slot,
  CommandId cid,
  int options,
  BulkInsertState bistate,
  uint32 specToken) {
  DEBUG_FUNC();
}

static void memam_tuple_complete_speculative(
  Relation relation,
  TupleTableSlot *slot,
  uint32 specToken,
  bool succeeded) {
  DEBUG_FUNC();
}

static void memam_multi_insert(
  Relation relation,
  TupleTableSlot **slots,
  int ntuples,
  CommandId cid,
  int options,
  BulkInsertState bistate
) {
  DEBUG_FUNC();
}

static TM_Result memam_tuple_delete(
  Relation relation,
  ItemPointer tid,
  CommandId cid,
  Snapshot snapshot,
  Snapshot crosscheck,
  bool wait,
  TM_FailureData *tmfd,
  bool changingPart
) {
  TM_Result result = 0;
  DEBUG_FUNC();
  return result;
}

static TM_Result memam_tuple_update(
  Relation relation,
  ItemPointer otid,
  TupleTableSlot *slot,
  CommandId cid,
  Snapshot snapshot,
  Snapshot crosscheck,
  bool wait,
  TM_FailureData *tmfd,
  LockTupleMode *lockmode,
  TU_UpdateIndexes *update_indexes
) {
  TM_Result result = 0;
  DEBUG_FUNC();
  return result;
}

static TM_Result memam_tuple_lock(
  Relation relation,
  ItemPointer tid,
  Snapshot snapshot,
  TupleTableSlot *slot,
  CommandId cid,
  LockTupleMode mode,
  LockWaitPolicy wait_policy,
  uint8 flags,
  TM_FailureData *tmfd)
{
  TM_Result result = 0;
  DEBUG_FUNC();
  return result;
}

static bool memam_fetch_row_version(
  Relation relation,
  ItemPointer tid,
  Snapshot snapshot,
  TupleTableSlot *slot
) {
  DEBUG_FUNC();
  return false;
}

static void memam_get_latest_tid(
  TableScanDesc sscan,
  ItemPointer tid
) {
  DEBUG_FUNC();
}

static bool memam_tuple_tid_valid(TableScanDesc scan, ItemPointer tid) {
  DEBUG_FUNC();
  return false;
}

static bool memam_tuple_satisfies_snapshot(
  Relation rel,
  TupleTableSlot *slot,
  Snapshot snapshot
) {
  DEBUG_FUNC();
  return false;
}

static TransactionId memam_index_delete_tuples(
  Relation rel,
  TM_IndexDeleteOp *delstate
) {
  TransactionId id = 0;
  DEBUG_FUNC();
  return id;
}

static void memam_relation_set_new_filelocator(
  Relation rel,
  const RelFileLocator *newrlocator,
  char persistence,
  TransactionId *freezeXid,
  MultiXactId *minmulti
) {
  struct Table table;
  table.name = strdup(NameStr(rel->rd_rel->relname));
  fprintf(fd, "Created table: [%s]\n", table.name);
  table.rows = (struct Row*)malloc(sizeof(struct Row) * MAX_ROWS);
  table.nrows = 0;

  database->tables[database->ntables] = table;
  database->ntables++;
  DEBUG_FUNC();
}

static void memam_relation_nontransactional_truncate(
  Relation rel
) {
  DEBUG_FUNC();
}

static void memam_relation_copy_data(
  Relation rel,
  const RelFileLocator *newrlocator
) {
  DEBUG_FUNC();
}

static void memam_relation_copy_for_cluster(
  Relation OldHeap,
  Relation NewHeap,
  Relation OldIndex,
  bool use_sort,
  TransactionId OldestXmin,
  TransactionId *xid_cutoff,
  MultiXactId *multi_cutoff,
  double *num_tuples,
  double *tups_vacuumed,
  double *tups_recently_dead
) {
  DEBUG_FUNC();
}

static void memam_vacuum_rel(
  Relation rel,
  VacuumParams *params,
  BufferAccessStrategy bstrategy
) {
  DEBUG_FUNC();
}

static bool memam_scan_analyze_next_block(
  TableScanDesc scan,
  BlockNumber blockno,
  BufferAccessStrategy bstrategy
) {
  DEBUG_FUNC();
  return false;
}

static bool memam_scan_analyze_next_tuple(
  TableScanDesc scan,
  TransactionId OldestXmin,
  double *liverows,
  double *deadrows,
  TupleTableSlot *slot
) {
  DEBUG_FUNC();
  return false;
}

static double memam_index_build_range_scan(
  Relation heapRelation,
  Relation indexRelation,
  IndexInfo *indexInfo,
  bool allow_sync,
  bool anyvisible,
  bool progress,
  BlockNumber start_blockno,
  BlockNumber numblocks,
  IndexBuildCallback callback,
  void *callback_state,
  TableScanDesc scan
) {
  DEBUG_FUNC();
  return 0;
}

static void memam_index_validate_scan(
  Relation heapRelation,
  Relation indexRelation,
  IndexInfo *indexInfo,
  Snapshot snapshot,
  ValidateIndexState *state
) {
  DEBUG_FUNC();
}

static bool memam_relation_needs_toast_table(Relation rel) {
  DEBUG_FUNC();
  return false;
}

static Oid memam_relation_toast_am(Relation rel) {
  Oid oid = 0;
  DEBUG_FUNC();
  return oid;
}

static void memam_fetch_toast_slice(
  Relation toastrel,
  Oid valueid,
  int32 attrsize,
  int32 sliceoffset,
  int32 slicelength,
  struct varlena *result
) {
  DEBUG_FUNC();
}

static void memam_estimate_rel_size(
  Relation rel,
  int32 *attr_widths,
  BlockNumber *pages,
  double *tuples,
  double *allvisfrac
) {
  DEBUG_FUNC();
}

static bool memam_scan_sample_next_block(
  TableScanDesc scan, SampleScanState *scanstate
) {
  DEBUG_FUNC();
  return false;
}

static bool memam_scan_sample_next_tuple(
  TableScanDesc scan,
  SampleScanState *scanstate,
  TupleTableSlot *slot
) {
  DEBUG_FUNC();
  return false;
}

const TableAmRoutine memam_methods = {
  .type = T_TableAmRoutine,

  .slot_callbacks = memam_slot_callbacks,

  .scan_begin = memam_beginscan,
  .scan_end = memam_endscan,
  .scan_rescan = memam_rescan,
  .scan_getnextslot = memam_getnextslot,

  .parallelscan_estimate = table_block_parallelscan_estimate,
  .parallelscan_initialize = table_block_parallelscan_initialize,
  .parallelscan_reinitialize = table_block_parallelscan_reinitialize,

  .index_fetch_begin = memam_index_fetch_begin,
  .index_fetch_reset = memam_index_fetch_reset,
  .index_fetch_end = memam_index_fetch_end,
  .index_fetch_tuple = memam_index_fetch_tuple,

  .tuple_insert = memam_tuple_insert,
  .tuple_insert_speculative = memam_tuple_insert_speculative,
  .tuple_complete_speculative = memam_tuple_complete_speculative,
  .multi_insert = memam_multi_insert,
  .tuple_delete = memam_tuple_delete,
  .tuple_update = memam_tuple_update,
  .tuple_lock = memam_tuple_lock,

  .tuple_fetch_row_version = memam_fetch_row_version,
  .tuple_get_latest_tid = memam_get_latest_tid,
  .tuple_tid_valid = memam_tuple_tid_valid,
  .tuple_satisfies_snapshot = memam_tuple_satisfies_snapshot,
  .index_delete_tuples = memam_index_delete_tuples,

  .relation_set_new_filelocator = memam_relation_set_new_filelocator,
  .relation_nontransactional_truncate = memam_relation_nontransactional_truncate,
  .relation_copy_data = memam_relation_copy_data,
  .relation_copy_for_cluster = memam_relation_copy_for_cluster,
  .relation_vacuum = memam_vacuum_rel,
  .scan_analyze_next_block = memam_scan_analyze_next_block,
  .scan_analyze_next_tuple = memam_scan_analyze_next_tuple,
  .index_build_range_scan = memam_index_build_range_scan,
  .index_validate_scan = memam_index_validate_scan,

  .relation_size = table_block_relation_size,
  .relation_needs_toast_table = memam_relation_needs_toast_table,
  .relation_toast_am = memam_relation_toast_am,
  .relation_fetch_toast_slice = memam_fetch_toast_slice,

  .relation_estimate_size = memam_estimate_rel_size,

  .scan_sample_next_block = memam_scan_sample_next_block,
  .scan_sample_next_tuple = memam_scan_sample_next_tuple
};

PG_FUNCTION_INFO_V1(mem_tableam_handler);

Datum mem_tableam_handler(PG_FUNCTION_ARGS) {
  fd = fopen("/tmp/pgtam.log", "a");
  setvbuf(fd, NULL, _IONBF, 0);
  fprintf(fd, "\n\nmem_tableam handler loaded\n");

  if (database == NULL) {
    database = (struct Database*)malloc(sizeof(struct Database));
    database->ntables = 0;
    database->tables = (struct Table*)malloc(sizeof(struct Table) * MAX_TABLES);
  }

  PG_RETURN_POINTER(&memam_methods);
}

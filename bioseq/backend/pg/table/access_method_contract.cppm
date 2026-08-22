/**
 * @file
 * @brief Declares TableAM callbacks without importing their implementation partitions.
 */
module;

#include <bioseq/pg_compat.h>
#include <cstdint>

export module bioseq.postgres.table:access_method_contract;

import bioseq.postgres.storage.metadata;
export import bioseq.postgres.table.types;

/** Begin a table scan using PostgreSQL's standard projection policy. */
TableScanDesc bioseq_beginscan(Relation relation,
                               Snapshot snapshot,
                               int key_count,
                               ScanKey key,
                               ParallelTableScanDesc parallel_table_scan,
                               std::uint32_t flags);

/** Begin a projected table scan with eager sequence hydration. */
export TableScanDesc bioseq_beginscan_extended(
    Relation relation,
    Snapshot snapshot,
    int key_count,
    ScanKey key,
    ParallelTableScanDesc parallel_table_scan,
    std::uint32_t flags,
    Bitmapset* attributes_needed,
    List* scan_qualifiers,
    parallel_column_scan_handle parallel_column_scan,
    bool returns_vectorized_tuples);

/** Begin a projected table scan that defers sequence hydration. */
export TableScanDesc bioseq_beginscan_deferred(
    Relation relation,
    Snapshot snapshot,
    int key_count,
    ScanKey key,
    ParallelTableScanDesc parallel_table_scan,
    std::uint32_t flags,
    Bitmapset* attributes_needed,
    List* scan_qualifiers,
    parallel_column_scan_handle parallel_column_scan,
    bool returns_vectorized_tuples);

/** C ABI entry point for a projected scan with eager sequence hydration. */
export extern "C" TableScanDesc bioseq_beginscan_extended_c(
    Relation relation,
    Snapshot snapshot,
    int key_count,
    ScanKey key,
    ParallelTableScanDesc parallel_table_scan,
    std::uint32_t flags,
    Bitmapset* attributes_needed,
    List* scan_qualifiers,
    parallel_column_scan_handle parallel_column_scan,
    bool returns_vectorized_tuples);

/** C ABI entry point for a projected scan with deferred sequence hydration. */
export extern "C" TableScanDesc bioseq_beginscan_deferred_c(
    Relation relation,
    Snapshot snapshot,
    int key_count,
    ScanKey key,
    ParallelTableScanDesc parallel_table_scan,
    std::uint32_t flags,
    Bitmapset* attributes_needed,
    List* scan_qualifiers,
    parallel_column_scan_handle parallel_column_scan,
    bool returns_vectorized_tuples);

/** End a table scan and release its owned reader state. */
void bioseq_endscan(TableScanDesc scan);

/** Reinitialize a table scan with new scan keys. */
void bioseq_rescan(TableScanDesc scan,
                   ScanKey key,
                   bool set_params,
                   bool allow_strat,
                   bool allow_sync,
                   bool allow_pagemode);

/** Report the shared-memory size required by a parallel table scan. */
Size bioseq_parallelscan_estimate(Relation relation);

/** Initialize shared state for a parallel table scan. */
Size bioseq_parallelscan_initialize(Relation relation, ParallelTableScanDesc scan);

/** Reset shared state for a parallel table scan. */
void bioseq_parallelscan_reinitialize(Relation relation, ParallelTableScanDesc scan);

/** Begin an exact-row fetch for an index scan. */
IndexFetchTableData* bioseq_index_fetch_begin(Relation relation);

/** Begin an exact-row fetch with an explicit projected-attribute bitmap. */
export IndexFetchTableData* bioseq_index_fetch_begin_extended(
    Relation relation,
    Bitmapset* attributes_needed);

/** Reset an exact-row fetch before it is reused. */
void bioseq_index_fetch_reset(IndexFetchTableData* scan);

/** End an exact-row fetch and release its owned state. */
void bioseq_index_fetch_end(IndexFetchTableData* scan);

/** Fetch one index-selected row under the supplied snapshot. */
bool bioseq_index_fetch_tuple(IndexFetchTableData* scan,
                              ItemPointer tuple_id,
                              Snapshot snapshot,
                              TupleTableSlot* slot,
                              bool* call_again,
                              bool* all_dead);

/** Fetch one visible row version into a tuple slot. */
bool bioseq_fetch_row_version(Relation relation,
                              ItemPointer tuple_id,
                              Snapshot snapshot,
                              TupleTableSlot* slot);

/** Resolve the latest virtual tuple identifier for a scan. */
void bioseq_get_latest_tid(TableScanDesc scan, ItemPointer tuple_id);

/** Check whether a virtual tuple identifier is valid for a scan. */
bool bioseq_tuple_tid_valid(TableScanDesc scan, ItemPointer tuple_id);

/** Check whether a tuple slot is visible under a snapshot. */
bool bioseq_tuple_satisfies_snapshot(Relation relation,
                                     TupleTableSlot* slot,
                                     Snapshot snapshot);

/** Apply PostgreSQL's index-deletion request to virtual row identities. */
TransactionId bioseq_index_delete_tuples(Relation relation,
                                         TM_IndexDeleteOp* delete_state);

/** Insert one row into column storage. */
void bioseq_tuple_insert(Relation relation,
                         TupleTableSlot* slot,
                         CommandId command_id,
                         int options,
                         BulkInsertState bulk_state);

/** Insert one row while retaining speculative source authorities. */
void bioseq_tuple_insert_speculative(Relation relation,
                                     TupleTableSlot* slot,
                                     CommandId command_id,
                                     int options,
                                     BulkInsertState bulk_state,
                                     std::uint32_t speculative_token);

/** Complete or abort one speculative row insertion. */
void bioseq_tuple_complete_speculative(Relation relation,
                                       TupleTableSlot* slot,
                                       std::uint32_t speculative_token,
                                       bool succeeded);

/** Insert a batch of rows into column storage. */
void bioseq_multi_insert(Relation relation,
                         TupleTableSlot** slots,
                         int tuple_count,
                         CommandId command_id,
                         int options,
                         BulkInsertState bulk_state);

/** Delete one row by masking its virtual identity. */
TM_Result bioseq_tuple_delete(Relation relation,
                              ItemPointer tuple_id,
                              CommandId command_id,
                              Snapshot snapshot,
                              Snapshot crosscheck,
                              bool wait,
                              TM_FailureData* failure_data,
                              bool changing_partition);

/** Replace one row and report whether indexes require updates. */
TM_Result bioseq_tuple_update(Relation relation,
                              ItemPointer old_tuple_id,
                              TupleTableSlot* slot,
                              CommandId command_id,
                              Snapshot snapshot,
                              Snapshot crosscheck,
                              bool wait,
                              TM_FailureData* failure_data,
                              LockTupleMode* lock_mode,
                              TU_UpdateIndexes* update_indexes);

/** Fetch one row for PostgreSQL's tuple-lock callback. */
TM_Result bioseq_tuple_lock(Relation relation,
                            ItemPointer tuple_id,
                            Snapshot snapshot,
                            TupleTableSlot* slot,
                            CommandId command_id,
                            LockTupleMode mode,
                            LockWaitPolicy wait_policy,
                            std::uint8_t flags,
                            TM_FailureData* failure_data);

/** Finish a bulk insert while retaining transaction-owned write state. */
void bioseq_finish_bulk_insert(Relation relation, int options);

/** Build an index from all visible rows in a table scan. */
double bioseq_index_build_range_scan(Relation relation,
                                     Relation index_relation,
                                     IndexInfo* index_info,
                                     bool allow_sync,
                                     bool any_visible,
                                     bool progress,
                                     BlockNumber start_block,
                                     BlockNumber block_count,
                                     IndexBuildCallback callback,
                                     void* callback_state,
                                     TableScanDesc scan);

/** Validate an index against every visible row. */
void bioseq_index_validate_scan(Relation relation,
                                Relation index_relation,
                                IndexInfo* index_info,
                                Snapshot snapshot,
                                ValidateIndexState* validation_state);

/** Allocate storage for a newly assigned relation locator. */
void bioseq_relation_set_new_filenode(Relation relation,
                                      const RelFileLocator* new_locator,
                                      char persistence,
                                      TransactionId* freeze_id,
                                      MultiXactId* minimum_multi_id);

/** Reinitialize a relation after nontransactional truncation. */
void bioseq_relation_nontransactional_truncate(Relation relation);

/** Copy a relation's physical forks to a new locator. */
void bioseq_relation_copy_data(Relation relation,
                               const RelFileLocator* new_locator);

/** Copy visible rows while rewriting a relation. */
void bioseq_relation_copy_for_cluster(Relation old_relation,
                                      Relation new_relation,
                                      Relation old_index,
                                      bool use_sort,
                                      TransactionId oldest_xmin,
                                      TransactionId* xid_cutoff,
                                      MultiXactId* multi_cutoff,
                                      double* tuple_count,
                                      double* vacuumed_count,
                                      double* recently_dead_count);

/** Run Bioseq relation vacuum maintenance. */
void bioseq_vacuum_rel(Relation relation,
                       VacuumParams* parameters,
                       BufferAccessStrategy buffer_strategy);

/** Select the next logical block for ANALYZE. */
bool bioseq_scan_analyze_next_block(TableScanDesc scan, ReadStream* stream);

/** Feed the next visible row to ANALYZE. */
bool bioseq_scan_analyze_next_tuple(TableScanDesc scan,
                                    TransactionId oldest_xmin,
                                    double* live_rows,
                                    double* dead_rows,
                                    TupleTableSlot* slot);

/** Return bytes used by one relation fork or all forks. */
uint64 bioseq_relation_size(Relation relation, ForkNumber fork_number);

/** Report whether the relation requires a TOAST table. */
bool bioseq_relation_needs_toast_table(Relation relation);

/** Populate planner size and visibility estimates. */
void bioseq_estimate_rel_size(Relation relation,
                              std::int32_t* attribute_widths,
                              BlockNumber* pages,
                              double* tuples,
                              double* all_visible_fraction);

/** Select the next TABLESAMPLE block. */
bool bioseq_scan_sample_next_block(TableScanDesc scan,
                                   SampleScanState* sample_state);

/** Select the next TABLESAMPLE tuple. */
bool bioseq_scan_sample_next_tuple(TableScanDesc scan,
                                   SampleScanState* sample_state,
                                   TupleTableSlot* slot);

/** Apply table state transitions at a transaction boundary. */
void column_transaction_callback(XactEvent event, void* argument);

/** Apply table state transitions at a subtransaction boundary. */
void column_subtransaction_callback(SubXactEvent event,
                                    SubTransactionId subtransaction_id,
                                    SubTransactionId parent_id,
                                    void* argument);

/** Flush pending writes through the storage hook boundary. */
void bioseq_flush_pending_writes_adapter(column_write_state* write_state);

/** Close an opaque sequence-cache owner through the storage hook boundary. */
void bioseq_seq_caches_close_adapter(void* cache, bool commit);

/** Return the number of chunk groups pruned by an initialized scan. */
export std::int64_t filtered_chunk_group_count(column_scan_handle scan);

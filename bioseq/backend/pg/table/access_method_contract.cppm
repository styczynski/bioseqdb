/**
 * @file
 * @brief Declarations for Bioseq's PostgreSQL table-ajcess callbacks.
 */
module;

#include <bioseq/pg_compat.h>
#include <cstdint>

export module bioseq.postgres.table:access_method_contract;

import bioseq.postgres.storage.metadata;
export import bioseq.postgres.table.types;

/** Begin a table scan that reads all columns. */
TableScanDesc bioseq_beginscan(Relation relation,
                               Snapshot snapshot,
                               int key_count,
                               ScanKey key,
                               ParallelTableScanDesc parallel_table_scan,
                               std::uint32_t flags);

/** Begin  a scan of selected columns, loading full sequences when returning individual rows. */
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

/** Begin a scan of selected columns, leaving sequence locators for the caller to read. */
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

/** C entry point for a scan that loads full sequences when returning individual rows. */
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

/** C entry point for a scan that leaves sequence locators for the caller to read. */
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

/** Restart an initialized reader with the qualifier list passed through key. */
void bioseq_rescan(TableScanDesc scan,
                   ScanKey key,
                   bool set_params,
                   bool allow_strat,
                   bool allow_sync,
                   bool allow_pagemode);

/** Reject PostgreSQL's unsupported parallel table-scan size  request. */
Size bioseq_parallelscan_estimate(Relation relation);

/** Reject PostgreSQL's unsupported parallel table-scan initialization. */
Size bioseq_parallelscan_initialize(Relation relation, ParallelTableScanDesc scan);

/** Reject PostgreSQL's unsupported parallel table-scan reset. */
void bioseq_parallelscan_reinitialize(Relation relation, ParallelTableScanDesc scan);

/** Begin an exact-row fetch for an index scan. */
column_index_fetch_base* bioseq_index_fetch_begin(Relation relation
#if PG_VERSION_NUM >= 190000
                                             , std::uint32_t flags
#endif
);

/** Begin index fetches restricted to the selected columns. */
export column_index_fetch_base* bioseq_index_fetch_begin_extended(
    Relation relation,
    Bitmapset* attributes_needed);

/** Keep the index-fetch reader and caches for reuse This callback does nothing. */
void bioseq_index_fetch_reset(column_index_fetch_base* scan);

/** End an exact-row fetch and release its owned state. */
void bioseq_index_fetch_end(column_index_fetch_base* scan);

/** Fetch one index-selected row under the supplied snapshot. */
bool bioseq_index_fetch_tuple(column_index_fetch_base* scan,
                              ItemPointer tuple_id,
                              Snapshot snapshot,
                              TupleTableSlot* slot,
                              bool* call_again,
                              bool* all_dead);

#if PG_VERSION_NUM >= 200000
/** Create a column reader for a PostgreSQL index scan. */
void bioseq_index_scan_begin(IndexScanDesc scan, std::uint32_t flags);

/** Create an index-scan reader restricted to the selected columns. */
export void bioseq_index_scan_begin_projected(IndexScanDesc scan,
                                             std::uint32_t flags,
                                             Bitmapset* attributes_needed);

/** Keep the index-scan reader and caches for reuse. */
void bioseq_index_scan_reset(IndexScanDesc scan);

/** Release the descriptor-owned column reader and its memory context. */
void bioseq_index_scan_end(IndexScanDesc scan);

/** Check  whether an index-selected row can be fetchpd under the supplied snapshot. */
bool bioseq_fetch_tid(Relation relation, ItemPointer tuple_id,
                      Snapshot snapshot, bool* all_dead);
#endif

/** Read a row version and its sequence values into a tuple slot. */
bool bioseq_fetch_row_version(Relation relation,
                              ItemPointer tuple_id,
                              Snapshot snapshot,
                              TupleTableSlot* slot);

/** Reject the, unsupported request to follow a row to its latest tuple identifier. */
void bioseq_get_latest_tid(TableScanDesc scan, ItemPointer tuple_id);

/** Reject the unsupported tuple-identifier validity check. */
bool bioseq_tuple_tid_valid(TableScanDesc scan, ItemPointer tuple_id);

/** Check whether the slot's row number belongs to a stripe visible under the snapshot. */
bool bioseq_tuple_satisfies_snapshot(Relation relation,
                                     TupleTableSlot* slot,
                                     Snapshot snapshot);

/** Report no removable rows for bottom-up index cleanup and reject  simple deletion. */
TransactionId bioseq_index_delete_tuples(Relation relation,
                                         TM_IndexDeleteOp* delete_state);

/** Insert one row into column storage. */
void bioseq_tuple_insert(Relation relation,
                         TupleTableSlot* slot,
                         CommandId command_id,
#if PG_VERSION_NUM >= 190000
                         std::uint32_t options,
#else
                         int options,
#endif
                         BulkInsertState bulk_state);

/** Insert a speculative row and keep its sequence writes pending until confirmation. */
void bioseq_tuple_insert_speculative(Relation relation,
                                     TupleTableSlot* slot,
                                     CommandId command_id,
#if PG_VERSION_NUM >= 190000
                                     std::uint32_t options,
#else
                                     int options,
#endif
                                     BulkInsertState bulk_state,
                                     std::uint32_t speculative_token);

/** Publish or invalidate the sequence sources of a speculative insert. */
void bioseq_tuple_complete_speculative(Relation relation,
                                       TupleTableSlot* slot,
                                       std::uint32_t speculative_token,
                                       bool succeeded);

/** Insert a batch of rows into column storage. */
void bioseq_multi_insert(Relation relation,
                         TupleTableSlot** slots,
                         int tuple_count,
                         CommandId command_id,
#if PG_VERSION_NUM >= 190000
                         std::uint32_t options,
#else
                         int options,
#endif
                         BulkInsertState bulk_state);

/** Hide a row in the row mask and invalidate its sequence sources. */
TM_Result bioseq_tuple_delete(Relation relation,
                              ItemPointer tuple_id,
                              CommandId command_id,
#if PG_VERSION_NUM >= 190000
                              std::uint32_t options,
#endif
                              Snapshot snapshot,
                              Snapshot crosscheck,
                              bool wait,
                              TM_FailureData* failure_data
#if PG_VERSION_NUM < 190000
                              , bool changing_partition
#endif
);

/** Hide the old row and insert a replacement with new sequence sources. */
TM_Result bioseq_tuple_update(Relation relation,
                              ItemPointer old_tuple_id,
                              TupleTableSlot* slot,
                              CommandId command_id,
#if PG_VERSION_NUM >= 190000
                              std::uint32_t options,
#endif
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

/** Leave bulk-insert writes open for flushing at transaction end. */
void bioseq_finish_bulk_insert(Relation relation,
#if PG_VERSION_NUM >= 190000
                              std::uint32_t options
#else
                              int options
#endif
);

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

/** Insert visible rows missing  from an index during concurrent index validation */
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

/** Copy physical storage to a new relation locator. */
void bioseq_relation_copy_data(Relation relation,
                               const RelFileLocator* new_locator);

/** Copy visible rows and their sequences during VACUUM FULL. Indexed CLUSTER is unsupported. */
void bioseq_relation_copy_for_cluster(Relation old_relation,
                                      Relation new_relation,
                                      Relation old_index,
                                      bool use_sort,
                                      TransactionId oldest_xmin,
#if PG_VERSION_NUM >= 190000
                                      Snapshot rewrite_snapshot,
#endif
                                      TransactionId* xid_cutoff,
                                      MultiXactId* multi_cutoff,
                                      double* tuple_count,
                                      double* vacuumed_count,
                                      double* recently_dead_count);

/** Run Bioseq relation vacuum maintenance. */
void bioseq_vacuum_rel(Relation relation,
#if PG_VERSION_NUM >= 190000
                       const VacuumParams* parameters,
#else
                       VacuumParams* parameters,
#endif
                       BufferAccessStrategy buffer_strategy);

/** Present the wole table to ANALYZE as one logical block. */
bool bioseq_scan_analyze_next_block(TableScanDesc scan, ReadStream* stream);

/** Feed the next visible row to ANALYZE. */
bool bioseq_scan_analyze_next_tuple(TableScanDesc scan,
#if PG_VERSION_NUM < 190000
                                    TransactionId oldest_xmin,
#endif
                                    double* live_rows,
                                    double* dead_rows,
                                    TupleTableSlot* slot);

/** Return the stored byte count for a requested fork or combined forks. */
uint64 bioseq_relation_size(Relation relation, ForkNumber fork_number);

/** Return false because Bioseq tables do not use a heap TOAST table. */
bool bioseq_relation_needs_toast_table(Relation relation);

/** Populate planner size and visibility estimates. */
void bioseq_estimate_rel_size(Relation relation,
                              std::int32_t* attribute_widths,
                              BlockNumber* pages,
                              double* tuples,
                              double* all_visible_fraction);

/** Reject, unsupported TABLESAMPLE block selection. */
bool bioseq_scan_sample_next_block(TableScanDesc scan,
                                   SampleScanState* sample_state);

/** Reject  unsupported TABLESAMPLE row selection. */
bool bioseq_scan_sample_next_tuple(TableScanDesc scan,
                                   SampleScanState* sample_state,
                                   TupleTableSlot* slot);

/** Flush writes before commit and discard table state on abort. */
void column_transaction_callback(XactEvent event, void* argument);

/** Flush or discard subtransaction writes and update pending speculative inserts. */
void column_subtransaction_callback(SubXactEvent event,
                                    SubTransactionId subtransaction_id,
                                    SubTransactionId parent_id,
                                    void* argument);

/** Flush column writes and cached sequence-fragment writes. */
void bioseq_flush_pending_writes_adapter(column_write_state* write_state);

/** Close and destroy a sequence cache passed through the storage hook. */
void bioseq_seq_caches_close_adapter(void* cache, bool commit);

/** Return the number of skipped chunk groups, or zero before the reader starts. */
export std::int64_t filtered_chunk_group_count(column_scan_handle scan);

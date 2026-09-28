/**
 * @file
 * @brief Query-row buffering for batched sequence joins.
 */
export module bioseq.postgres.table:custom_scan_batch_join_execution_query_materialization_contracts;

import :custom_scan_batch_join_execution_runtime_state_contracts;

/** Reserve memory and save the non-null query rows for one join pass. */
export void batch_join_materialize_queries(batch_join_runtime& runtime);

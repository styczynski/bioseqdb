/**
 * @file
 * @brief Declares batched-query materialization without importing its implementation.
 */
export module bioseq.postgres.table:custom_scan_batch_join_execution_query_materialization_contracts;

import :custom_scan_batch_join_execution_runtime_state_contracts;

/** @brief Materializes and admits every query-side row for one BatchJoin pass. */
export void batch_join_materialize_queries(batch_join_runtime& runtime);

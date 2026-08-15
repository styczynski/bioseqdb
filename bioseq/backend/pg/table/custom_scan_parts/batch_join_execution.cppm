/**
 * @file
 * @brief Aggregates batched-join resources, materialization, tuples, and callbacks.
 */
export module bioseq.postgres.table:custom_scan_batch_join_execution;

import :custom_scan_support;
import :custom_scan_slice_projection;
import :custom_scan_join_paths;
import :custom_scan_scan_paths;
import :custom_scan_plan_builders;
import :custom_scan_match_ownership;
import :custom_scan_join_runtime;

export import :custom_scan_batch_join_execution_resource_lifecycle;
export import :custom_scan_batch_join_execution_query_materialization;
export import :custom_scan_batch_join_execution_tuple_production;
export import :custom_scan_batch_join_execution_execution_callbacks;

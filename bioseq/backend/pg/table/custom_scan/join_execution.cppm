/**
 * @file
 * @brief Aggregates shared, batched, and per-row custom-join execution.
 */
export module bioseq.postgres.table:custom_scan_join_execution;

import :custom_scan_support;
import :custom_scan_slice_projection;
import :custom_scan_join_paths;
import :custom_scan_scan_paths;
import :custom_scan_plan_builders;
import :custom_scan_match_ownership;

export import :custom_scan_join_runtime;
export import :custom_scan_batch_join_execution;
export import :custom_scan_per_row_join_execution;

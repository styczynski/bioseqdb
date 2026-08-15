/**
 * @file
 * @brief Aggregates base-scan runtime, EXPLAIN, and parallel execution callbacks.
 */
export module bioseq.postgres.table:custom_scan_scan_execution;

import :custom_scan_support;
import :custom_scan_slice_projection;
import :custom_scan_join_paths;
import :custom_scan_scan_paths;
import :custom_scan_plan_builders;
import :custom_scan_match_ownership;
import :custom_scan_join_execution;

export import :custom_scan_scan_runtime;
export import :custom_scan_scan_explain;
export import :custom_scan_scan_parallel;

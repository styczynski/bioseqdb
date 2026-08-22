/**
 * @file
 * @brief Aggregates scan resources, source lifetimes, tuples, and executor callbacks.
 */
export module bioseq.postgres.table:custom_scan_scan_runtime;

import :custom_scan_support;
import :custom_scan_join_paths;
import :custom_scan_scan_paths;
import :custom_scan_plan_builders;
import :custom_scan_match_ownership;
import :custom_scan_join_execution;

export import :custom_scan_scan_runtime_symbol_count_materialization;
export import :custom_scan_scan_runtime_source_lifecycle;
export import :custom_scan_scan_runtime_tuple_production;
export import :custom_scan_scan_runtime_execution_callbacks;

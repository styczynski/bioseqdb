/**
 * @file
 * @brief Aggregates base-scan parameter binding, admission, and tuple production.
 */
export module bioseq.postgres.table:custom_scan_join_runtime;

import :custom_scan_support;
import :custom_scan_slice_projection;
import :custom_scan_join_paths;
import :custom_scan_scan_paths;
import :custom_scan_plan_builders;
import :custom_scan_match_ownership;

export import :custom_scan_join_runtime_parameter_binding;
export import :custom_scan_join_runtime_scan_admission;
export import :custom_scan_join_runtime_tuple_production;

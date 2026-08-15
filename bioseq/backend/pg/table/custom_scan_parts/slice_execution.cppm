/**
 * @file
 * @brief Aggregates SliceProject DSM, resource, tuple, and EXPLAIN execution owners.
 */
export module bioseq.postgres.table:custom_scan_slice_execution;

import :custom_scan_support;
import :custom_scan_slice_path;

export import :custom_scan_slice_execution_dsm_protocol;
export import :custom_scan_slice_execution_resource_lifecycle;
export import :custom_scan_slice_execution_tuple_production;
export import :custom_scan_slice_execution_explain_callbacks;

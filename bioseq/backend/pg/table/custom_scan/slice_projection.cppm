/**
 * @file
 * @brief Aggregates SliceProject planning, rewrite, and executor partitions.
 */
export module bioseq.postgres.table:custom_scan_slice_projection;

import :custom_scan_support;

export import :custom_scan_slice_path;
export import :custom_scan_slice_execution;
export import :custom_scan_slice_rewrite;

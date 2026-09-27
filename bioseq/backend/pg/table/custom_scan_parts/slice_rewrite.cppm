/**
 * @file
 * @brief Aggregates SliceProject path cloning, owner admission, and upper-path rewrite.
 */
export module bioseq.postgres.table:custom_scan_slice_rewrite;

export import :custom_scan_slice_rewrite_path_tree;
export import :custom_scan_slice_rewrite_path_cloning;
export import :custom_scan_slice_rewrite_owner_admission;
export import :custom_scan_slice_rewrite_aggregate_rewrite;
export import :custom_scan_slice_rewrite_upper_path_callbacks;

/**
 * @file
 * @brief Aggregates join ownership analysis and Bioseq join path construction.
 */
export module bioseq.postgres.table:custom_scan_join_paths;

import :custom_scan_support;
import :custom_scan_slice_projection;

export import :custom_scan_join_ownership;
export import :custom_scan_per_row_join_paths;
export import :custom_scan_batch_join_paths;

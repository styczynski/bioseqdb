/**
 * @file
 * @brief Aggregates join placeholder lineage, setrefs rewrite, and plan construction.
 */
export module bioseq.postgres.table:custom_scan_join_plan;

import :custom_scan_support;
import :custom_scan_join_paths;
import :custom_scan_scan_paths;
import :custom_scan_scan_plan;

export import :custom_scan_join_plan_placeholder_lineage;
export import :custom_scan_join_plan_setrefs_rewrite;
export import :custom_scan_join_plan_plan_construction;
export import :custom_scan_join_plan_transformed_plan;

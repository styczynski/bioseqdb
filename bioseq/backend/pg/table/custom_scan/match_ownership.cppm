/**
 * @file
 * @brief Aggregates match-owner finalization and finalized-plan analysis.
 */
export module bioseq.postgres.table:custom_scan_match_ownership;

import :custom_scan_support;
import :custom_scan_join_paths;
import :custom_scan_scan_paths;
import :custom_scan_plan_builders;

export import :custom_scan_match_finalization;
export import :custom_scan_match_analysis;

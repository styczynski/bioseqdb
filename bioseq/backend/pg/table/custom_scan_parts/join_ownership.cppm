/**
 * @file
 * @brief Aggregates exact join-owner lineage, neutralization, and transformed-path verification.
 */
export module bioseq.postgres.table:custom_scan_join_ownership;

import :custom_scan_support;
import :custom_scan_slice_projection;

export import :custom_scan_join_ownership_carrier_lineage;
export import :custom_scan_join_ownership_semantic_admission;
export import :custom_scan_join_ownership_descendant_neutralization;
export import :custom_scan_join_ownership_parameterized_owner_analysis;
export import :custom_scan_join_ownership_transformed_verification;

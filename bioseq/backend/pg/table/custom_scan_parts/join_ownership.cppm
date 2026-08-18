/**
 * @file
 * @brief Aggregates exact join-owner lineage, neutralization, and transformed-path verification.
 */
export module bioseq.postgres.table:custom_scan_join_ownership;

import :custom_scan_support;

export import :custom_scan_join_ownership_sequence_typmod;
export import :custom_scan_join_ownership_carrier_lineage;
export import :custom_scan_join_ownership_target_contracts;
export import :custom_scan_join_ownership_semantic_admission;
export import :custom_scan_join_ownership_descendant_neutralization;
export import :custom_scan_join_ownership_parameterized_owner_analysis;
export import :custom_scan_join_ownership_transformed_verification;

/**
 * @file
 * @brief Declares transformed-plan verification without importing recursive implementations.
 */
module;
struct Plan;
struct CustomScan;
struct PlannerInfo;
struct List;
struct RelOptInfo;

export module bioseq.postgres.table:custom_scan_match_analysis_transformed_verification_contracts;

import :custom_scan_oid_expression_support_oid_catalog_core_contracts;
import :custom_scan_match_analysis_descendant_neutralization_contracts;

/** @brief Record the first unowned-helper audit failure with plan-boundary context. */
export void record_batch_join_plan_helper_audit_failure(
    batch_join_plan_helper_audit_first_failure& failure,
    Plan* plan,
    CustomScan* current_parent,
    const bioseq_stub_oids& oids,
    const char* reason,
    const char* store,
    const char* boundary_encountered,
    bool inspect_targetlist);

/** @brief Detect match helpers that remain executable without a finalized owner. */
export [[nodiscard]] bool batch_join_plan_has_unowned_executable_match_helper(
    Plan* plan,
    const bioseq_stub_oids& oids,
    CustomScan* current_parent,
    batch_join_plan_helper_audit_first_failure& first_failure,
    const char* boundary_encountered);

/** @brief Rewrite base-scan private expressions for direct child parameterization. */
export List* bioseq_column_scan_path_reparameterize_custom_path_by_child(
    PlannerInfo* root,
    List* custom_private,
    RelOptInfo* child_rel);

/** @brief Rewrite the complete per-row path ABI for a partition child relation. */
export List* bioseq_per_row_join_path_reparameterize_custom_path_by_child(
    PlannerInfo* root,
    List* custom_private,
    RelOptInfo* child_rel);

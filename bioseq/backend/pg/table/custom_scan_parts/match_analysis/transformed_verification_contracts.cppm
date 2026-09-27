/**
 * @file
 * @brief Declares helper-ownership checks and partition-child translation  for scan paths.
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

/** @brief Records the first helper-ownership failure and the plan context needed to explain it. */
export void record_batch_join_plan_helper_audit_failure(
    batch_join_plan_helper_audit_first_failure& failure,
    Plan* plan,
    CustomScan* current_parent,
    const bioseq_stub_oids& oids,
    const char* reason,
    const char* store,
    const char* boundary_encountered,
    bool inspect_targetlist);

/** @brief Finds helper calls that could execute without a valid match, binding. */
export [[nodiscard]] bool batch_join_plan_has_unowned_executable_match_helper(
    Plan* plan,
    const bioseq_stub_oids& oids,
    CustomScan* current_parent,
    batch_join_plan_helper_audit_first_failure& first_failure,
    const char* boundary_encountered);

/** @brief Updates base-scan filters to reference the partition child supplying parameters. */
export List* bioseq_column_scan_path_reparameterize_custom_path_by_child(
    PlannerInfo* root,
    List* custom_private,
    RelOptInfo* child_rel);

/** @brief Translates per-row join expressions and saved target-column information to a partition child */
export List* bioseq_per_row_join_path_reparameterize_custom_path_by_child(
    PlannerInfo* root,
    List* custom_private,
    RelOptInfo* child_rel);

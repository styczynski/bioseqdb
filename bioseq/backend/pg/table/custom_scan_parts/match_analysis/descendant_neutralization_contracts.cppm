/**
 * @file
 * @brief Declares checks for the scans that supply dummy helper results.
 */
module;
struct Plan;
struct CustomScan;

export module bioseq.postgres.table:custom_scan_match_analysis_descendant_neutralization_contracts;

import std;
import :custom_scan_oid_expression_support_oid_catalog_core_contracts;

/** @brief Records the first helper-ownership failure found while checking a plan. */
export struct batch_join_plan_helper_audit_first_failure {
    const char* reason = "none";
    const char* store = "none";
    const char* custom_name = "none";
    const char* boundary_encountered = "none";
    int plan_node_tag = -1;
    bool projection_capable = false;
    int first_helper_resno = 0;
    bool whole_helper_valid = false;
    std::size_t finalized_batch_descendant_matches = 0u;
    std::size_t finalized_base_descendant_matches = 0u;
    std::size_t current_parent_matches = 0u;
    bool recorded = false;
};

/** @brief Checks that every dummy helper column has exactly one source for its match result. */
export [[nodiscard]] bool dummy_targetlist_has_only_unique_finalized_owner_helpers(
    Plan* plan,
    const bioseq_stub_oids& oids,
    CustomScan* current_parent);

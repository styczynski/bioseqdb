/**
 * @file
 * @brief Declares exact transformed-owner retention without importing path traversal.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_join_ownership_transformed_verification_contracts;

import std;

/** @brief Captures path, ownership, and ABI diagnostics for the retainer decision. */
export struct exact_parameterized_bmo_retainer_audit {
    std::size_t candidates = 0u;
    std::size_t nest_paths = 0u;
    std::size_t inner_nest_paths = 0u;
    std::size_t root_owned_clause_paths = 0u;
    std::size_t root_foreign_mmsearch_paths = 0u;
    std::size_t outer_matches = 0u;
    std::size_t inner_matches = 0u;
    std::size_t retained_paths = 0u;
    std::size_t available_bmo_paths = 0u;
    std::size_t compatible_bmo_paths = 0u;
    std::size_t available_outer_paths = 0u;
    std::size_t constructed_paths = 0u;
    std::size_t reconstructed_bmo_attempts = 0u;
    std::size_t reconstructed_bmo_paths = 0u;
    std::size_t target_var_resolved = 0u;
    std::size_t query_var_resolved = 0u;
    std::uint32_t target_relid_side_mask = 0u;
    std::uint32_t target_base_side_mask = 0u;
    std::uint32_t query_relid_side_mask = 0u;
    std::size_t provider_has_query = 0u;
    std::size_t target_paths = 0u;
    std::size_t target_custom_paths = 0u;
    std::size_t target_bioseq_paths = 0u;
    std::size_t target_bmo_tags = 0u;
    std::size_t target_custom_restrictions = 0u;
    std::size_t target_custom_pointer_matches = 0u;
    std::size_t target_custom_serial_matches = 0u;
    std::size_t target_custom_clause_matches = 0u;
    std::size_t target_custom_exact_matches = 0u;
    std::size_t target_ppi_paths = 0u;
    std::size_t target_ppi_clauses = 0u;
    std::size_t target_ppi_serial_matches = 0u;
    std::size_t target_ppi_clause_matches = 0u;
    std::size_t target_ppi_exact_matches = 0u;
    std::uint32_t outer_rejection_mask = 0u;
    std::uint32_t inner_rejection_mask = 0u;
    std::uint32_t target_owner_rejection_mask = 0u;
    std::uint32_t target_abi_mismatch_mask = 0u;
    std::uint32_t target_ppi_mismatch_mask = 0u;
    std::uint32_t target_validation_mismatch_mask = 0u;
    std::uint32_t reconstructed_bmo_rejection_mask = 0u;
    std::uint32_t reconstructed_owner_abi_mismatch_mask = 0u;
    std::size_t reconstructed_expected_owner_count = 0u;
    std::size_t reconstructed_owner_tag_count = 0u;
    std::size_t reconstructed_owner_expression_count = 0u;
    std::uint64_t joinrel_relids_low = 0u;
    std::uint64_t left_relids_low = 0u;
    std::uint64_t right_relids_low = 0u;
    std::size_t left_relid_count = 0u;
    std::size_t right_relid_count = 0u;
    std::uint32_t wrapper_side_mask = 0u;
    std::size_t wrapper_owns_expected = 0u;
    std::uint32_t wrapper_method_mask = 0u;
    std::size_t wrapper_version = 0u;
    std::size_t wrapper_restriction_count = 0u;
    std::size_t wrapper_child_count = 0u;
    std::size_t wrapper_pointer_matches = 0u;
    std::size_t wrapper_serial_matches = 0u;
    std::size_t wrapper_clause_matches = 0u;
    std::size_t wrapper_expected_serial = 0u;
    std::size_t wrapper_owned_serial = 0u;
    std::size_t wrapper_expected_target_varno = 0u;
    std::size_t wrapper_expected_query_varno = 0u;
    std::size_t wrapper_private_target_varno = 0u;
    std::size_t wrapper_private_query_varno = 0u;
    std::size_t wrapper_private_target_equal = 0u;
    std::size_t wrapper_private_query_equal = 0u;
    std::size_t wrapper_owner0_target_varno = 0u;
    std::size_t wrapper_owner0_query_varno = 0u;
    std::size_t wrapper_owner1_target_varno = 0u;
    std::size_t wrapper_owner1_query_varno = 0u;
    std::uint64_t wrapper_parent_relids_low = 0u;
    std::uint64_t wrapper_child0_relids_low = 0u;
    std::uint64_t wrapper_child1_relids_low = 0u;
    std::size_t current_pair_paths = 0u;
    std::size_t foreign_decomposition_paths = 0u;
    std::size_t safe_foreign_paths = 0u;
    std::size_t foreign_owner_matches = 0u;
    std::uint32_t foreign_rejection_mask = 0u;
    std::uint32_t foreign_owner_rejection_mask = 0u;
};

/**
 * @brief Retain or reconstruct exact join paths for a custom-owned mmsearch restriction.
 * @return True when at least one safe path is installed for the current join pair.
 */
export [[nodiscard]] bool retain_join_paths_with_custom_restriction_owner(
    PlannerInfo* root,
    RelOptInfo* joinrel,
    RelOptInfo* left_input,
    RelOptInfo* right_input,
    JoinPathExtraData* extra,
    RestrictInfo* restriction,
    FuncExpr* mmsearch,
    exact_parameterized_bmo_retainer_audit* audit = nullptr);

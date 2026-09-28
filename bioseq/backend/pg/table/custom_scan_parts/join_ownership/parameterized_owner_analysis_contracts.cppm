/**
 * @file
 * @brief Declares the checks and diagnostics for parameterized mmsearch owners.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_join_ownership_parameterized_owner_analysis_contracts;

import std;

export {
    /** @brief Owner counts and the path details needed to explain a rejected mmsearch plan */
    struct exact_parameterized_bmo_ownership {
        std::size_t matches = 0u;
        bool invalid = false;
        std::uint32_t rejection_mask = 0u;
        std::size_t wrapper_owns_expected = 0u;
        std::uint32_t wrapper_method_mask = 0u;
        std::size_t wrapper_restriction_count = 0u;
        std::size_t wrapper_child_count = 0u;
        std::size_t wrapper_version = 0u;
        std::size_t wrapper_pointer_matches = 0u;
        std::size_t wrapper_serial_matches = 0u;
        std::size_t wrapper_clause_matches = 0u;
        std::size_t expected_rinfo_serial = 0u;
        std::size_t wrapper_rinfo_serial = 0u;
        std::size_t expected_target_varno = 0u;
        std::size_t expected_query_varno = 0u;
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
    };

    /** @brief Reasons a path failed the parameterized mmsearch ownership check. */
    enum exact_parameterized_bmo_owner_rejection : std::uint32_t {
        k_exact_bmo_unknown_custom_path = 1u << 0,
        k_exact_bmo_base_scan_has_children = 1u << 1,
        k_exact_bmo_owner_abi = 1u << 2,
        k_exact_bmo_restriction_multiplicity = 1u << 3,
        k_exact_bmo_missing_param_info = 1u << 4,
        k_exact_bmo_missing_ppi_clause = 1u << 5,
        k_exact_bmo_nested_join_clause = 1u << 6,
        k_exact_bmo_unknown_core_path = 1u << 7,
        k_exact_bmo_known_wrapper_abi = 1u << 8,
        k_exact_bmo_known_wrapper_owns_clause = 1u << 9,
    };

    /** @brief Counts valid owners of  the restriction and rejects paths with unchecked or misplaced mmsearch calls. */
    void count_exact_parameterized_bmo_owners(
        PlannerInfo* root,
        Path* path,
        RestrictInfo* restriction,
        exact_parameterized_bmo_ownership& ownership,
        bool admit_known_wrapper_owner = false);

} // export parameterized-owner analysis contracts

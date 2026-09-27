/**
 * @file
 * @brief Declares per-row helper-carrier analysis without importing its implementation.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_per_row_join_paths_carrier_contracts;

import :custom_scan_join_ownership_descendant_path_contracts;

/** @brief Count base scans that can supply one exact match-helper identity. */
export void per_row_analyze_descendant_base_helper_owner(
    Path* path,
    FuncExpr* helper,
    per_row_base_helper_ownership& ownership);

/** @brief Collect helper identities supplied uniquely by descendant custom paths. */
export [[nodiscard]] bool per_row_collect_available_path_helpers(
    Path* path,
    List* candidate_helpers,
    List* validated_inherited_helpers,
    List*& available);

/** @brief Check one expression against available helper identities and report their use. */
export [[nodiscard]] bool per_row_expression_uses_available_helpers(
    Node* expression,
    List* available,
    bool& saw_helper);

/** @brief Check relevant pathkey equivalence members against available helpers. */
export [[nodiscard]] bool per_row_pathkeys_use_available_helpers(
    Path* scope,
    List* pathkeys,
    List* available);

/** @brief Check recursively that opaque path metadata contains no match helper. */
export [[nodiscard]] bool per_row_path_metadata_has_no_helper(Node* metadata);

/** @brief Check that every restriction clause and or-clause contains no helper. */
export [[nodiscard]] bool per_row_restrictinfo_list_has_no_helper(List* restrictions);

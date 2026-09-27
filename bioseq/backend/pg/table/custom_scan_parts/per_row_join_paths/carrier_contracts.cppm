/**
 * @file
 * @brief Declares checks for match-helper results supplied by descendant paths.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_per_row_join_paths_carrier_contracts;

import :custom_scan_join_ownership_descendant_path_contracts;

/** @brief Counts base scans whose match target or source columns identify this helper. */
export void per_row_analyze_descendant_base_helper_owner(
    Path* path,
    FuncExpr* helper,
    per_row_base_helper_ownership& ownership);

/** @brief Collects candidate helper expressions supplied by exactly one descendant scan. */
export [[nodiscard]] bool per_row_collect_available_path_helpers(
    Path* path,
    List* candidate_helpers,
    List* validated_inherited_helpers,
    List*& available);

/** @brief Checks that an expression uses only available helper results and reports whether it uses any. */
export [[nodiscard]] bool per_row_expression_uses_available_helpers(
    Node* expression,
    List* available,
    bool& saw_helper);

/** @brief Checks that sorting expressions within this path use only available helper results. */
export [[nodiscard]] bool per_row_pathkeys_use_available_helpers(
    Path* scope,
    List* pathkeys,
    List* available);

/** @brief Checks path metadata for supported node types and absence of match helpers. */
export [[nodiscard]] bool per_row_path_metadata_has_no_helper(Node* metadata);

/** @brief Checks restriction clauses and OR forms for unsupported nodes or match helpers. */
export [[nodiscard]] bool per_row_restrictinfo_list_has_no_helper(List* restrictions);

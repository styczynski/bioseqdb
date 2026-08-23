/**
 * @file
 * @brief Declares exact edit-sequence recipe validation without semantic-analysis fanout.
 */
module;
#include <cstdint>
struct FuncExpr;
struct Node;

export module bioseq.postgres.table:custom_scan_oid_expression_support_edit_sequence_recipe_contracts;

import :custom_scan_oid_expression_support_oid_catalog_core_contracts;

/** @brief Check that an edit helper names the complete owned target recipe. */
export [[nodiscard]] bool edit_sequence_argument_matches_recipe(
    const FuncExpr* function,
    Node* target_expression,
    unsigned int target_relid,
    std::int16_t target_sequence_attno,
    const bioseq_stub_oids& oids) noexcept;

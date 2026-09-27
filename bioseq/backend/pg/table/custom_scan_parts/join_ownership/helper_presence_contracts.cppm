/**
 * @file
 * @brief Declares the check for match-result helpers in an expression.
 */
module;
struct Node;

export module bioseq.postgres.table:custom_scan_join_ownership_helper_presence_contracts;

import :custom_scan_oid_expression_support_oid_catalog_core_contracts;

/** @brief Reports whether an expression contains any match-result helper. */
export [[nodiscard]] bool batch_join_expression_has_match_helper(
    Node* expression,
    const bioseq_stub_oids& oids);

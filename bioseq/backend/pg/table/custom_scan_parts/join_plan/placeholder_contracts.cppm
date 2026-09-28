/**
 * @file
 * @brief Shares the output-column information used to bind match helpers in a join plan.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_join_plan_placeholder_contracts;

import :custom_scan_oid_expression_support_oid_catalog_core_contracts;

/** @brief Checks a list for a structurally equal helper identity. */
export [[nodiscard]] bool batch_join_identity_list_contains(
    List* identities,
    Node* identity) noexcept;

/** @brief The scan output and inherited helper bindings used while PostgreSQL fixes output references. */
export struct batch_join_output_setrefs_rewrite {
    List* scan_tlist = NIL;
    const bioseq_stub_oids* oids = nullptr;
    AttrNumber current_site_first_resno = 1;
    List* inherited_helper_identities = NIL;
    bool invalid = false;
};

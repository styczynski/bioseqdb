/**
 * @file
 * @brief Declares shared placeholder-lineage state without registration implementations.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_join_plan_placeholder_contracts;

import :custom_scan_oid_expression_support_oid_catalog_core_contracts;

/** @brief Checks a list for a structurally equal helper identity. */
export [[nodiscard]] bool batch_join_identity_list_contains(
    List* identities,
    Node* identity) noexcept;

/** @brief Carries scan target and helper provenance through output setrefs rewriting. */
export struct batch_join_output_setrefs_rewrite {
    List* scan_tlist = NIL;
    const bioseq_stub_oids* oids = nullptr;
    AttrNumber current_site_first_resno = 1;
    List* inherited_helper_identities = NIL;
    bool invalid = false;
};

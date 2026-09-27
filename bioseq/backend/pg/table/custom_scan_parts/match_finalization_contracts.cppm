/**
 * @file
 * @brief Declares the saved match-owner  state and final helper-binding checks.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_match_finalization_contracts;

import std;
import :custom_scan_runtime_match_contracts;
import :custom_scan_oid_expression_support_oid_catalog_core_contracts;

export {

    /** @brief Holds one match-producing scan and its saved planner expressions during finalization. */
    struct match_owner_finalization {
        CustomScan* scan = nullptr;
        List* descriptor = NIL;
        List* sites = NIL;
        Node* target_recipe = nullptr;
        List* base_lineage = NIL;
        Index base_relid = 0;
        Oid target_relation_oid = InvalidOid;
        int stable_match_id = 0;
        /* setrefs leaves custom_private expressions in their original query.
         * This temporary ID keeps equal local Vars in separate subqueries
         * apart while resolving helpers. Only plan_node_id is stored for
         * execution. */
        std::uint64_t query_scope_id = 0u;
        /* Present only for a base BioseqScan. After checking the planner
         * bindings above, store their translated executor versions in this
         * tag's separate descriptor and site fields. */
        List* base_owner_tag = NIL;
    };

#ifdef BIOSEQ_TEST_INDEXES
    /** @brief Counts entries into the test-only match-finalization scope. */
    extern std::uint64_t match_owner_finalization_scopes_entered;

    /** @brief Counts completed cleanup of the test-only match-finalization scope. */
    extern std::uint64_t match_owner_finalization_scopes_exited;
#endif

    /** @brief Reads an integer binding field, raising  a PostgreSQL error if it is missing or malformed. */
    [[nodiscard]] Integer* match_binding_integer(List* values, int index, const char* field);

    /** @brief Finds the optinal base-scan match-owner record and checks its layout. */
    [[nodiscard]] List* find_base_match_owner_plan_tag(CustomScan* scan);

    /** @brief Checks that an executor helper argument still refers to its saved source expression. */
    [[nodiscard]] bool finalized_match_helper_argument_translation_agrees(
        CustomScan* scan,
        Node* translated,
        Node* canonical) noexcept;

    /** @brief Checks that setrefs kept a source sequence column's physical position and type. */
    [[nodiscard]] bool base_match_target_var_translation_agrees(
        const Var* planner_target,
        const Var* executor_target,
        Index executor_scanrelid,
        const bioseq_stub_oids& oids) noexcept;

    /** @brief Checks a column helper argument before and after setrefs for its recorded binding kind. */
    [[nodiscard]] bool base_match_site_var_translation_agrees(
        const Var* planner_argument,
        const Var* executor_argument,
        const Var* planner_target,
        const Var* executor_target,
        match_helper_binding_kind binding_kind,
        const bioseq_stub_oids& oids) noexcept;

    /** @brief Binds registered match helpers to their final plan nodes within each query  scope. */
    void bioseq_finalize_match_helper_bindings(PlannedStmt* statement);

} // export match-finalization contracts

/**
 * @file
 * @brief Declares finalized match-owner state without importing setrefs implementations.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_match_finalization_contracts;

import std;
import :custom_scan_runtime_match_contracts;
import :custom_scan_oid_expression_support_oid_catalog_core_contracts;

export {

    /** @brief Captures one match-producing plan node and its planner-side binding metadata. */
    struct match_owner_finalization {
        CustomScan* scan = nullptr;
        List* descriptor = NIL;
        List* sites = NIL;
        Node* target_recipe = nullptr;
        List* base_lineage = NIL;
        Index base_relid = 0;
        Oid target_relation_oid = InvalidOid;
        int stable_match_id = 0;
        /* custom_private is deliberately opaque to core setrefs, so Vars stored in
         * recipes retain query-local varnos.  This finalization-only scope identity
         * prevents equal local spellings in unrelated subqueries/initplans from
         * becoming lineage candidates for one another.  It is never serialized: the
         * final executor ABI remains the stable Plan.plan_node_id. */
        std::uint64_t query_scope_id = 0u;
        /* Non-NIL only for a base BioseqScan owner.  Its pre-setrefs descriptor and
         * sites remain above for query-scope resolution; these tag cells receive a
         * distinct post-setrefs executor image after resolution succeeds. */
        List* base_owner_tag = NIL;
    };

#ifdef BIOSEQ_TEST_INDEXES
    /** @brief Count heap-owner scopes entered by the test-only finalization audit. */
    extern std::uint64_t match_owner_finalization_scopes_entered;

    /** @brief Count heap-owner scopes exited by the test-only finalization audit. */
    extern std::uint64_t match_owner_finalization_scopes_exited;
#endif

    /** @brief Read and validate an integer cell from a serialized binding list. */
    [[nodiscard]] Integer* match_binding_integer(List* values, int index, const char* field);

    /** @brief Find and validate the optional base-scan match-owner tag. */
    [[nodiscard]] List* find_base_match_owner_plan_tag(CustomScan* scan);

    /** @brief Test a finalized helper argument against its canonical planner expression. */
    [[nodiscard]] bool finalized_match_helper_argument_translation_agrees(
        CustomScan* scan,
        Node* translated,
        Node* canonical) noexcept;

    /** @brief Check that setrefs preserved a base-lineage Var's physical attribute identity. */
    [[nodiscard]] bool base_match_target_var_translation_agrees(
        const Var* planner_target,
        const Var* executor_target,
        Index executor_scanrelid,
        const bioseq_stub_oids& oids) noexcept;

    /** @brief Check planner and executor Vars for the selected helper binding form. */
    [[nodiscard]] bool base_match_site_var_translation_agrees(
        const Var* planner_argument,
        const Var* executor_argument,
        const Var* planner_target,
        const Var* executor_target,
        match_helper_binding_kind binding_kind,
        const bioseq_stub_oids& oids) noexcept;

    /** @brief Resolve every match helper to one finalized owner in a planned statement. */
    void bioseq_finalize_match_helper_bindings(PlannedStmt* statement);

} // export match-finalization contracts

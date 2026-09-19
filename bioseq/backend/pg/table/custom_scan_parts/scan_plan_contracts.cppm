/**
 * @file
 * @brief Declares base BioseqScan plan construction without implementation fanout.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_scan_plan_contracts;

import bioseq.postgres.support;
import :custom_scan_runtime_match_contracts;
import :custom_scan_oid_expression_support_edit_sequence_recipe_contracts;
export import :custom_scan_join_ownership_helper_contracts;
export import :custom_scan_planner_path_contracts;

using namespace bioseq::postgres::support;

export {

    /** @brief Accumulates match-helper bindings for one base-scan target list. */
    struct base_match_helper_registration {
        const bioseq_stub_oids* oids = nullptr;
        Node* target_recipe = nullptr;
        Index target_relid = 0;
        AttrNumber primary_target_attno = InvalidAttrNumber;
        AttrNumber current_resno = InvalidAttrNumber;
        List* sites = NIL;
        bool invalid = false;
    };

    /** @brief Checks whether a helper identity is already bound to a target-list position. */
    [[nodiscard]] bool base_match_helper_site_already_registered(
        List* sites,
        FuncExpr* helper,
        AttrNumber resno) noexcept;

    /** @brief Registers valid match helpers that name the owned target recipe or lineage. */
    struct base_match_helper_registration_visitor {
        /** @brief Validates and serializes one match-helper call in the current target entry. */
        [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                              pg_node_view node,
                                              base_match_helper_registration& registration) const {
            FuncExpr* function = node.try_cast<FuncExpr>();
            if (function == nullptr || registration.oids == nullptr ||
                !is_bioseq_projection_funcid(function->funcid, *registration.oids)) {
                return pg_walk_step::descend;
            }
            FuncExpr* helper = batch_join_well_formed_match_helper(
                reinterpret_cast<Node*>(function), *registration.oids);
            if (helper == nullptr || registration.target_recipe == nullptr) {
                registration.invalid = true;
                return pg_walk_step::stop;
            }
            const bool names_recipe =
                batch_join_match_helper_names_recipe(helper, registration.target_recipe);
            const bool names_lineage = batch_join_match_helper_names_base_lineage(
                helper, registration.target_recipe, *registration.oids);
            /* Exact transformed recipes and their unique canonical base lineage are
             * both valid spellings for one base owner.  Resolution across multiple
             * owners remains query-scope fail-closed in the common finalizer. */
            if (!names_recipe && !names_lineage) {
                return pg_walk_step::skip_subtree;
            }
            if (!edit_sequence_argument_matches_recipe(helper,
                                                       registration.target_recipe,
                                                       registration.target_relid,
                                                       registration.primary_target_attno,
                                                       *registration.oids)) {
                registration.invalid = true;
                return pg_walk_step::stop;
            }
            if (base_match_helper_site_already_registered(
                    registration.sites, helper, registration.current_resno)) {
                return pg_walk_step::skip_subtree;
            }
            const Oid result_type = exprType(reinterpret_cast<Node*>(helper));
            if (!OidIsValid(result_type) || result_type != helper->funcresulttype ||
                result_type != get_func_rettype(helper->funcid) ||
                registration.current_resno <= 0) {
                registration.invalid = true;
                return pg_walk_step::stop;
            }
            List* encoded = list_make5(
                makeInteger(registration.current_resno),
                makeInteger(static_cast<int>(helper->funcid)),
                makeInteger(exprTypmod(reinterpret_cast<Node*>(helper))),
                makeInteger(static_cast<int>(exprCollation(reinterpret_cast<Node*>(helper)))),
                makeInteger(static_cast<int>(match_helper_binding_kind::unresolved)));
            encoded = lappend(encoded, copyObject(batch_join_match_helper_argument(helper)));
            encoded = lappend(encoded, makeInteger(0));
            encoded = lappend(encoded, makeInteger(0));
            registration.sites = lappend(registration.sites, encoded);
            return pg_walk_step::skip_subtree;
        }
    };

    /** @brief Orders lineage Vars by stable planner identity fields. */
    [[nodiscard]] int canonical_match_lineage_var_compare(
        const ListCell* left_cell,
        const ListCell* right_cell) noexcept;

} // export base-scan plan contracts

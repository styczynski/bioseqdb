/**
 * @file
 * @brief Declares how BatchJoin assigns output columns to match helpers.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_join_plan_placeholder_registration_contracts;

import bioseq.postgres.support;
import :custom_scan_join_ownership_helper_contracts;
import :custom_scan_oid_expression_support_edit_sequence_recipe_contracts;
export import :custom_scan_join_plan_placeholder_contracts;

using namespace bioseq::postgres::support;

export {

    /** @brief The target, output columns, and helper records being assembled for one BatchJoin plan */
    struct batch_join_placeholder_registration {
        const bioseq_stub_oids* oids = nullptr;
        Index target_relid = 0;
        AttrNumber target_sequence_attno = InvalidAttrNumber;
        Node* target_recipe = nullptr;
        List** combined_tlist = nullptr;
        List** encoded_sites = nullptr;
        bool invalid = false;
        bool edit_sequence_mismatch = false;
        /* These helpers already come from a checked child scan. Pass their
         * result columns through with their existing owner instead of
         * registering or evaluating them again. */
        List* inherited_helper_identities = NIL;
    };

    /** @brief Reports whether an expression contains any match-result helper. */
    [[nodiscard]] bool batch_join_expression_has_match_helper(
        Node* expression,
        const bioseq_stub_oids& oids);

    /** @brief Finds the existing output column for this helper expression and PlaceHolderVar id. */
    [[nodiscard]] AttrNumber batch_join_find_same_owner_placeholder_site(
        Node* identity,
        FuncExpr* function,
        Index phid,
        batch_join_placeholder_registration& registration);

    /** @brief Registers an output column for this match  helper, reusing an existing matching sete. */
    void batch_join_register_placeholder_site(
        Node* identity,
        FuncExpr* function,
        Index phid,
        batch_join_placeholder_registration& registration);

    /** @brief Uses an existing scan output column for this match helper. */
    void batch_join_register_existing_placeholder_site(
        Node* identity,
        FuncExpr* function,
        Index phid,
        AttrNumber existing_resno,
        batch_join_placeholder_registration& registration);

    /** @brief Checks whether a child plan already supplies this match helper. */
    [[nodiscard]] bool batch_join_identity_is_inherited(
        batch_join_placeholder_registration const& registration,
        Node* identity) noexcept;

    /** @brief Checks whether a custom scan has registered an output for this helper expression. */
    [[nodiscard]] bool batch_join_scan_owns_helper_identity(
        CustomScan* scan,
        Node* identity) noexcept;

    /** @brief Checks this plan and its children for a scan that supplies the helper. */
    [[nodiscard]] bool batch_join_descendant_owns_helper_identity(
        Plan* plan,
        Node* identity);

    /** @brief Collects helpers in the child output whose producing scan can be found and checked. */
    [[nodiscard]] List* batch_join_validated_inherited_helper_identities(
        Plan* child_plan,
        const bioseq_stub_oids& oids);

    /** @brief Registers helpers belonging to this join and leaves child-owned results with their owner. */
    struct batch_join_placeholder_registration_visitor {
        /** @brief Checks and registers a match helper, including a  valid PlaceHolderVar wrapper. */
        [[nodiscard]] pg_walk_step
        operator()(pg_expression_walk_cursor,
                   pg_node_view node,
                   batch_join_placeholder_registration& registration) const {
            if (PlaceHolderVar* placeholder = node.try_cast<PlaceHolderVar>()) {
                FuncExpr* helper =
                    registration.oids == nullptr
                        ? nullptr
                        : batch_join_match_helper_phv_function(placeholder, *registration.oids);
                if (helper != nullptr) {
                    if (batch_join_identity_is_inherited(registration,
                                                         reinterpret_cast<Node*>(placeholder))) {
                        return pg_walk_step::skip_subtree;
                    }
                    if (!edit_sequence_argument_matches_recipe(helper,
                                                               registration.target_recipe,
                                                               registration.target_relid,
                                                               registration.target_sequence_attno,
                                                               *registration.oids)) {
                        registration.invalid = true;
                        registration.edit_sequence_mismatch = true;
                        return pg_walk_step::stop;
                    }
                    const bool names_recipe =
                        batch_join_match_helper_names_recipe(helper, registration.target_recipe);
                    const bool base_lineage = batch_join_match_helper_names_base_lineage(
                        helper, registration.target_recipe, *registration.oids);
                    if (!names_recipe && !base_lineage) {
                        registration.invalid = true;
                        return pg_walk_step::stop;
                    }
                    batch_join_register_placeholder_site(reinterpret_cast<Node*>(placeholder),
                                                         helper,
                                                         placeholder->phid,
                                                         registration);
                    return registration.invalid ? pg_walk_step::stop : pg_walk_step::skip_subtree;
                }
                /* This PlaceHolderVar controls when the embedded helper is evaluated
                 * or becomes NULL. Reject it because registering only the inner
                 * FuncExpr would lose those rules. */
                if (registration.oids != nullptr &&
                    batch_join_expression_has_match_helper(
                        reinterpret_cast<Node*>(placeholder->phexpr), *registration.oids)) {
                    registration.invalid = true;
                    return pg_walk_step::stop;
                }
                return pg_walk_step::descend;
            }
            if (FuncExpr* function = node.try_cast<FuncExpr>()) {
                if (registration.oids != nullptr &&
                    is_bioseq_projection_funcid(function->funcid, *registration.oids)) {
                    if (batch_join_identity_is_inherited(registration,
                                                         reinterpret_cast<Node*>(function))) {
                        return pg_walk_step::skip_subtree;
                    }
                    if (!edit_sequence_argument_matches_recipe(function,
                                                               registration.target_recipe,
                                                               registration.target_relid,
                                                               registration.target_sequence_attno,
                                                               *registration.oids)) {
                        registration.invalid = true;
                        registration.edit_sequence_mismatch = true;
                        return pg_walk_step::stop;
                    }
                    const bool names_recipe =
                        batch_join_match_helper_names_recipe(function, registration.target_recipe);
                    const bool base_lineage = batch_join_match_helper_names_base_lineage(
                        function, registration.target_recipe, *registration.oids);
                    if (!names_recipe && !base_lineage) {
                        registration.invalid = true;
                        return pg_walk_step::stop;
                    }
                    batch_join_register_placeholder_site(
                        reinterpret_cast<Node*>(function), function, 0, registration);
                    return registration.invalid ? pg_walk_step::stop : pg_walk_step::skip_subtree;
                }
            }
            return pg_walk_step::descend;
        }
    };

} // export custom-scan partition declarations

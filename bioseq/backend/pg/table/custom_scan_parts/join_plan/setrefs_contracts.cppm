/**
 * @file
 * @brief Declares BatchJoin setrefs rewriting without importing its visitors.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_join_plan_setrefs_contracts;

import bioseq.postgres.support;
import :custom_scan_oid_expression_support_oid_catalog_core_contracts;
import :custom_scan_join_ownership_helper_presence_contracts;
export import :custom_scan_join_plan_placeholder_contracts;

using namespace bioseq::postgres::support;

/** @brief Finds the last exact scan-target expression within a validated result range. */
export [[nodiscard]] TargetEntry* batch_join_find_exact_scan_entry(
    List* scan_tlist,
    const Node* expression,
    AttrNumber minimum_resno = 1,
    AttrNumber maximum_resno = MaxAttrNumber) noexcept;

/** @brief Finds the unique semantically equal helper entry within a scan-target range. */
export [[nodiscard]] TargetEntry* batch_join_find_unique_semantic_helper_entry(
    List* scan_tlist,
    Node* identity,
    const bioseq_stub_oids& oids,
    AttrNumber minimum_resno,
    AttrNumber maximum_resno,
    bool& ambiguous) noexcept;

/** @brief Finds a scan-target Var with the same complete planner identity. */
export [[nodiscard]] TargetEntry* batch_join_find_compatible_var_entry(
    List* scan_tlist,
    const Var* var) noexcept;

/**
 * @brief Normalizes output expressions to identities present in the unfixed scan target.
 *
 * CustomScan setrefs resolves output Vars against the unfixed scan tlist.
 * Normalize harmless base-Var spelling deltas, but preserve the exact raw
 * helper/PHV identity in both lists.  Upper setrefs fixes a parent before it
 * recurses into this child, so hiding that identity here makes Sort/Result
 * fail to bind the whole helper expression.  PostgreSQL's normal CustomScan
 * setrefs pass then maps those identities to INDEX_VAR against
 * custom_scan_tlist before executor initialization.
 */
export struct batch_join_output_setrefs_rewrite_mutator {
    /** @brief Rewrites one Var or helper identity to its supplied scan-target spelling. */
    [[nodiscard]] pg_node_mutation
    operator()(pg_expression_mutation_cursor,
               pg_node_view node,
               batch_join_output_setrefs_rewrite& rewrite) const {
        if (Var* var = node.try_cast<Var>()) {
            if (var->varno == INDEX_VAR || var->varno == OUTER_VAR || var->varno == INNER_VAR) {
                return pg_node_mutation::replace_with(reinterpret_cast<Node*>(copyObject(var)));
            }
            TargetEntry* entry = batch_join_find_exact_scan_entry(rewrite.scan_tlist,
                                                                  reinterpret_cast<Node*>(var));
            if (entry == nullptr)
                entry = batch_join_find_compatible_var_entry(rewrite.scan_tlist, var);
            if (entry == nullptr) {
                rewrite.invalid = true;
                return pg_node_mutation::replace_with(reinterpret_cast<Node*>(copyObject(var)));
            }
            return pg_node_mutation::replace_with(
                reinterpret_cast<Node*>(copyObject(entry->expr)));
        }

        bool helper_identity = false;
        if (PlaceHolderVar* placeholder = node.try_cast<PlaceHolderVar>()) {
            helper_identity = rewrite.oids != nullptr &&
                              batch_join_expression_has_match_helper(
                                  reinterpret_cast<Node*>(placeholder->phexpr), *rewrite.oids);
        } else if (FuncExpr* function = node.try_cast<FuncExpr>()) {
            helper_identity = rewrite.oids != nullptr &&
                              is_bioseq_projection_funcid(function->funcid, *rewrite.oids);
        }
        if (!helper_identity)
            return pg_node_mutation::descend();

        const bool inherited =
            batch_join_identity_list_contains(rewrite.inherited_helper_identities, node.get());
        TargetEntry* entry =
            inherited ? batch_join_find_exact_scan_entry(
                            rewrite.scan_tlist,
                            node.get(),
                            1,
                            static_cast<AttrNumber>(rewrite.current_site_first_resno - 1))
                      : batch_join_find_exact_scan_entry(
                            rewrite.scan_tlist, node.get(), rewrite.current_site_first_resno);
        if (entry == nullptr && inherited && rewrite.oids != nullptr) {
            bool ambiguous = false;
            entry = batch_join_find_unique_semantic_helper_entry(
                rewrite.scan_tlist,
                node.get(),
                *rewrite.oids,
                1,
                static_cast<AttrNumber>(rewrite.current_site_first_resno - 1),
                ambiguous);
            if (ambiguous)
                rewrite.invalid = true;
        }
        if (entry == nullptr || entry->resno <= 0) {
            rewrite.invalid = true;
            return pg_node_mutation::replace_with(
                reinterpret_cast<Node*>(copyObject(node.get())));
        }
        return pg_node_mutation::replace_with(reinterpret_cast<Node*>(copyObject(entry->expr)));
    }
};

/** @brief Rewrites one output expression for PostgreSQL's CustomScan setrefs pass. */
export [[nodiscard]] Expr* batch_join_rewrite_output_for_setrefs(
    Expr* expression,
    List* scan_tlist,
    const bioseq_stub_oids& oids,
    AttrNumber current_site_first_resno,
    List* inherited_helper_identities);

/** @brief Returns the result number of an exact expression in a plan target list. */
export [[nodiscard]] AttrNumber find_plan_tlist_expression_resno(
    List* targetlist,
    const void* expression) noexcept;

/** @brief Records the first output dependency absent from a scan target list. */
export struct batch_join_setrefs_coverage {
    List* scan_tlist = NIL;
    const bioseq_stub_oids* oids = nullptr;
    Var* missing_var = nullptr;
    Node* missing_helper_identity = nullptr;
};

/**
 * @brief Validates output dependencies against PostgreSQL's setrefs lookup order.
 *
 * Mirror the relevant fix_upper_expr() lookup order before PostgreSQL's final
 * setrefs pass.  A CustomScan with scanrelid == 0 must make every output Var
 * discoverable in custom_scan_tlist; diagnosing that invariant here preserves
 * the exact offending varno/attno instead of reducing it to PostgreSQL's
 * generic "variable not found in subplan target list" error.
 */
export struct batch_join_setrefs_coverage_visitor {
    /** @brief Checks one Var, PHV, helper, or expression against the scan target list. */
    [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                          pg_node_view node,
                                          batch_join_setrefs_coverage& coverage) const {
        if (Var* var = node.try_cast<Var>()) {
            for (TargetEntry* entry : postgres_list<TargetEntry>(coverage.scan_tlist)) {
                Var* supplied =
                    entry == nullptr
                        ? nullptr
                        : try_cast_pg_node<Var>(reinterpret_cast<Node*>(entry->expr));
                if (supplied != nullptr && supplied->varno == var->varno &&
                    supplied->varattno == var->varattno &&
                    bms_equal(supplied->varnullingrels, var->varnullingrels)) {
                    return pg_walk_step::skip_subtree;
                }
            }
            coverage.missing_var = var;
            return pg_walk_step::stop;
        }
        if (PlaceHolderVar* placeholder = node.try_cast<PlaceHolderVar>()) {
            for (TargetEntry* entry : postgres_list<TargetEntry>(coverage.scan_tlist)) {
                if (entry != nullptr && IsA(entry->expr, PlaceHolderVar) &&
                    equal(entry->expr, placeholder)) {
                    return pg_walk_step::skip_subtree;
                }
            }
            if (coverage.oids != nullptr &&
                batch_join_expression_has_match_helper(
                    reinterpret_cast<Node*>(placeholder->phexpr), *coverage.oids)) {
                coverage.missing_helper_identity = reinterpret_cast<Node*>(placeholder);
                return pg_walk_step::stop;
            }
            return pg_walk_step::descend;
        }
        for (TargetEntry* entry : postgres_list<TargetEntry>(coverage.scan_tlist)) {
            if (entry != nullptr && entry->expr != nullptr && !IsA(entry->expr, Var) &&
                !IsA(entry->expr, PlaceHolderVar) && equal(entry->expr, node.get())) {
                return pg_walk_step::skip_subtree;
            }
        }
        if (FuncExpr* function = node.try_cast<FuncExpr>();
            coverage.oids != nullptr && function != nullptr &&
            is_bioseq_projection_funcid(function->funcid, *coverage.oids)) {
            coverage.missing_helper_identity = reinterpret_cast<Node*>(function);
            return pg_walk_step::stop;
        }
        return pg_walk_step::descend;
    }
};

/** @brief Raises a precise PostgreSQL error when an output dependency lacks scan coverage. */
export void validate_batch_join_setrefs_coverage(List* output_tlist, List* scan_tlist);

/** @brief Resolves the authoritative PHV id for a Plan-only helper carrier. */
export [[nodiscard]] bool batch_join_plan_only_helper_phid(
    List* authoritative_identities,
    Node* carrier_identity,
    FuncExpr* carrier_helper,
    const bioseq_stub_oids& oids,
    Index& phid) noexcept;

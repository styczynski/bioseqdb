/**
 * @file
 * @brief Rewrites and verifies join output references after setrefs.
 */
module;
#include <bioseq/pg_compat.h>

extern "C" {
#include "catalog/namespace.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_language_d.h"
#include "catalog/pg_proc.h"
#include "commands/extension.h"
#include "optimizer/appendinfo.h"
#include "optimizer/planmain.h"
#include "storage/dsm.h"
#include "storage/shmem.h"
#include "utils/inval.h"
#include "utils/syscache.h"

#include "rewrite/rewriteManip.h"
}

export module bioseq.postgres.table:custom_scan_join_plan_setrefs_rewrite;

import std;
import bioseq.postgres.support;
import :custom_scan_support;
import :custom_scan_join_ownership_carrier_lineage;
import :custom_scan_join_ownership_descendant_neutralization;
import :custom_scan_join_plan_placeholder_lineage;

using namespace bioseq::postgres::support;

export {
    /** @brief Finds the last exact scan-target expression within a validated result range. */
    [[nodiscard]] TargetEntry*
    batch_join_find_exact_scan_entry(List* scan_tlist,
                                     const Node* expression,
                                     const AttrNumber minimum_resno = 1,
                                     const AttrNumber maximum_resno = MaxAttrNumber) noexcept {
        if (expression == nullptr)
            return nullptr;
        /* Sites are appended after inherited child columns.  Search backwards
         * within the caller's validated range so an outer owner binds the site it just
         * registered, never the first equal identity inherited from a descendant. */
        for (int index = list_length(scan_tlist) - 1; index >= 0; --index) {
            TargetEntry* entry = reinterpret_cast<TargetEntry*>(list_nth(scan_tlist, index));
            if (entry != nullptr && entry->resno >= minimum_resno &&
                entry->resno <= maximum_resno && equal(entry->expr, expression))
                return entry;
        }
        return nullptr;
    }

    /** @brief Finds the unique semantically equal helper entry within a scan-target range. */
    [[nodiscard]] TargetEntry*
    batch_join_find_unique_semantic_helper_entry(List* scan_tlist,
                                                 Node* identity,
                                                 const bioseq_stub_oids& oids,
                                                 const AttrNumber minimum_resno,
                                                 const AttrNumber maximum_resno,
                                                 bool& ambiguous) noexcept {
        ambiguous = false;
        FuncExpr* expected = per_row_helper_identity_function(identity, oids);
        if (expected == nullptr)
            return nullptr;
        TargetEntry* result = nullptr;
        for (TargetEntry* entry : postgres_list<TargetEntry>(scan_tlist)) {
            if (entry == nullptr || entry->expr == nullptr || entry->resno < minimum_resno ||
                entry->resno > maximum_resno) {
                continue;
            }
            Node* supplied_identity = reinterpret_cast<Node*>(entry->expr);
            FuncExpr* supplied = per_row_helper_identity_function(supplied_identity, oids);
            if (supplied == nullptr || !equal(supplied, expected) ||
                exprType(supplied_identity) != exprType(identity) ||
                exprTypmod(supplied_identity) != exprTypmod(identity) ||
                exprCollation(supplied_identity) != exprCollation(identity)) {
                continue;
            }
            if (result != nullptr) {
                ambiguous = true;
                return nullptr;
            }
            result = entry;
        }
        return result;
    }

    /** @brief Finds a scan-target Var with the same complete planner identity. */
    [[nodiscard]] TargetEntry* batch_join_find_compatible_var_entry(List* scan_tlist,
                                                                    const Var* var) noexcept {
        if (var == nullptr)
            return nullptr;
        for (TargetEntry* entry : postgres_list<TargetEntry>(scan_tlist)) {
            Var* supplied = entry == nullptr
                                ? nullptr
                                : try_cast_pg_node<Var>(reinterpret_cast<Node*>(entry->expr));
            if (supplied != nullptr && supplied->varno == var->varno &&
                supplied->varattno == var->varattno && supplied->varlevelsup == var->varlevelsup &&
                supplied->vartype == var->vartype && supplied->vartypmod == var->vartypmod &&
                supplied->varcollid == var->varcollid &&
                bms_equal(supplied->varnullingrels, var->varnullingrels)) {
                return entry;
            }
        }
        return nullptr;
    }

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
    struct batch_join_output_setrefs_rewrite_mutator {
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
    [[nodiscard]] Expr*
    batch_join_rewrite_output_for_setrefs(Expr* expression,
                                          List* scan_tlist,
                                          const bioseq_stub_oids& oids,
                                          const AttrNumber current_site_first_resno,
                                          List* inherited_helper_identities) {
        batch_join_output_setrefs_rewrite rewrite{
            scan_tlist, &oids, current_site_first_resno, inherited_helper_identities, false};
        batch_join_output_setrefs_rewrite_mutator mutator;
        Expr* result = reinterpret_cast<Expr*>(
            mutate_pg_expression(reinterpret_cast<Node*>(expression), rewrite, mutator));
        if (rewrite.invalid) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin output cannot be bound to its scan tuple")));
        }
        return result;
    }

    /** @brief Returns the result number of an exact expression in a plan target list. */
    [[nodiscard]] AttrNumber find_plan_tlist_expression_resno(List* targetlist,
                                                              const void* expression) noexcept {
        for (TargetEntry* entry : postgres_list<TargetEntry>(targetlist)) {
            if (entry != nullptr && equal(entry->expr, expression)) {
                return entry->resno;
            }
        }
        return InvalidAttrNumber;
    }

    /** @brief Records the first output dependency absent from a scan target list. */
    struct batch_join_setrefs_coverage {
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
    struct batch_join_setrefs_coverage_visitor {
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
    void validate_batch_join_setrefs_coverage(List* output_tlist, List* scan_tlist) {
        const bioseq_stub_oids& oids = get_cached_stub_oids();
        batch_join_setrefs_coverage coverage{scan_tlist, &oids, nullptr, nullptr};
        batch_join_setrefs_coverage_visitor visitor;
        for (TargetEntry* output : postgres_list<TargetEntry>(output_tlist)) {
            if (output == nullptr || output->expr == nullptr) {
                continue;
            }
            walk_pg_expression_including_root(
                reinterpret_cast<Node*>(output->expr), coverage, visitor);
            if (coverage.missing_helper_identity != nullptr) {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin helper identity is absent from scan tuple"),
                     errdetail(
                         "node_type=%d scan_columns=%d; executing the SQL helper stub is forbidden",
                         static_cast<int>(nodeTag(coverage.missing_helper_identity)),
                         list_length(scan_tlist))));
            }
            if (coverage.missing_var != nullptr) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqBatchJoin output variable is absent from scan tuple"),
                         errdetail("varno=%u attno=%d type=%u typmod=%d scan_columns=%d",
                                   coverage.missing_var->varno,
                                   coverage.missing_var->varattno,
                                   coverage.missing_var->vartype,
                                   coverage.missing_var->vartypmod,
                                   list_length(scan_tlist))));
            }
        }
    }

    /**
     * @brief Resolves the authoritative PHV id for a Plan-only helper carrier.
     *
     * The same helper is commonly represented by a positive-id PHV in the
     * processed target and by its stripped FuncExpr in a non-projecting locator
     * child.  Preserve the authoritative PHV id in the site codec when present;
     * finalized binding then accepts only that exact PHV-to-bare transition.
     */
    [[nodiscard]] bool batch_join_plan_only_helper_phid(List* authoritative_identities,
                                                        Node* carrier_identity,
                                                        FuncExpr* carrier_helper,
                                                        const bioseq_stub_oids& oids,
                                                        Index& phid) noexcept {
        if (carrier_identity == nullptr || carrier_helper == nullptr)
            return false;
        bool matched = false;
        Index positive_phid = 0;
        for (Node* identity : postgres_list<Node>(authoritative_identities)) {
            FuncExpr* helper = per_row_helper_identity_function(identity, oids);
            if (helper == nullptr || !equal(helper, carrier_helper))
                continue;
            matched = true;
            PlaceHolderVar* placeholder = try_cast_pg_node<PlaceHolderVar>(identity);
            if (placeholder == nullptr)
                continue;
            if (placeholder->phid == 0 ||
                (positive_phid != 0 && positive_phid != placeholder->phid)) {
                return false;
            }
            positive_phid = placeholder->phid;
        }
        PlaceHolderVar* carrier = try_cast_pg_node<PlaceHolderVar>(carrier_identity);
        if (positive_phid != 0) {
            phid = positive_phid;
            return true;
        }
        if (carrier != nullptr) {
            if (carrier->phid == 0)
                return false;
            phid = carrier->phid;
            return true;
        }
        phid = 0;
        return matched || equal(carrier_identity, carrier_helper);
    }

} // export custom-scan partition declarations

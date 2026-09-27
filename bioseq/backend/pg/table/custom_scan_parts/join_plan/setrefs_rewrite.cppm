/**
 * @file
 * @brief Prepares and checks join output references before PostgreSQL runs setrefs.
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
import :custom_scan_join_ownership_descendant_path_contracts;
import :custom_scan_join_ownership_helper_presence_contracts;
import :custom_scan_join_plan_placeholder_contracts;
export import :custom_scan_join_plan_setrefs_contracts;

using namespace bioseq::postgres::support;

export {
    /** @brief Finds the last matching expression within the requested scan  output columns. */
    [[nodiscard]] TargetEntry*
    batch_join_find_exact_scan_entry(List* scan_tlist,
                                     const Node* expression,
                                     const AttrNumber minimum_resno,
                                     const AttrNumber maximum_resno) noexcept {
        if (expression == nullptr)
            return nullptr;
        /* New helper sites follow inherited child columns. Search backwards
         * within the requested range so this owner can select its newly
         * registered result when an equal expression exists below it */
        for (int index = list_length(scan_tlist) - 1; index >= 0; --index) {
            TargetEntry* entry = reinterpret_cast<TargetEntry*>(list_nth(scan_tlist, index));
            if (entry != nullptr && entry->resno >= minimum_resno &&
                entry->resno <= maximum_resno && equal(entry->expr, expression))
                return entry;
        }
        return nullptr;
    }

    /** @brief Finds a single matching helper in the requested output columns, allowing a PlaceHolderVar wrapper. */
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

    /** @brief Finds the same Var by relation, column, type, query level, and NULL-producing joins. */
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

    /** @brief Reports the missing input column or match helper when the output cannot bind to the scan tuple. */
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
     * @brief Finds the PlaceHolderVar id for a helper carried as a hidden plan output.
     *
     * The processed output may wrap a  helper in a PlaceHolderVar while the
     * locator child carries its bare FuncExpr. Save the original phid in the
     * helper record so final binding can recognize that change. Reject
     * conflicting positive ids.
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

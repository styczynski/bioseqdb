/**
 * @file
 * @brief Removes dummy helper calls whose results will be supplied by an ancestor join
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

export module bioseq.postgres.table:custom_scan_match_analysis_descendant_neutralization;

import std;
import bioseq.postgres.support;

import :custom_scan_oid_expression_support_oid_catalog_contracts;
import :custom_scan_planner_method_contracts;
import :custom_scan_match_finalization_contracts;
import :custom_scan_join_ownership_helper_contracts;
import :custom_scan_match_analysis_carrier_validation_contracts;
import :custom_scan_match_analysis_semantic_analysis_contracts;
export import :custom_scan_match_analysis_descendant_neutralization_contracts;

using namespace bioseq::postgres::support;

export {
    /**
     * @brief Replaces dummy helper calls with typed NULLs when an ancestor supplies their results.
     *
     * Leave helpers owned by this  base scan in place. An inherited helper must be a
     * whole target-list entry witm exactly one ancestor owner. Reject executable
     * helper calls in filters and preserve the output column's metadata when
     * replacing its expression.
     */
    void neutralize_bioseq_scan_ancestor_helper_carriers(CustomScan* scan,
                                                         List* ancestor_stack,
                                                         const bioseq_stub_oids& oids) {
        if (scan == nullptr || scan->methods != bioseq_column_scan_scan_methods() ||
            ancestor_stack == NIL)
            return;
        if (batch_join_expression_has_match_helper(reinterpret_cast<Node*>(scan->scan.plan.qual),
                                                   oids)) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqScan inherited match helper remains executable after setrefs"),
                     errdetail("store=Plan.qual plan_node_id=%d", scan->scan.plan.plan_node_id)));
        }
        if (batch_join_expression_has_match_helper(reinterpret_cast<Node*>(scan->custom_exprs),
                                                   oids)) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqScan inherited match helper remains executable after setrefs"),
                     errdetail("store=CustomScan.custom_exprs plan_node_id=%d",
                               scan->scan.plan.plan_node_id)));
        }

        for (TargetEntry* entry : postgres_list<TargetEntry>(scan->scan.plan.targetlist)) {
            Node* identity = entry == nullptr ? nullptr : reinterpret_cast<Node*>(entry->expr);
            if (!batch_join_expression_has_match_helper(identity, oids))
                continue;
            PlaceHolderVar* placeholder = try_cast_pg_node<PlaceHolderVar>(identity);
            FuncExpr* helper = placeholder == nullptr
                                   ? batch_join_well_formed_match_helper(identity, oids)
                                   : batch_join_match_helper_phv_function(placeholder, oids);
            if (entry == nullptr || helper == nullptr ||
                is_projection_capable_plan(reinterpret_cast<Plan*>(scan))) {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg(
                         "BioseqScan inherited match helper carrier is not a whole dummy target"),
                     errdetail("plan_node_id=%d resno=%d",
                               scan->scan.plan.plan_node_id,
                               entry == nullptr ? 0 : entry->resno)));
            }

            const std::size_t local_base_matches =
                finalized_base_owner_matching_site_count(scan, identity, oids);
            std::size_t ancestor_matches = 0u;
            finalized_match_helper_carrier_audit carrier_audit{};
            for (match_owner_finalization* ancestor :
                 postgres_list<match_owner_finalization>(ancestor_stack)) {
                if (ancestor == nullptr || ancestor->scan == nullptr) {
                    ereport(ERROR,
                            (errcode(ERRCODE_INTERNAL_ERROR),
                             errmsg("Bioseq finalized ancestor stack is invalid")));
                }
                ancestor_matches = saturated_match_owner_site_count_add(
                    ancestor_matches,
                    finalized_batch_owner_matching_site_count(
                        ancestor->scan, identity, oids, &carrier_audit));
            }
            if (local_base_matches > 1u || ancestor_matches > 1u ||
                (local_base_matches == 1u && ancestor_matches != 0u)) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqScan match helper carrier ownership is ambiguous"),
                         errdetail(
                             "plan_node_id=%d resno=%d local_base_matches=%zu ancestor_matches=%zu",
                             scan->scan.plan.plan_node_id,
                             entry->resno,
                             local_base_matches,
                             ancestor_matches)));
            }
            /* This base scan fills its own match helpers. A helper supplied only
             * by an ancestor join occupies a dummy output column here. */
            if (local_base_matches == 1u)
                continue;
            if (!dummy_match_helper_is_unique_ancestor_carrier(local_base_matches,
                                                               ancestor_matches)) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqScan inherited match helper carrier has no unique finalized "
                                "ancestor"),
                         errdetail(
                             "plan_node_id=%d resno=%d local_base_matches=%zu ancestor_matches=%zu "
                             "inspected=%s early_reject=%d version=%d private_fields=%d "
                             "expected_fields=%d descriptor_tag=%d sites_tag=%d site_count=%d "
                             "supplied_tag=%d carrier_tag=%d supplied_arg_tag=%d "
                             "carrier_arg_tag=%d canonical_arg_tag=%d site_resno=%d site_phid=%d "
                             "supplied_phid=%d carrier_phid=%d identity_equal=%s "
                             "supplied_arg_canonical=%s carrier_arg_canonical=%s equivalent=%s",
                             scan->scan.plan.plan_node_id,
                             entry->resno,
                             local_base_matches,
                             ancestor_matches,
                             carrier_audit.inspected ? "true" : "false",
                             carrier_audit.early_reject,
                             carrier_audit.version,
                             carrier_audit.private_fields,
                             carrier_audit.expected_fields,
                             carrier_audit.descriptor_tag,
                             carrier_audit.sites_tag,
                             carrier_audit.site_count,
                             carrier_audit.supplied_tag,
                             carrier_audit.carrier_tag,
                             carrier_audit.supplied_argument_tag,
                             carrier_audit.carrier_argument_tag,
                             carrier_audit.canonical_argument_tag,
                             carrier_audit.site_resno,
                             carrier_audit.site_phid,
                             carrier_audit.supplied_phid,
                             carrier_audit.carrier_phid,
                             carrier_audit.identity_equal ? "true" : "false",
                             carrier_audit.supplied_argument_canonical ? "true" : "false",
                             carrier_audit.carrier_argument_canonical ? "true" : "false",
                             carrier_audit.equivalent ? "true" : "false")));
            }

            const Oid type_oid = exprType(identity);
            const int32 typmod = exprTypmod(identity);
            const Oid collation = exprCollation(identity);
            if (!OidIsValid(type_oid) || type_oid != helper->funcresulttype ||
                type_oid != get_func_rettype(helper->funcid)) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqScan inherited match helper carrier type is invalid"),
                         errdetail("plan_node_id=%d resno=%d type_oid=%u helper_type_oid=%u",
                                   scan->scan.plan.plan_node_id,
                                   entry->resno,
                                   type_oid,
                                   helper->funcresulttype)));
            }

            const AttrNumber original_resno = entry->resno;
            char* const original_resname = entry->resname;
            const Index original_sortgroupref = entry->ressortgroupref;
            const Oid original_table_oid = entry->resorigtbl;
            const AttrNumber original_column = entry->resorigcol;
            const bool original_resjunk = entry->resjunk;
            Const* replacement = makeNullConst(type_oid, typmod, collation);
            entry->expr = reinterpret_cast<Expr*>(replacement);
            if (replacement == nullptr || !replacement->constisnull ||
                exprType(reinterpret_cast<Node*>(entry->expr)) != type_oid ||
                exprTypmod(reinterpret_cast<Node*>(entry->expr)) != typmod ||
                exprCollation(reinterpret_cast<Node*>(entry->expr)) != collation ||
                entry->resno != original_resno || entry->resname != original_resname ||
                entry->ressortgroupref != original_sortgroupref ||
                entry->resorigtbl != original_table_oid || entry->resorigcol != original_column ||
                entry->resjunk != original_resjunk) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqScan inherited match helper carrier neutralization changed "
                                "its slot contract"),
                         errdetail("plan_node_id=%d resno=%d",
                                   scan->scan.plan.plan_node_id,
                                   original_resno)));
            }
        }
    }

    /** @brief Walks the plan and tracks ancestor joins that can supply dummy helper results. */
    void
    neutralize_ancestor_owned_dummy_match_helpers_plan(Plan* plan,
                                                       std::span<match_owner_finalization> owners,
                                                       List* ancestor_stack,
                                                       const bioseq_stub_oids& oids) {
        if (plan == nullptr)
            return;
        check_stack_depth();
        if (IsA(plan, SubqueryScan)) {
            /* Helpers inside a subquery belong to that query. Start  a fresh
             * ancestor list before visiting its plan. */
            neutralize_ancestor_owned_dummy_match_helpers_plan(
                reinterpret_cast<SubqueryScan*>(plan)->subplan, owners, NIL, oids);
            return;
        }
        if (IsA(plan, CustomScan)) {
            auto* custom = reinterpret_cast<CustomScan*>(plan);
            if (custom->methods == bioseq_column_scan_scan_methods()) {
                neutralize_bioseq_scan_ancestor_helper_carriers(custom, ancestor_stack, oids);
            }
            match_owner_finalization* owner =
                finalized_batch_owner_for_ancestor_stack(custom, owners);
            match_owner_finalization* parent_owner =
                ancestor_stack == NIL
                    ? nullptr
                    : reinterpret_cast<match_owner_finalization*>(llast(ancestor_stack));
            if (owner != nullptr && parent_owner != nullptr &&
                parent_owner->query_scope_id != owner->query_scope_id) {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("Bioseq finalized ancestor stack crosses query scopes"),
                     errdetail("parent_scope_id=" UINT64_FORMAT " child_scope_id=" UINT64_FORMAT,
                               static_cast<uint64>(parent_owner->query_scope_id),
                               static_cast<uint64>(owner->query_scope_id))));
            }
            List* child_ancestors =
                owner == nullptr ? ancestor_stack : lappend(list_copy(ancestor_stack), owner);
            for (Plan* child : postgres_list<Plan>(custom->custom_plans)) {
                neutralize_ancestor_owned_dummy_match_helpers_plan(
                    child, owners, child_ancestors, oids);
            }
            if (owner != nullptr)
                list_free(child_ancestors);
            return;
        }
        // Visit each child with the same ancestor match owners.
        auto visit_plan_list = [owners, ancestor_stack, &oids](List* plans) {
            for (Plan* child : postgres_list<Plan>(plans)) {
                neutralize_ancestor_owned_dummy_match_helpers_plan(
                    child, owners, ancestor_stack, oids);
            }
        };
        if (IsA(plan, Append)) {
            visit_plan_list(reinterpret_cast<Append*>(plan)->appendplans);
            return;
        }
        if (IsA(plan, MergeAppend)) {
            visit_plan_list(reinterpret_cast<MergeAppend*>(plan)->mergeplans);
            return;
        }
        if (IsA(plan, BitmapAnd)) {
            visit_plan_list(reinterpret_cast<BitmapAnd*>(plan)->bitmapplans);
            return;
        }
        if (IsA(plan, BitmapOr)) {
            visit_plan_list(reinterpret_cast<BitmapOr*>(plan)->bitmapplans);
            return;
        }
        neutralize_ancestor_owned_dummy_match_helpers_plan(
            outerPlan(plan), owners, ancestor_stack, oids);
        if (innerPlan(plan) != outerPlan(plan)) {
            neutralize_ancestor_owned_dummy_match_helpers_plan(
                innerPlan(plan), owners, ancestor_stack, oids);
        }
    }

    /** @brief Replaces ancestor-supplied dummy helpers throughout the statement and its subplans. */
    void neutralize_ancestor_owned_dummy_match_helpers(PlannedStmt* statement,
                                                       std::span<match_owner_finalization> owners) {
        if (statement == nullptr || statement->planTree == nullptr)
            return;
        const bioseq_stub_oids& oids = get_cached_stub_oids();
        neutralize_ancestor_owned_dummy_match_helpers_plan(statement->planTree, owners, NIL, oids);
        for (Plan* subplan : postgres_list<Plan>(statement->subplans)) {
            neutralize_ancestor_owned_dummy_match_helpers_plan(subplan, owners, NIL, oids);
        }
    }

    /**
     * @brief Checks that each dummy helper column has exactly one source for its result.
     *
     * The helper must occupy a whole target-list entry in a plan that does not
     * project expressions. Count matching owners in its children and the  current
     * parent join. A subquery has its own, helper bindings.
     */
    [[nodiscard]] bool dummy_targetlist_has_only_unique_finalized_owner_helpers(
        Plan* plan, const bioseq_stub_oids& oids, CustomScan* current_parent) {
        if (plan == nullptr || is_projection_capable_plan(plan))
            return false;
        for (TargetEntry* entry : postgres_list<TargetEntry>(plan->targetlist)) {
            Node* identity = entry == nullptr ? nullptr : reinterpret_cast<Node*>(entry->expr);
            if (!batch_join_expression_has_match_helper(identity, oids))
                continue;
            PlaceHolderVar* placeholder = try_cast_pg_node<PlaceHolderVar>(identity);
            FuncExpr* helper = placeholder == nullptr
                                   ? batch_join_well_formed_match_helper(identity, oids)
                                   : batch_join_match_helper_phv_function(placeholder, oids);
            if (helper == nullptr)
                return false;
            const std::size_t current_parent_matches =
                current_parent == nullptr || IsA(plan, SubqueryScan)
                    ? 0u
                    : finalized_batch_owner_matching_site_count(current_parent, identity, oids);
            std::size_t descendant_matches = 0u;
            // Count child owners only up to 2, since multiple owners are ambiguous
            auto count_child = [&descendant_matches, identity, &oids](Plan* child) {
                const std::size_t additional =
                    finalized_descendant_match_owner_site_count(child, identity, oids);
                descendant_matches =
                    saturated_match_owner_site_count_add(descendant_matches, additional);
            };
            if (IsA(plan, Append)) {
                for (Plan* child :
                     postgres_list<Plan>(reinterpret_cast<Append*>(plan)->appendplans)) {
                    count_child(child);
                }
            } else if (IsA(plan, MergeAppend)) {
                for (Plan* child :
                     postgres_list<Plan>(reinterpret_cast<MergeAppend*>(plan)->mergeplans)) {
                    count_child(child);
                }
            } else if (IsA(plan, CustomScan)) {
                for (Plan* child :
                     postgres_list<Plan>(reinterpret_cast<CustomScan*>(plan)->custom_plans)) {
                    count_child(child);
                }
            } else if (IsA(plan, BitmapAnd)) {
                for (Plan* child :
                     postgres_list<Plan>(reinterpret_cast<BitmapAnd*>(plan)->bitmapplans)) {
                    count_child(child);
                }
            } else if (IsA(plan, BitmapOr)) {
                for (Plan* child :
                     postgres_list<Plan>(reinterpret_cast<BitmapOr*>(plan)->bitmapplans)) {
                    count_child(child);
                }
            } else if (IsA(plan, SubqueryScan)) {
                /* Do not derive a parent helper from a separate query scope. */
            } else {
                count_child(outerPlan(plan));
                if (innerPlan(plan) != outerPlan(plan))
                    count_child(innerPlan(plan));
            }
            if (!dummy_match_helper_has_unique_finalized_owner(descendant_matches,
                                                               current_parent_matches))
                return false;
        }
        return true;
    }

} // export custom-scan partition declarations

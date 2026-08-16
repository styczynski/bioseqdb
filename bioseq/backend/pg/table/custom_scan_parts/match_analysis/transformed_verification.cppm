/**
 * @file
 * @brief Verifies transformed helpers, reparameterization, and method tables.
 */
module;
// Anchor Highway in this global module fragment so target-specific definitions
// imported through multiple module graphs share one translation-unit context.
#include <bioseq/pg_compat.h>
#include <hwy/highway.h>

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

export module bioseq.postgres.table:custom_scan_match_analysis_transformed_verification;

import std;
import bioseq.postgres.table.io;
import :access_method;
import :access_common;
import :utils;
import bioseq.postgres.column.table_access;
import bioseq.postgres.configuration.aligner;
import bioseq.postgres.configuration.core;
import bioseq.postgres.vectorization.types;
import bioseq.postgres.vectorization.execution;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.highway.dispatch_contract;
import bioseq.postgres.external.path_builder_slot;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.table.types;
import bioseq.postgres.table.scan_state;
import bioseq.postgres.table.sql_row_codecs;
import bioseq.postgres.table.sql_rows;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.scheduler.dispatch_buffer.api;
import bioseq.postgres.scheduler.supervisor;
import bioseq.postgres.scheduler.supervisor.vulkan;
import bioseq.parallelism.mode;
import bioseq.parallelism.memory_admission;
import bioseq.gpu.vulkan.runtime;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.pipeline.stats;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.sequences.cigar;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.storage.manager;
import bioseq.storage.layout;
import bioseq.storage.size;
import bioseq.storage.coordinates;
import bioseq.storage.lazy_sequence;
import bioseq.postgres.storage.scanner;
import bioseq.indexing.type;
import bioseq.indexing.symbol_counts;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.admin;
import bioseq.instrumentation.summary_writer;
import bioseq.common.seqan_adapter;
import bioseq.postgres.table.batch_sort_scan;

import :custom_scan_support;
import :custom_scan_slice_projection;
import :custom_scan_join_paths;
import :custom_scan_scan_paths;
import :custom_scan_plan_builders;

import :custom_scan_match_finalization;

import :custom_scan_match_analysis_carrier_validation;
import :custom_scan_match_analysis_semantic_analysis;
import :custom_scan_match_analysis_descendant_neutralization;

using namespace bioseq::postgres::support;

export {
    /** @brief Records the first unowned-helper audit failure with plan-boundary context. */
    void
    record_batch_join_plan_helper_audit_failure(batch_join_plan_helper_audit_first_failure& failure,
                                                Plan* plan,
                                                CustomScan* current_parent,
                                                const bioseq_stub_oids& oids,
                                                const char* reason,
                                                const char* store,
                                                const char* boundary_encountered,
                                                bool inspect_targetlist) {
        if (failure.recorded)
            return;
        failure.recorded = true;
        failure.reason = reason;
        failure.store = store;
        failure.boundary_encountered = boundary_encountered;
        failure.plan_node_tag = plan == nullptr ? -1 : static_cast<int>(nodeTag(plan));
        failure.projection_capable = plan != nullptr && is_projection_capable_plan(plan);
        if (plan != nullptr && IsA(plan, CustomScan)) {
            auto* custom = reinterpret_cast<CustomScan*>(plan);
            if (custom->methods != nullptr && custom->methods->CustomName != nullptr) {
                failure.custom_name = custom->methods->CustomName;
            }
        }
        if (!inspect_targetlist || plan == nullptr)
            return;

        TargetEntry* first_helper_entry = nullptr;
        bool first_helper_whole = false;
        finalized_match_owner_site_counts first_helper_counts{};
        std::size_t first_helper_parent_matches = 0u;
        for (TargetEntry* entry : postgres_list<TargetEntry>(plan->targetlist)) {
            Node* identity = entry == nullptr ? nullptr : reinterpret_cast<Node*>(entry->expr);
            if (!batch_join_expression_has_match_helper(identity, oids))
                continue;
            PlaceHolderVar* placeholder = try_cast_pg_node<PlaceHolderVar>(identity);
            FuncExpr* helper = placeholder == nullptr
                                   ? batch_join_well_formed_match_helper(identity, oids)
                                   : batch_join_match_helper_phv_function(placeholder, oids);
            const bool whole_helper = helper != nullptr;
            const finalized_match_owner_site_counts counts =
                finalized_child_match_owner_site_counts(plan, identity, oids);
            const std::size_t parent_matches =
                current_parent == nullptr
                    ? 0u
                    : finalized_batch_owner_matching_site_count(current_parent, identity, oids);
            if (first_helper_entry == nullptr) {
                first_helper_entry = entry;
                first_helper_whole = whole_helper;
                first_helper_counts = counts;
                first_helper_parent_matches = parent_matches;
            }
            const std::size_t descendant_matches =
                saturated_match_owner_site_count_add(counts.batch, counts.base);
            if (failure.projection_capable || !whole_helper ||
                !dummy_match_helper_has_unique_finalized_owner(descendant_matches,
                                                               parent_matches)) {
                first_helper_entry = entry;
                first_helper_whole = whole_helper;
                first_helper_counts = counts;
                first_helper_parent_matches = parent_matches;
                break;
            }
        }
        if (first_helper_entry == nullptr)
            return;
        failure.first_helper_resno = first_helper_entry->resno;
        failure.whole_helper_valid = first_helper_whole;
        failure.finalized_batch_descendant_matches = first_helper_counts.batch;
        failure.finalized_base_descendant_matches = first_helper_counts.base;
        failure.current_parent_matches = first_helper_parent_matches;
    }

    /**
     * @brief Detects match helpers that remain executable without a finalized owner.
     *
     * Final setrefs must replace every propagated helper identity in an
     * executable Plan expression with the descendant owner's scan-slot Var.
     * A non-projecting node may retain the whole identity in its dummy targetlist,
     * but only when exactly one finalized descendant or current PerRow owner site
     * supplies that column.  Mixed parent/descendant ownership remains ambiguous.
     * Descriptor-only custom_scan_tlist/custom_private fields remain excluded.
     */
    [[nodiscard]] bool batch_join_plan_has_unowned_executable_match_helper(
        Plan* plan,
        const bioseq_stub_oids& oids,
        CustomScan* current_parent,
        batch_join_plan_helper_audit_first_failure& first_failure,
        const char* boundary_encountered) {
        if (plan == nullptr)
            return false;
        check_stack_depth();
        const bool targetlist_has_helper =
            batch_join_expression_has_match_helper(reinterpret_cast<Node*>(plan->targetlist), oids);
        if (targetlist_has_helper &&
            (!IsA(plan, CustomScan) || !base_scan_targetlist_has_only_finalized_match_helpers(
                                           reinterpret_cast<CustomScan*>(plan), oids)) &&
            !dummy_targetlist_has_only_unique_finalized_owner_helpers(plan, oids, current_parent)) {
            record_batch_join_plan_helper_audit_failure(
                first_failure,
                plan,
                current_parent,
                oids,
                is_projection_capable_plan(plan) ? "projection-capable targetlist"
                                                 : "dummy targetlist ownership validation failed",
                "Plan.targetlist",
                boundary_encountered,
                true);
            return true;
        }
        if (batch_join_expression_has_match_helper(reinterpret_cast<Node*>(plan->qual), oids)) {
            record_batch_join_plan_helper_audit_failure(first_failure,
                                                        plan,
                                                        current_parent,
                                                        oids,
                                                        "executable expression",
                                                        "Plan.qual",
                                                        boundary_encountered,
                                                        false);
            return true;
        }
        if ((IsA(plan, NestLoop) || IsA(plan, MergeJoin) || IsA(plan, HashJoin)) &&
            batch_join_expression_has_match_helper(
                reinterpret_cast<Node*>(reinterpret_cast<Join*>(plan)->joinqual), oids)) {
            record_batch_join_plan_helper_audit_failure(first_failure,
                                                        plan,
                                                        current_parent,
                                                        oids,
                                                        "executable expression",
                                                        "Join.joinqual",
                                                        boundary_encountered,
                                                        false);
            return true;
        }
        if (IsA(plan, MergeJoin) &&
            batch_join_expression_has_match_helper(
                reinterpret_cast<Node*>(reinterpret_cast<MergeJoin*>(plan)->mergeclauses), oids)) {
            record_batch_join_plan_helper_audit_failure(first_failure,
                                                        plan,
                                                        current_parent,
                                                        oids,
                                                        "executable expression",
                                                        "MergeJoin.mergeclauses",
                                                        boundary_encountered,
                                                        false);
            return true;
        }
        if (IsA(plan, HashJoin) &&
            batch_join_expression_has_match_helper(
                reinterpret_cast<Node*>(reinterpret_cast<HashJoin*>(plan)->hashclauses), oids)) {
            record_batch_join_plan_helper_audit_failure(first_failure,
                                                        plan,
                                                        current_parent,
                                                        oids,
                                                        "executable expression",
                                                        "HashJoin.hashclauses",
                                                        boundary_encountered,
                                                        false);
            return true;
        }
        if (IsA(plan, Result) &&
            batch_join_expression_has_match_helper(
                reinterpret_cast<Node*>(reinterpret_cast<Result*>(plan)->resconstantqual), oids)) {
            record_batch_join_plan_helper_audit_failure(first_failure,
                                                        plan,
                                                        current_parent,
                                                        oids,
                                                        "executable expression",
                                                        "Result.resconstantqual",
                                                        boundary_encountered,
                                                        false);
            return true;
        }
        if (IsA(plan, CustomScan)) {
            auto* custom = reinterpret_cast<CustomScan*>(plan);
            if (batch_join_expression_has_match_helper(
                    reinterpret_cast<Node*>(custom->custom_exprs), oids)) {
                record_batch_join_plan_helper_audit_failure(first_failure,
                                                            plan,
                                                            current_parent,
                                                            oids,
                                                            "executable expression",
                                                            "CustomScan.custom_exprs",
                                                            boundary_encountered,
                                                            false);
                return true;
            }
            for (Plan* child : postgres_list<Plan>(custom->custom_plans)) {
                if (batch_join_plan_has_unowned_executable_match_helper(
                        child, oids, current_parent, first_failure, boundary_encountered)) {
                    return true;
                }
            }
            return false;
        }
        // Recursively audits one PostgreSQL plan-child list.
        auto search_plan_list =
            [&oids, current_parent, &first_failure, boundary_encountered](List* plans) {
                for (Plan* child : postgres_list<Plan>(plans)) {
                    if (batch_join_plan_has_unowned_executable_match_helper(
                            child, oids, current_parent, first_failure, boundary_encountered)) {
                        return true;
                    }
                }
                return false;
            };
        if (IsA(plan, Append)) {
            return search_plan_list(reinterpret_cast<Append*>(plan)->appendplans);
        }
        if (IsA(plan, MergeAppend)) {
            return search_plan_list(reinterpret_cast<MergeAppend*>(plan)->mergeplans);
        }
        if (IsA(plan, BitmapAnd)) {
            return search_plan_list(reinterpret_cast<BitmapAnd*>(plan)->bitmapplans);
        }
        if (IsA(plan, BitmapOr)) {
            return search_plan_list(reinterpret_cast<BitmapOr*>(plan)->bitmapplans);
        }
        if (IsA(plan, SubqueryScan)) {
            return batch_join_plan_has_unowned_executable_match_helper(
                reinterpret_cast<SubqueryScan*>(plan)->subplan,
                oids,
                nullptr,
                first_failure,
                "SubqueryScan");
        }
        return batch_join_plan_has_unowned_executable_match_helper(
                   outerPlan(plan), oids, current_parent, first_failure, boundary_encountered) ||
               (innerPlan(plan) != outerPlan(plan) &&
                batch_join_plan_has_unowned_executable_match_helper(
                    innerPlan(plan), oids, current_parent, first_failure, boundary_encountered));
    }

    /**
     * @brief Rewrites parent Vars and copied restrictions for a partition child.
     *
     * Changes all varnos referencing the topmost parent
     * of child_rel to instead reference child_rel directly. Both the Var and the
     * RestrictInfo are deep copied before being written (copy-before-write), and
     * a RestrictInfo's clause is recursively rewritten via the cursor.
     */
    struct reparameterize_mutator {
        /** @brief Copy-on-write rewrites one Var or RestrictInfo node. */
        [[nodiscard]] pg_node_mutation operator()(pg_expression_mutation_cursor cursor,
                                                  pg_node_view node,
                                                  RelOptInfo& child_rel) const {
            if (Var* var = node.try_cast<Var>()) {
                if (bioseq_is_concrete_rte_varno(var->varno) &&
                    bms_is_member(static_cast<int>(var->varno), child_rel.top_parent_relids)) {
                    var = copy_pg_node(var);
                    var->varno = child_rel.relid;
                }
                return pg_node_mutation::replace_with(to_pg_node(var));
            }

            if (RestrictInfo* rinfo = node.try_cast<RestrictInfo>()) {
                rinfo = copy_pg_node(rinfo);
                rinfo->clause =
                    reinterpret_cast<Expr*>(cursor.descend(reinterpret_cast<Node*>(rinfo->clause)));
                return pg_node_mutation::replace_with(to_pg_node(rinfo));
            }

            return pg_node_mutation::descend();
        }
    };

    /**
     * @brief Rewrites base-scan private expressions for direct child parameterization.
     *
     * This method is called when a
     * path is reparameterized directly to a child relation, rather than the
     * top-level parent.
     *
     * For instance, let there be a join of two partitioned columnar relations PX
     * and PY. A path for a BioseqColumnScan of PY3 might be parameterized by PX so
     * that the join qual "PY3.a = PX.a" (referencing the parent PX) can be pushed
     * down. But if the planner decides on a partition-wise join, then the path
     * will be reparameterized on the child table PX3 directly.
     *
     * When that happens, we need to update all Vars in the pushed-down quals to
     * reference PX3, not PX, to match the child parameterization. This method
     * notifies us that it needs to be done, and allows us to update the
     * information in custom_private.
     */
    List* bioseq_column_scan_path_reparameterize_custom_path_by_child(PlannerInfo* root,
                                                                      List* custom_private,
                                                                      RelOptInfo* child_rel) {
        reparameterize_mutator mutator;
        return reinterpret_cast<List*>(
            mutate_pg_expression(reinterpret_cast<Node*>(custom_private), *child_rel, mutator));
    }

    /** @brief Rewrites the complete per-row path ABI for a partition child relation. */
    List* bioseq_per_row_join_path_reparameterize_custom_path_by_child(PlannerInfo* root,
                                                                       List* custom_private,
                                                                       RelOptInfo* child_rel) {
        if (root == nullptr || root->parse == nullptr || child_rel == nullptr ||
            child_rel->top_parent == nullptr ||
            list_length(custom_private) != k_bioseq_per_row_join_path_private_fields) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqPerRowMmsearch child reparameterization state is invalid")));
        }
        for (const int index : {0, 1, 2, 3, 7, 8, 9}) {
            Node* value = reinterpret_cast<Node*>(list_nth(custom_private, index));
            if (value == nullptr || !IsA(value, Integer)) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqPerRowMmsearch child scalar ABI is invalid"),
                         errdetail("field=%d", index)));
            }
        }
        for (const int index : {4, 5, 6, 10, 11, 12}) {
            if (list_nth(custom_private, index) == nullptr) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqPerRowMmsearch child expression ABI is invalid"),
                         errdetail("field=%d", index)));
            }
        }

        const Index target_varno = static_cast<Index>(intVal(list_nth(custom_private, 2)));
        const AttrNumber target_storage_attno =
            static_cast<AttrNumber>(intVal(list_nth(custom_private, 3)));
        const Oid target_relation_oid = static_cast<Oid>(intVal(list_nth(custom_private, 1)));
        /* Use PostgreSQL's multilevel appendrel translator, not a varno-only
         * rewrite: attached partitions may have a distinct physical attno map. */
        List* rewritten = reinterpret_cast<List*>(adjust_appendrel_attrs_multilevel(
            root, reinterpret_cast<Node*>(custom_private), child_rel, child_rel->top_parent));
        if (!bioseq_is_concrete_rte_varno(target_varno)) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqPerRowMmsearch target child varno is invalid")));
        }
        if (bms_is_member(static_cast<int>(target_varno), child_rel->top_parent_relids)) {
            if (!bioseq_is_concrete_rte_varno(child_rel->relid) ||
                child_rel->relid > static_cast<Index>(list_length(root->parse->rtable))) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqPerRowMmsearch target child RTE is unavailable")));
            }
            RangeTblEntry* child_rte = rt_fetch(child_rel->relid, root->parse->rtable);
            if (child_rte == nullptr || child_rte->rtekind != RTE_RELATION ||
                !OidIsValid(child_rte->relid) ||
                !relation_oid_uses_column_table_access_method(child_rte->relid)) {
                ereport(ERROR,
                        (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                         errmsg("BioseqPerRowMmsearch target partition is not a Bioseq relation")));
            }
            Oid target_type = InvalidOid;
            int32 target_typmod = -1;
            Oid target_collation = InvalidOid;
            get_atttypetypmodcoll(target_relation_oid,
                                  target_storage_attno,
                                  &target_type,
                                  &target_typmod,
                                  &target_collation);
            Var* parent_storage_var = makeVar(target_varno,
                                              target_storage_attno,
                                              target_type,
                                              target_typmod,
                                              target_collation,
                                              0);
            Var* child_storage_var = try_cast_pg_node<Var>(
                adjust_appendrel_attrs_multilevel(root,
                                                  reinterpret_cast<Node*>(parent_storage_var),
                                                  child_rel,
                                                  child_rel->top_parent));
            if (child_storage_var == nullptr || child_storage_var->varno != child_rel->relid ||
                child_storage_var->varattno <= 0 ||
                child_storage_var->vartype != get_cached_stub_oids().bioseq_seq_type_oid) {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("BioseqPerRowMmsearch target partition column mapping is invalid")));
            }
            lfirst(list_nth_cell(rewritten, 1)) = makeInteger(static_cast<int>(child_rte->relid));
            lfirst(list_nth_cell(rewritten, 2)) =
                makeInteger(static_cast<int>(child_storage_var->varno));
            lfirst(list_nth_cell(rewritten, 3)) =
                makeInteger(static_cast<int>(child_storage_var->varattno));
        }
        return rewritten;
    }

    /** @brief Returns the stable PostgreSQL path callbacks for base Bioseq scans. */
    [[nodiscard]] const CustomPathMethods* bioseq_column_scan_path_methods() noexcept {
        static const CustomPathMethods methods{
            .CustomName = "BioseqScan",
            .PlanCustomPath = bioseq_column_scan_path_plan_custom_path,
            .ReparameterizeCustomPathByChild =
                bioseq_column_scan_path_reparameterize_custom_path_by_child,
        };
        return &methods;
    }

    /** @brief Returns the stable PostgreSQL path callbacks for per-row joins. */
    [[nodiscard]] const CustomPathMethods* bioseq_per_row_join_path_methods() noexcept {
        static const CustomPathMethods methods{
            .CustomName = "BioseqBatchJoin",
            .PlanCustomPath = bioseq_per_row_join_plan_custom_path,
            .ReparameterizeCustomPathByChild =
                bioseq_per_row_join_path_reparameterize_custom_path_by_child,
        };
        return &methods;
    }

} // export custom-scan partition declarations

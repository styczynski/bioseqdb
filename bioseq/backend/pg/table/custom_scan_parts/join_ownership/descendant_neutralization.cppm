/**
 * @file
 * @brief Removes the owned restriction and transfers helper ownership through descendant paths.
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

export module bioseq.postgres.table:custom_scan_join_ownership_descendant_neutralization;

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

import :custom_scan_join_ownership_carrier_lineage;
import :custom_scan_join_ownership_semantic_admission;

using namespace bioseq::postgres::support;

export {
    [[nodiscard]] List* per_row_without_restriction(List* restrictions, RestrictInfo* owned) {
        List* filtered = NIL;
        for (RestrictInfo* candidate : postgres_list<RestrictInfo>(restrictions)) {
            if (candidate != owned)
                filtered = lappend(filtered, candidate);
        }
        return filtered;
    }

    /* A RestrictInfo can migrate below a NestLoop through PathParamInfo (or sit
     * in another custom/join child) while still appearing in the incumbent's
     * joinrestrictinfo.  Removing only the top copy would leave a second live
     * mmsearch rendezvous.  The post-join owner therefore accepts an incumbent
     * only when the exact pointer is absent from every retained descendant. */
    [[nodiscard]] bool per_row_descendant_owns_restriction(Path const* path,
                                                           RestrictInfo const* owned) noexcept {
        if (path == nullptr || owned == nullptr)
            return false;
        if (path->param_info != nullptr && list_member_ptr(path->param_info->ppi_clauses, owned))
            return true;
        if (IsA(path, CustomPath)) {
            auto const* custom = reinterpret_cast<CustomPath const*>(path);
            if (list_member_ptr(custom->custom_restrictinfo, owned))
                return true;
            for (Path* child : postgres_list<Path>(custom->custom_paths)) {
                if (per_row_descendant_owns_restriction(child, owned))
                    return true;
            }
            return false;
        }
        if (IsA(path, NestPath) || IsA(path, MergePath) || IsA(path, HashPath)) {
            auto const* join = reinterpret_cast<JoinPath const*>(path);
            if (list_member_ptr(join->joinrestrictinfo, owned))
                return true;
            return per_row_descendant_owns_restriction(join->outerjoinpath, owned) ||
                   per_row_descendant_owns_restriction(join->innerjoinpath, owned);
        }
        switch (path->pathtype) {
        case T_Result:
            return per_row_descendant_owns_restriction(
                reinterpret_cast<ProjectionPath const*>(path)->subpath, owned);
        case T_Material:
            return per_row_descendant_owns_restriction(
                reinterpret_cast<MaterialPath const*>(path)->subpath, owned);
        case T_Memoize:
            return per_row_descendant_owns_restriction(
                reinterpret_cast<MemoizePath const*>(path)->subpath, owned);
        case T_Sort:
            return per_row_descendant_owns_restriction(
                reinterpret_cast<SortPath const*>(path)->subpath, owned);
        case T_IncrementalSort:
            return per_row_descendant_owns_restriction(
                reinterpret_cast<IncrementalSortPath const*>(path)->spath.subpath, owned);
        case T_Append: {
            auto const* append = reinterpret_cast<AppendPath const*>(path);
            for (Path* child : postgres_list<Path>(append->subpaths)) {
                if (per_row_descendant_owns_restriction(child, owned))
                    return true;
            }
            return false;
        }
        case T_MergeAppend: {
            auto const* append = reinterpret_cast<MergeAppendPath const*>(path);
            for (Path* child : postgres_list<Path>(append->subpaths)) {
                if (per_row_descendant_owns_restriction(child, owned))
                    return true;
            }
            return false;
        }
        default:
            return false;
        }
    }

    struct per_row_mmsearch_plan_audit {
        std::size_t executable_qual_copies = 0u;
        std::size_t vector_qual_copies = 0u;
        std::size_t storage_qual_copies = 0u;
        std::size_t descendant_owner_copies = 0u;

        [[nodiscard]] bool clean() const noexcept {
            return executable_qual_copies == 0u && vector_qual_copies == 0u &&
                   storage_qual_copies == 0u && descendant_owner_copies == 0u;
        }
    };

    void per_row_audit_mmsearch_expression(Node* expression,
                                           std::size_t& destination,
                                           bioseq_stub_oids const& oids) {
        if (expression == nullptr)
            return;
        const std::size_t additional = count_mmsearch_occurrences(expression, oids);
        destination = additional > std::numeric_limits<std::size_t>::max() - destination
                          ? std::numeric_limits<std::size_t>::max()
                          : destination + additional;
    }

    struct exact_mmsearch_owner_audit {
        FuncExpr* candidate = nullptr;
        bioseq_stub_oids const* oids = nullptr;
        std::size_t matches = 0u;
    };

    struct exact_mmsearch_owner_audit_visitor {
        [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                              pg_node_view node,
                                              exact_mmsearch_owner_audit& audit) const {
            FuncExpr* function = node.try_cast<FuncExpr>();
            if (function == nullptr || audit.oids == nullptr ||
                !is_mmsearch_funcid(function->funcid, *audit.oids)) {
                return pg_walk_step::descend;
            }
            if (audit.candidate != nullptr && equal(function, audit.candidate)) {
                audit.matches = audit.matches == std::numeric_limits<std::size_t>::max()
                                    ? audit.matches
                                    : audit.matches + 1u;
            }
            return pg_walk_step::skip_subtree;
        }
    };

    void per_row_audit_exact_mmsearch_owner(Node* expression,
                                            FuncExpr* candidate,
                                            std::size_t& destination,
                                            bioseq_stub_oids const& oids) {
        if (expression == nullptr || candidate == nullptr)
            return;
        exact_mmsearch_owner_audit audit{candidate, &oids, 0u};
        exact_mmsearch_owner_audit_visitor visitor;
        walk_pg_expression_including_root(expression, audit, visitor);
        destination = audit.matches > std::numeric_limits<std::size_t>::max() - destination
                          ? std::numeric_limits<std::size_t>::max()
                          : destination + audit.matches;
    }

    /* Audit the serialized child plan that will execute below the transformed
     * owner.  `Plan::qual` is only one of PostgreSQL's executable-qual stores:
     * join/index/recheck fields and the Bioseq reader's storage/vector lists must
     * be inspected independently.  The fourth BioseqScan expression list is an
     * executor-only mmsearch owner.  A foreign owner is valid nested SQL state;
     * only an owner equal to the candidate recipe is a duplicate of this owner. */
    void per_row_audit_mmsearch_plan(Plan* plan,
                                     per_row_mmsearch_plan_audit& audit,
                                     bioseq_stub_oids const& oids,
                                     FuncExpr* exact_owner_candidate = nullptr) {
        if (plan == nullptr)
            return;
        per_row_audit_mmsearch_expression(
            reinterpret_cast<Node*>(plan->qual), audit.executable_qual_copies, oids);

        if (IsA(plan, NestLoop)) {
            auto* join = &reinterpret_cast<NestLoop*>(plan)->join;
            per_row_audit_mmsearch_expression(
                reinterpret_cast<Node*>(join->joinqual), audit.executable_qual_copies, oids);
        } else if (IsA(plan, MergeJoin)) {
            auto* merge = reinterpret_cast<MergeJoin*>(plan);
            per_row_audit_mmsearch_expression(
                reinterpret_cast<Node*>(merge->join.joinqual), audit.executable_qual_copies, oids);
            per_row_audit_mmsearch_expression(
                reinterpret_cast<Node*>(merge->mergeclauses), audit.executable_qual_copies, oids);
        } else if (IsA(plan, HashJoin)) {
            auto* hash = reinterpret_cast<HashJoin*>(plan);
            per_row_audit_mmsearch_expression(
                reinterpret_cast<Node*>(hash->join.joinqual), audit.executable_qual_copies, oids);
            per_row_audit_mmsearch_expression(
                reinterpret_cast<Node*>(hash->hashclauses), audit.executable_qual_copies, oids);
        } else if (IsA(plan, Result)) {
            per_row_audit_mmsearch_expression(reinterpret_cast<Result*>(plan)->resconstantqual,
                                              audit.executable_qual_copies,
                                              oids);
        } else if (IsA(plan, IndexScan)) {
            auto* scan = reinterpret_cast<IndexScan*>(plan);
            for (List* expressions : {scan->indexqual,
                                      scan->indexqualorig,
                                      scan->indexorderby,
                                      scan->indexorderbyorig}) {
                per_row_audit_mmsearch_expression(
                    reinterpret_cast<Node*>(expressions), audit.executable_qual_copies, oids);
            }
        } else if (IsA(plan, IndexOnlyScan)) {
            auto* scan = reinterpret_cast<IndexOnlyScan*>(plan);
            for (List* expressions : {scan->indexqual, scan->recheckqual, scan->indexorderby}) {
                per_row_audit_mmsearch_expression(
                    reinterpret_cast<Node*>(expressions), audit.executable_qual_copies, oids);
            }
        } else if (IsA(plan, BitmapIndexScan)) {
            auto* scan = reinterpret_cast<BitmapIndexScan*>(plan);
            for (List* expressions : {scan->indexqual, scan->indexqualorig}) {
                per_row_audit_mmsearch_expression(
                    reinterpret_cast<Node*>(expressions), audit.executable_qual_copies, oids);
            }
        } else if (IsA(plan, BitmapHeapScan)) {
            per_row_audit_mmsearch_expression(
                reinterpret_cast<Node*>(reinterpret_cast<BitmapHeapScan*>(plan)->bitmapqualorig),
                audit.executable_qual_copies,
                oids);
        } else if (IsA(plan, TidScan)) {
            per_row_audit_mmsearch_expression(
                reinterpret_cast<Node*>(reinterpret_cast<TidScan*>(plan)->tidquals),
                audit.executable_qual_copies,
                oids);
        } else if (IsA(plan, TidRangeScan)) {
            per_row_audit_mmsearch_expression(
                reinterpret_cast<Node*>(reinterpret_cast<TidRangeScan*>(plan)->tidrangequals),
                audit.executable_qual_copies,
                oids);
        } else if (IsA(plan, ForeignScan)) {
            auto* scan = reinterpret_cast<ForeignScan*>(plan);
            for (List* expressions : {scan->fdw_exprs, scan->fdw_recheck_quals}) {
                per_row_audit_mmsearch_expression(
                    reinterpret_cast<Node*>(expressions), audit.executable_qual_copies, oids);
            }
        }

        if (IsA(plan, CustomScan)) {
            auto* custom = reinterpret_cast<CustomScan*>(plan);
            if (custom->methods == bioseq_column_scan_scan_methods() &&
                list_length(custom->custom_exprs) >= 3) {
                per_row_audit_mmsearch_expression(
                    reinterpret_cast<Node*>(list_nth(custom->custom_exprs, 0)),
                    audit.storage_qual_copies,
                    oids);
                per_row_audit_mmsearch_expression(
                    reinterpret_cast<Node*>(list_nth(custom->custom_exprs, 1)),
                    audit.storage_qual_copies,
                    oids);
                per_row_audit_mmsearch_expression(
                    reinterpret_cast<Node*>(list_nth(custom->custom_exprs, 2)),
                    audit.vector_qual_copies,
                    oids);
                for (int index = 3; index < list_length(custom->custom_exprs); ++index) {
                    Node* owner_expression =
                        reinterpret_cast<Node*>(list_nth(custom->custom_exprs, index));
                    if (exact_owner_candidate == nullptr) {
                        per_row_audit_mmsearch_expression(
                            owner_expression, audit.descendant_owner_copies, oids);
                    } else {
                        per_row_audit_exact_mmsearch_owner(owner_expression,
                                                           exact_owner_candidate,
                                                           audit.descendant_owner_copies,
                                                           oids);
                    }
                }
            } else {
                per_row_audit_mmsearch_expression(reinterpret_cast<Node*>(custom->custom_exprs),
                                                  audit.executable_qual_copies,
                                                  oids);
            }
            for (Plan* child : postgres_list<Plan>(custom->custom_plans)) {
                per_row_audit_mmsearch_plan(child, audit, oids, exact_owner_candidate);
            }
            return;
        }
        if (IsA(plan, Append)) {
            for (Plan* child : postgres_list<Plan>(reinterpret_cast<Append*>(plan)->appendplans)) {
                per_row_audit_mmsearch_plan(child, audit, oids, exact_owner_candidate);
            }
            return;
        }
        if (IsA(plan, MergeAppend)) {
            for (Plan* child :
                 postgres_list<Plan>(reinterpret_cast<MergeAppend*>(plan)->mergeplans)) {
                per_row_audit_mmsearch_plan(child, audit, oids, exact_owner_candidate);
            }
            return;
        }
        if (IsA(plan, BitmapAnd)) {
            for (Plan* child :
                 postgres_list<Plan>(reinterpret_cast<BitmapAnd*>(plan)->bitmapplans)) {
                per_row_audit_mmsearch_plan(child, audit, oids, exact_owner_candidate);
            }
            return;
        }
        if (IsA(plan, BitmapOr)) {
            for (Plan* child :
                 postgres_list<Plan>(reinterpret_cast<BitmapOr*>(plan)->bitmapplans)) {
                per_row_audit_mmsearch_plan(child, audit, oids, exact_owner_candidate);
            }
            return;
        }
        if (IsA(plan, SubqueryScan)) {
            per_row_audit_mmsearch_plan(
                reinterpret_cast<SubqueryScan*>(plan)->subplan, audit, oids, exact_owner_candidate);
            return;
        }
        per_row_audit_mmsearch_plan(outerPlan(plan), audit, oids, exact_owner_candidate);
        if (innerPlan(plan) != outerPlan(plan)) {
            per_row_audit_mmsearch_plan(innerPlan(plan), audit, oids, exact_owner_candidate);
        }
    }

    [[nodiscard]] Path* per_row_path_without_owned_qual(RelOptInfo* rel,
                                                        RestrictInfo* owned,
                                                        const bool require_bioseq_source) noexcept {
        if (rel == nullptr || owned == nullptr)
            return nullptr;
        Path* best = nullptr;
        for (Path* candidate : postgres_list<Path>(rel->pathlist)) {
            if (candidate == nullptr || candidate->parent != rel ||
                batch_join_path_contains_parallel_execution(candidate) ||
                per_row_descendant_owns_restriction(candidate, owned) ||
                (require_bioseq_source && !slice_project_path_tree_has_bioseq_scan(candidate)) ||
                !std::isfinite(candidate->total_cost) || candidate->total_cost < 0)
                continue;
            if (best == nullptr || candidate->total_cost < best->total_cost) {
                best = candidate;
            }
        }
        return best;
    }

    /* A reconstructed join may expose raw sequence Vars that its incumbent child
     * PathTargets did not need after core pushed the original join predicate.
     * Add only positive, level-zero raw Vars owned by this child's relids.  The
     * projection is a new Path node; shared child paths and PHV/whole-row
     * semantics are never mutated or synthesized. */
    [[nodiscard]] Path*
    per_row_project_required_child_vars(PlannerInfo* root, Path* child, List* required_vars) {
        if (root == nullptr || child == nullptr || child->parent == nullptr ||
            child->pathtarget == nullptr || required_vars == nullptr) {
            return nullptr;
        }
        PathTarget* projected = copy_pathtarget(child->pathtarget);
        bool added = false;
        for (Node* expression : postgres_list<Node>(required_vars)) {
            Var* var = try_cast_pg_node<Var>(expression);
            const bool row_identity = per_row_is_exact_row_identity_var(var);
            if (var == nullptr || !bioseq_is_concrete_rte_varno(var->varno) ||
                (var->varattno <= 0 && !row_identity) || var->varlevelsup != 0 ||
                !bms_is_empty(var->varnullingrels))
                return nullptr;
            if (!bms_is_member(static_cast<int>(var->varno), child->parent->relids))
                continue;
            if (!path_target_contains_expression(projected, reinterpret_cast<Expr*>(var))) {
                add_column_to_pathtarget(projected, reinterpret_cast<Expr*>(copyObject(var)), 0);
                added = true;
            }
        }
        if (!added)
            return child;
        set_pathtarget_cost_width(root, projected);
        return reinterpret_cast<Path*>(
            create_projection_path(root, child->parent, child, projected));
    }

    struct per_row_descendant_ownership {
        CustomPath* owner = nullptr;
        std::size_t matches = 0u;
        bool invalid_owner_abi = false;
        bool unsupported_wrapper = false;
    };

    [[nodiscard]] Node* per_row_known_path_owner_recipe(CustomPath* custom, bool& invalid) {
        if (custom == nullptr)
            return nullptr;
        const bool raw_owner = custom->methods == bioseq_batch_join_path_methods();
        const bool transformed_owner = custom->methods == bioseq_per_row_join_path_methods();
        if (!raw_owner && !transformed_owner)
            return nullptr;
        const int expected_version =
            raw_owner ? k_bioseq_batch_join_plan_version : k_bioseq_per_row_join_plan_version;
        const int expected_children = raw_owner ? 2 : 1;
        const int expected_fields = raw_owner ? k_bioseq_batch_join_path_private_fields
                                              : k_bioseq_per_row_join_path_private_fields;
        Node* version = list_length(custom->custom_private) == expected_fields
                            ? reinterpret_cast<Node*>(linitial(custom->custom_private))
                            : nullptr;
        Node* recipe = list_length(custom->custom_private) == expected_fields
                           ? reinterpret_cast<Node*>(list_nth(custom->custom_private, 4))
                           : nullptr;
        if (version == nullptr || !IsA(version, Integer) || intVal(version) != expected_version ||
            list_length(custom->custom_paths) != expected_children || recipe == nullptr ||
            exprType(recipe) != get_cached_stub_oids().bioseq_seq_type_oid) {
            invalid = true;
            return nullptr;
        }
        return recipe;
    }

    void per_row_analyze_descendant_exact_owner(Path* path,
                                                Node* helper_argument,
                                                per_row_descendant_ownership& ownership) {
        if (path == nullptr || helper_argument == nullptr || ownership.invalid_owner_abi ||
            ownership.unsupported_wrapper)
            return;
        check_stack_depth();
        if (IsA(path, CustomPath)) {
            auto* custom = reinterpret_cast<CustomPath*>(path);
            const bool known_owner = custom->methods == bioseq_batch_join_path_methods() ||
                                     custom->methods == bioseq_per_row_join_path_methods();
            if (known_owner) {
                Node* recipe = per_row_known_path_owner_recipe(custom, ownership.invalid_owner_abi);
                if (ownership.invalid_owner_abi)
                    return;
                if (recipe != nullptr && equal(helper_argument, recipe)) {
                    ++ownership.matches;
                    ownership.owner = custom;
                }
            } else if (custom->methods != bioseq_column_scan_path_methods() &&
                       custom->methods != &bioseq_slice_project_path_methods()) {
                /* Unknown CustomPaths can hide an evaluation/projection frontier in
                 * provider-private state.  Never infer helper provenance through one. */
                ownership.unsupported_wrapper = true;
                return;
            }
            for (Path* child : postgres_list<Path>(custom->custom_paths)) {
                per_row_analyze_descendant_exact_owner(child, helper_argument, ownership);
            }
            return;
        }
        auto descend_join = [helper_argument, &ownership](JoinPath* join) {
            if (join == nullptr)
                return;
            per_row_analyze_descendant_exact_owner(join->outerjoinpath, helper_argument, ownership);
            per_row_analyze_descendant_exact_owner(join->innerjoinpath, helper_argument, ownership);
        };
        if (IsA(path, NestPath)) {
            descend_join(&reinterpret_cast<NestPath*>(path)->jpath);
            return;
        }
        if (IsA(path, MergePath)) {
            descend_join(&reinterpret_cast<MergePath*>(path)->jpath);
            return;
        }
        if (IsA(path, HashPath)) {
            descend_join(&reinterpret_cast<HashPath*>(path)->jpath);
            return;
        }
        auto descend_one = [helper_argument, &ownership](Path* child) {
            per_row_analyze_descendant_exact_owner(child, helper_argument, ownership);
        };
        if (IsA(path, ProjectionPath)) {
            descend_one(reinterpret_cast<ProjectionPath*>(path)->subpath);
            return;
        }
        if (IsA(path, ProjectSetPath)) {
            descend_one(reinterpret_cast<ProjectSetPath*>(path)->subpath);
            return;
        }
        if (IsA(path, SortPath)) {
            descend_one(reinterpret_cast<SortPath*>(path)->subpath);
            return;
        }
        if (IsA(path, IncrementalSortPath)) {
            descend_one(reinterpret_cast<IncrementalSortPath*>(path)->spath.subpath);
            return;
        }
        if (IsA(path, MaterialPath)) {
            descend_one(reinterpret_cast<MaterialPath*>(path)->subpath);
            return;
        }
        if (IsA(path, MemoizePath)) {
            descend_one(reinterpret_cast<MemoizePath*>(path)->subpath);
            return;
        }
        if (IsA(path, UniquePath)) {
            descend_one(reinterpret_cast<UniquePath*>(path)->subpath);
            return;
        }
        if (IsA(path, GatherPath)) {
            descend_one(reinterpret_cast<GatherPath*>(path)->subpath);
            return;
        }
        if (IsA(path, GatherMergePath)) {
            descend_one(reinterpret_cast<GatherMergePath*>(path)->subpath);
            return;
        }
        auto descend_list = [helper_argument, &ownership](List* paths) {
            for (Path* child : postgres_list<Path>(paths)) {
                per_row_analyze_descendant_exact_owner(child, helper_argument, ownership);
            }
        };
        if (IsA(path, AppendPath)) {
            descend_list(reinterpret_cast<AppendPath*>(path)->subpaths);
            return;
        }
        if (IsA(path, MergeAppendPath)) {
            descend_list(reinterpret_cast<MergeAppendPath*>(path)->subpaths);
            return;
        }
        /* Ordinary base-relation scan paths have no child Path.  Anything else is
         * an unmodelled join/subquery/foreign wrapper and must fail closed. */
        if (path->parent == nullptr || (path->parent->reloptkind != RELOPT_BASEREL &&
                                        path->parent->reloptkind != RELOPT_OTHER_MEMBER_REL)) {
            ownership.unsupported_wrapper = true;
        }
    }

    /* Genuine recursion cycle: the path clone owns unary nodes while its list helper
     * owns Append/MergeAppend children, and each must call the other. */
    [[nodiscard]] Path* per_row_clone_path_to_helper_owner(PlannerInfo* root,
                                                           Path* path,
                                                           CustomPath* owner,
                                                           Node* identity,
                                                           bool& applied,
                                                           bool& invalid);

    [[nodiscard]] PathTarget* per_row_helper_passthrough_target(PlannerInfo* root,
                                                                PathTarget* source,
                                                                Node* identity,
                                                                bool& invalid) {
        if (root == nullptr || source == nullptr || identity == nullptr) {
            invalid = true;
            return source;
        }
        std::vector<Index> sortgrouprefs;
        if (!per_row_authoritative_sortgrouprefs(root, source->exprs, sortgrouprefs) ||
            sortgrouprefs.size() != static_cast<std::size_t>(list_length(source->exprs))) {
            invalid = true;
            return source;
        }
        List* expressions = static_cast<List*>(copyObject(source->exprs));
        if (!per_row_expression_list_contains(expressions, identity)) {
            expressions = lappend(expressions, copyObject(identity));
            sortgrouprefs.push_back(0);
        }
        PathTarget* target = create_empty_pathtarget();
        target->exprs = expressions;
        const bool has_sortgroupref = std::any_of(sortgrouprefs.begin(),
                                                  sortgrouprefs.end(),
                                                  [](Index value) noexcept { return value != 0; });
        if (has_sortgroupref) {
            target->sortgrouprefs =
                static_cast<Index*>(palloc0(sortgrouprefs.size() * sizeof(Index)));
            for (std::size_t index = 0u; index < sortgrouprefs.size(); ++index) {
                target->sortgrouprefs[index] = sortgrouprefs[index];
            }
        }
        set_pathtarget_cost_width(root, target);
        return target;
    }

    [[nodiscard]] List* per_row_clone_path_list_to_helper_owner(PlannerInfo* root,
                                                                List* paths,
                                                                CustomPath* owner,
                                                                Node* identity,
                                                                bool& applied,
                                                                bool& invalid) {
        List* result = NIL;
        bool changed = false;
        for (Path* child : postgres_list<Path>(paths)) {
            Path* replacement = applied || invalid
                                    ? child
                                    : per_row_clone_path_to_helper_owner(
                                          root, child, owner, identity, applied, invalid);
            changed = changed || replacement != child;
            result = lappend(result, replacement);
        }
        return changed ? result : paths;
    }

    [[nodiscard]] Path* per_row_clone_path_to_helper_owner(PlannerInfo* root,
                                                           Path* path,
                                                           CustomPath* owner,
                                                           Node* identity,
                                                           bool& applied,
                                                           bool& invalid) {
        if (path == nullptr || owner == nullptr || identity == nullptr || applied || invalid) {
            return path;
        }
        if (path == reinterpret_cast<Path*>(owner)) {
            auto* clone = makeNode(CustomPath);
            *clone = *owner;
            clone->path.pathtarget =
                per_row_helper_passthrough_target(root, path->pathtarget, identity, invalid);
            if (invalid)
                return path;
            applied = true;
            return reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, CustomPath)) {
            auto* source = reinterpret_cast<CustomPath*>(path);
            List* children = per_row_clone_path_list_to_helper_owner(
                root, source->custom_paths, owner, identity, applied, invalid);
            if (!applied || invalid)
                return path;
            auto* clone = makeNode(CustomPath);
            *clone = *source;
            clone->custom_paths = children;
            clone->path.pathtarget =
                per_row_helper_passthrough_target(root, source->path.pathtarget, identity, invalid);
            if (invalid)
                return path;
            return reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, NestPath)) {
            auto* source = reinterpret_cast<NestPath*>(path);
            auto* clone = makeNode(NestPath);
            *clone = *source;
            clone->jpath.outerjoinpath = per_row_clone_path_to_helper_owner(
                root, source->jpath.outerjoinpath, owner, identity, applied, invalid);
            clone->jpath.innerjoinpath = per_row_clone_path_to_helper_owner(
                root, source->jpath.innerjoinpath, owner, identity, applied, invalid);
            if (!applied || invalid)
                return path;
            clone->jpath.path.pathtarget = per_row_helper_passthrough_target(
                root, source->jpath.path.pathtarget, identity, invalid);
            return invalid ? path : reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, MergePath)) {
            auto* source = reinterpret_cast<MergePath*>(path);
            auto* clone = makeNode(MergePath);
            *clone = *source;
            clone->jpath.outerjoinpath = per_row_clone_path_to_helper_owner(
                root, source->jpath.outerjoinpath, owner, identity, applied, invalid);
            clone->jpath.innerjoinpath = per_row_clone_path_to_helper_owner(
                root, source->jpath.innerjoinpath, owner, identity, applied, invalid);
            if (!applied || invalid)
                return path;
            clone->jpath.path.pathtarget = per_row_helper_passthrough_target(
                root, source->jpath.path.pathtarget, identity, invalid);
            return invalid ? path : reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, HashPath)) {
            auto* source = reinterpret_cast<HashPath*>(path);
            auto* clone = makeNode(HashPath);
            *clone = *source;
            clone->jpath.outerjoinpath = per_row_clone_path_to_helper_owner(
                root, source->jpath.outerjoinpath, owner, identity, applied, invalid);
            clone->jpath.innerjoinpath = per_row_clone_path_to_helper_owner(
                root, source->jpath.innerjoinpath, owner, identity, applied, invalid);
            if (!applied || invalid)
                return path;
            clone->jpath.path.pathtarget = per_row_helper_passthrough_target(
                root, source->jpath.path.pathtarget, identity, invalid);
            return invalid ? path : reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, ProjectionPath)) {
            auto* source = reinterpret_cast<ProjectionPath*>(path);
            Path* child = per_row_clone_path_to_helper_owner(
                root, source->subpath, owner, identity, applied, invalid);
            if (!applied || invalid)
                return path;
            auto* clone = makeNode(ProjectionPath);
            *clone = *source;
            clone->subpath = child;
            reinterpret_cast<Path*>(clone)->pathtarget =
                per_row_helper_passthrough_target(root, path->pathtarget, identity, invalid);
            return invalid ? path : reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, ProjectSetPath)) {
            auto* source = reinterpret_cast<ProjectSetPath*>(path);
            Path* child = per_row_clone_path_to_helper_owner(
                root, source->subpath, owner, identity, applied, invalid);
            if (!applied || invalid)
                return path;
            auto* clone = makeNode(ProjectSetPath);
            *clone = *source;
            clone->subpath = child;
            reinterpret_cast<Path*>(clone)->pathtarget =
                per_row_helper_passthrough_target(root, path->pathtarget, identity, invalid);
            return invalid ? path : reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, SortPath)) {
            auto* source = reinterpret_cast<SortPath*>(path);
            Path* child = per_row_clone_path_to_helper_owner(
                root, source->subpath, owner, identity, applied, invalid);
            if (!applied || invalid)
                return path;
            auto* clone = makeNode(SortPath);
            *clone = *source;
            clone->subpath = child;
            reinterpret_cast<Path*>(clone)->pathtarget =
                per_row_helper_passthrough_target(root, path->pathtarget, identity, invalid);
            return invalid ? path : reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, MaterialPath)) {
            auto* source = reinterpret_cast<MaterialPath*>(path);
            Path* child = per_row_clone_path_to_helper_owner(
                root, source->subpath, owner, identity, applied, invalid);
            if (!applied || invalid)
                return path;
            auto* clone = makeNode(MaterialPath);
            *clone = *source;
            clone->subpath = child;
            reinterpret_cast<Path*>(clone)->pathtarget =
                per_row_helper_passthrough_target(root, path->pathtarget, identity, invalid);
            return invalid ? path : reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, MemoizePath)) {
            auto* source = reinterpret_cast<MemoizePath*>(path);
            Path* child = per_row_clone_path_to_helper_owner(
                root, source->subpath, owner, identity, applied, invalid);
            if (!applied || invalid)
                return path;
            auto* clone = makeNode(MemoizePath);
            *clone = *source;
            clone->subpath = child;
            reinterpret_cast<Path*>(clone)->pathtarget =
                per_row_helper_passthrough_target(root, path->pathtarget, identity, invalid);
            return invalid ? path : reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, UniquePath)) {
            auto* source = reinterpret_cast<UniquePath*>(path);
            Path* child = per_row_clone_path_to_helper_owner(
                root, source->subpath, owner, identity, applied, invalid);
            if (!applied || invalid)
                return path;
            auto* clone = makeNode(UniquePath);
            *clone = *source;
            clone->subpath = child;
            reinterpret_cast<Path*>(clone)->pathtarget =
                per_row_helper_passthrough_target(root, path->pathtarget, identity, invalid);
            return invalid ? path : reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, GatherPath)) {
            auto* source = reinterpret_cast<GatherPath*>(path);
            Path* child = per_row_clone_path_to_helper_owner(
                root, source->subpath, owner, identity, applied, invalid);
            if (!applied || invalid)
                return path;
            auto* clone = makeNode(GatherPath);
            *clone = *source;
            clone->subpath = child;
            reinterpret_cast<Path*>(clone)->pathtarget =
                per_row_helper_passthrough_target(root, path->pathtarget, identity, invalid);
            return invalid ? path : reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, GatherMergePath)) {
            auto* source = reinterpret_cast<GatherMergePath*>(path);
            Path* child = per_row_clone_path_to_helper_owner(
                root, source->subpath, owner, identity, applied, invalid);
            if (!applied || invalid)
                return path;
            auto* clone = makeNode(GatherMergePath);
            *clone = *source;
            clone->subpath = child;
            reinterpret_cast<Path*>(clone)->pathtarget =
                per_row_helper_passthrough_target(root, path->pathtarget, identity, invalid);
            return invalid ? path : reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, IncrementalSortPath)) {
            auto* source = reinterpret_cast<IncrementalSortPath*>(path);
            Path* child = per_row_clone_path_to_helper_owner(
                root, source->spath.subpath, owner, identity, applied, invalid);
            if (!applied || invalid)
                return path;
            auto* clone = makeNode(IncrementalSortPath);
            *clone = *source;
            clone->spath.subpath = child;
            clone->spath.path.pathtarget = per_row_helper_passthrough_target(
                root, source->spath.path.pathtarget, identity, invalid);
            return invalid ? path : reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, AppendPath)) {
            auto* source = reinterpret_cast<AppendPath*>(path);
            List* children = per_row_clone_path_list_to_helper_owner(
                root, source->subpaths, owner, identity, applied, invalid);
            if (!applied || invalid)
                return path;
            auto* clone = makeNode(AppendPath);
            *clone = *source;
            clone->subpaths = children;
            clone->path.pathtarget =
                per_row_helper_passthrough_target(root, source->path.pathtarget, identity, invalid);
            return invalid ? path : reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, MergeAppendPath)) {
            auto* source = reinterpret_cast<MergeAppendPath*>(path);
            List* children = per_row_clone_path_list_to_helper_owner(
                root, source->subpaths, owner, identity, applied, invalid);
            if (!applied || invalid)
                return path;
            auto* clone = makeNode(MergeAppendPath);
            *clone = *source;
            clone->subpaths = children;
            clone->path.pathtarget =
                per_row_helper_passthrough_target(root, source->path.pathtarget, identity, invalid);
            return invalid ? path : reinterpret_cast<Path*>(clone);
        }
        return path;
    }

    /* A reconstructed transformed owner is deliberately narrower than the
     * incumbent join relation: helpers owned by the new PerRow node must not leak
     * into a descendant BioseqScan or an older BatchJoin.  PostgreSQL Path nodes
     * are shared, so normalize only after slice_project_clone_mutable_path_spine()
     * has made every supported container private.  Availability is scoped to the
     * current subtree: a descendant-owned helper is retained at its owner and
     * above it, but is removed from every child below that owner. */
    struct per_row_base_helper_ownership {
        CustomPath* owner = nullptr;
        std::size_t matches = 0u;
        bool invalid_owner_abi = false;
        bool unsupported_wrapper = false;
    };

    [[nodiscard]] FuncExpr*
    per_row_helper_identity_function(Node* identity, bioseq_stub_oids const& oids) noexcept {
        if (PlaceHolderVar* placeholder = try_cast_pg_node<PlaceHolderVar>(identity)) {
            return batch_join_match_helper_phv_function(placeholder, oids);
        }
        return batch_join_well_formed_match_helper(identity, oids);
    }

    [[nodiscard]] Node* per_row_base_scan_owner_recipe(CustomPath* custom, bool& invalid) {
        if (custom == nullptr || custom->methods != bioseq_column_scan_path_methods()) {
            return nullptr;
        }
        Node* recipe = nullptr;
        for (Node* private_node : postgres_list<Node>(custom->custom_private)) {
            if (private_node == nullptr || !IsA(private_node, List))
                continue;
            List* tag = reinterpret_cast<List*>(private_node);
            Node* magic = list_length(tag) >= 1 ? reinterpret_cast<Node*>(linitial(tag)) : nullptr;
            if (magic == nullptr || !IsA(magic, Integer) ||
                intVal(magic) != k_bioseq_mmsearch_owner_magic)
                continue;
            Node* version =
                list_length(tag) == 3 ? reinterpret_cast<Node*>(list_nth(tag, 1)) : nullptr;
            Node* expressions_node =
                list_length(tag) == 3 ? reinterpret_cast<Node*>(list_nth(tag, 2)) : nullptr;
            List* expressions = expressions_node != nullptr && IsA(expressions_node, List)
                                    ? reinterpret_cast<List*>(expressions_node)
                                    : NIL;
            FuncExpr* mmsearch =
                expressions == NIL
                    ? nullptr
                    : try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(linitial(expressions)));
            if (recipe != nullptr || version == nullptr || !IsA(version, Integer) ||
                intVal(version) != k_bioseq_mmsearch_owner_version || mmsearch == nullptr ||
                list_length(expressions) > 2 ||
                !is_mmsearch_funcid(mmsearch->funcid, get_cached_stub_oids()) ||
                (list_length(mmsearch->args) != 2 && list_length(mmsearch->args) != 3) ||
                list_length(custom->custom_restrictinfo) != list_length(expressions)) {
                invalid = true;
                return nullptr;
            }
            for (Node* expression : postgres_list<Node>(expressions)) {
                FuncExpr* owned = try_cast_pg_node<FuncExpr>(expression);
                if (owned == nullptr ||
                    !is_mmsearch_funcid(owned->funcid, get_cached_stub_oids()) ||
                    (list_length(owned->args) != 2 && list_length(owned->args) != 3)) {
                    invalid = true;
                    return nullptr;
                }
            }
            recipe = reinterpret_cast<Node*>(linitial(mmsearch->args));
            if (recipe == nullptr ||
                exprType(recipe) != get_cached_stub_oids().bioseq_seq_type_oid) {
                invalid = true;
                return nullptr;
            }
        }
        return recipe;
    }

} // export custom-scan partition declarations

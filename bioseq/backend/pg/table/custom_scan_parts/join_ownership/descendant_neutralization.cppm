/**
 * @file
 * @brief Checks for duplicate mmsearch evaluation below a join and carries match helpers up from their owner
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

export module bioseq.postgres.table:custom_scan_join_ownership_descendant_neutralization;

import std;
import bioseq.postgres.support;
import :custom_scan_plan_abi_constants;
import :custom_scan_oid_expression_support_oid_catalog;
import :custom_scan_oid_expression_support_expression_analysis;
import :custom_scan_planner_method_contracts;
import :custom_scan_slice_rewrite_path_tree;
import :custom_scan_join_ownership_carrier_lineage;
export import :custom_scan_join_ownership_path_owner_contracts;
import :custom_scan_join_ownership_target_contracts;
export import :custom_scan_join_ownership_descendant_path_contracts;
export import :custom_scan_join_ownership_descendant_neutralization_contracts;

using namespace bioseq::postgres::support;

namespace {
    /**
     * @brief Copies a one-child Path wrapper after its child or output has changed.
     *
     * Callers provide the tag  check and makeNode call because PostgreSQL builds
     * those macros from type names.
     */
    template <typename PathType,
              Path* PathType::* ChildMember,
              typename MatchesPath,
              typename MakePath,
              typename CloneChild,
              typename RewriteTarget>
    [[nodiscard]] [[gnu::always_inline]] inline Path*
    per_row_try_clone_unary_path_to_helper_owner(Path* path,
                                                  bool& applied,
                                                  bool& invalid,
                                                  const MatchesPath& matches_path,
                                                  const MakePath& make_path,
                                                  const CloneChild& clone_child,
                                                  const RewriteTarget& rewrite_target) {
        static_assert(std::is_invocable_r_v<bool, const MatchesPath&, Path*>);
        static_assert(std::is_invocable_r_v<PathType*, const MakePath&>);
        static_assert(std::is_invocable_r_v<Path*, const CloneChild&, Path*>);
        static_assert(std::is_invocable_r_v<PathTarget*, const RewriteTarget&, PathTarget*>);
        if (!matches_path(path))
            return nullptr;
        auto* source = reinterpret_cast<PathType*>(path);
        Path* child = clone_child(source->*ChildMember);
        if (!applied || invalid)
            return path;
        auto* clone = make_path();
        *clone = *source;
        clone->*ChildMember = child;
        reinterpret_cast<Path*>(clone)->pathtarget = rewrite_target(path->pathtarget);
        return invalid ? path : reinterpret_cast<Path*>(clone);
    }
} // namespace

export {
    /** @brief Copies the restriction list while leaving out the specified RestrictInfo pointer. */
    [[nodiscard]] List* per_row_without_restriction(List* restrictions, RestrictInfo* owned) {
        List* filtered = NIL;
        for (RestrictInfo* candidate : postgres_list<RestrictInfo>(restrictions)) {
            if (candidate != owned)
                filtered = lappend(filtered, candidate);
        }
        return filtered;
    }

    /**
     * @brief Checks whether this path or any supported child still uses the restriction.
     *
     * PostgreSQL can push a join restriction into ParamPathInfo or a child scan
     * while leaving it in the join list. Check the exact RestrictInfo pointer
     * throughout the remaining children so mmsearch has only one executor owner.
     */
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

    /** @brief Adds the expression's mmsearch count, saturating the counter on overflow. */
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

    /** @brief The mmsearch call to match and  the number of equal copies found. */
    struct exact_mmsearch_owner_audit {
        FuncExpr* candidate = nullptr;
        bioseq_stub_oids const* oids = nullptr;
        std::size_t matches = 0u;
    };

    /** @brief Counts equal mmsearch calls, skipping nested calls within their arguments. */
    struct exact_mmsearch_owner_audit_visitor {
        /** @brief Counts this node when it equals the selected mmsearch call. */
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

    /** @brief Adds the number of copies of this mmsearch call, saturating the counter on overflow. */
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

    /**
     * @brief Counts mmsearch calls left in the child plan after assigning a join owner.
     *
     * Check jkin and index conditions as well as Plan::qual. BioseqScan also stores
     * storage filters, vector filters, and executor-owned searches in custom_exprs.
     * A nested search with a different expression is valid. When a candidate is
     * supplied, count only equal copies in those executor-owner fields.
     */
    void per_row_audit_mmsearch_plan(Plan* plan,
                                     per_row_mmsearch_plan_audit& audit,
                                     bioseq_stub_oids const& oids,
                                     FuncExpr* exact_owner_candidate) {
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

    /** @brief Finds the cheapest serial  path that leaves this restriction for the parent to evaluate */
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

    /**
     * @brief Adds input columns needed by a rebuilt join through a new child projection.
     *
     * The join may need columns absent from the original child output. Accept
     * local, non-nulled column Vars and supported row-identity Vars belonging to
     * this, child. Copy the output list so other planner paths can keep using the
     * original child unchanged.
     */
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

    /** @brief Finds descendant BatchJoin or PerRow nodes that  own this helper's sequence expression. */
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
                /* An unknown CustomPath may evaluate expressions stored in private fields.
                 * Stop here because its helper ownership cannot be checked. */
                ownership.unsupported_wrapper = true;
                return;
            }
            for (Path* child : postgres_list<Path>(custom->custom_paths)) {
                per_row_analyze_descendant_exact_owner(child, helper_argument, ownership);
            }
            return;
        }
        // Visits both children of a supported join wrapper.
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
        // Visits the sole child of a supported unary wrapper.
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
        // Visits every child of a supported append wrapper.
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
        /* Base-relation scan paths end the walk. Reject other unrecognized paths
         * because they may contain children or evaluate helpers themselve. */
        if (path->parent == nullptr || (path->parent->reloptkind != RELOPT_BASEREL &&
                                        path->parent->reloptkind != RELOPT_OTHER_MEMBER_REL)) {
            ownership.unsupported_wrapper = true;
        }
    }

    /** @brief Copies the path from a helper owner to its parent and adds the helper to each output. */
    [[nodiscard]] Path* per_row_clone_path_to_helper_owner(PlannerInfo* root,
                                                           Path* path,
                                                           CustomPath* owner,
                                                           Node* identity,
                                                           bool& applied,
                                                           bool& invalid);

    /** @brief Copies a path's output and adds the helper, preserving its sort and group references. */
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

    /** @brief Copies the child subtree containing the  selected helper owner, leaving the other children shared. */
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

    /** @brief Copies paths up from the selected helper owner so each can pass its result to the parent. */
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
        const auto clone_child = [root, owner, identity, &applied, &invalid](Path* child) {
            return per_row_clone_path_to_helper_owner(
                root, child, owner, identity, applied, invalid);
        };
        const auto rewrite_target = [root, identity, &invalid](PathTarget* target) {
            return per_row_helper_passthrough_target(root, target, identity, invalid);
        };
        if (Path* replacement = per_row_try_clone_unary_path_to_helper_owner<
                ProjectionPath, &ProjectionPath::subpath>(
                path,
                applied,
                invalid,
                [](Path* candidate) noexcept { return IsA(candidate, ProjectionPath); },
                []() -> ProjectionPath* { return makeNode(ProjectionPath); },
                clone_child,
                rewrite_target);
            replacement != nullptr)
            return replacement;
        if (Path* replacement = per_row_try_clone_unary_path_to_helper_owner<
                ProjectSetPath, &ProjectSetPath::subpath>(
                path,
                applied,
                invalid,
                [](Path* candidate) noexcept { return IsA(candidate, ProjectSetPath); },
                []() -> ProjectSetPath* { return makeNode(ProjectSetPath); },
                clone_child,
                rewrite_target);
            replacement != nullptr)
            return replacement;
        if (Path* replacement = per_row_try_clone_unary_path_to_helper_owner<
                SortPath, &SortPath::subpath>(
                path,
                applied,
                invalid,
                [](Path* candidate) noexcept { return IsA(candidate, SortPath); },
                []() -> SortPath* { return makeNode(SortPath); },
                clone_child,
                rewrite_target);
            replacement != nullptr)
            return replacement;
        if (Path* replacement = per_row_try_clone_unary_path_to_helper_owner<
                MaterialPath, &MaterialPath::subpath>(
                path,
                applied,
                invalid,
                [](Path* candidate) noexcept { return IsA(candidate, MaterialPath); },
                []() -> MaterialPath* { return makeNode(MaterialPath); },
                clone_child,
                rewrite_target);
            replacement != nullptr)
            return replacement;
        if (Path* replacement = per_row_try_clone_unary_path_to_helper_owner<
                MemoizePath, &MemoizePath::subpath>(
                path,
                applied,
                invalid,
                [](Path* candidate) noexcept { return IsA(candidate, MemoizePath); },
                []() -> MemoizePath* { return makeNode(MemoizePath); },
                clone_child,
                rewrite_target);
            replacement != nullptr)
            return replacement;
        if (Path* replacement = per_row_try_clone_unary_path_to_helper_owner<
                UniquePath, &UniquePath::subpath>(
                path,
                applied,
                invalid,
                [](Path* candidate) noexcept { return IsA(candidate, UniquePath); },
                []() -> UniquePath* { return makeNode(UniquePath); },
                clone_child,
                rewrite_target);
            replacement != nullptr)
            return replacement;
        if (Path* replacement = per_row_try_clone_unary_path_to_helper_owner<
                GatherPath, &GatherPath::subpath>(
                path,
                applied,
                invalid,
                [](Path* candidate) noexcept { return IsA(candidate, GatherPath); },
                []() -> GatherPath* { return makeNode(GatherPath); },
                clone_child,
                rewrite_target);
            replacement != nullptr)
            return replacement;
        if (Path* replacement = per_row_try_clone_unary_path_to_helper_owner<
                GatherMergePath, &GatherMergePath::subpath>(
                path,
                applied,
                invalid,
                [](Path* candidate) noexcept { return IsA(candidate, GatherMergePath); },
                []() -> GatherMergePath* { return makeNode(GatherMergePath); },
                clone_child,
                rewrite_target);
            replacement != nullptr)
            return replacement;
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

    /** @brief Returns the match-helper call, unwrapping a valid PlaceHolderVar when present. */
    [[nodiscard]] FuncExpr*
    per_row_helper_identity_function(Node* identity, bioseq_stub_oids const& oids) noexcept {
        if (PlaceHolderVar* placeholder = try_cast_pg_node<PlaceHolderVar>(identity)) {
            return batch_join_match_helper_phv_function(placeholder, oids);
        }
        return batch_join_well_formed_match_helper(identity, oids);
    }

} // export custom-scan partition declarations

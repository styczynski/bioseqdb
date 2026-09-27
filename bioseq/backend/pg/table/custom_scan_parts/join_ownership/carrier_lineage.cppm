/**
 * @file
 * @brief Owns target projection, helper lineage, and carrier path discovery.
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

export module bioseq.postgres.table:custom_scan_join_ownership_carrier_lineage;

import std;
import bioseq.postgres.support;
import :custom_scan_oid_expression_support_oid_catalog;
import :custom_scan_oid_expression_support_expression_analysis;
import :custom_scan_planner_method_contracts;
export import :custom_scan_join_ownership_helper_contracts;
export import :custom_scan_join_ownership_carrier_lineage_contracts;

using namespace bioseq::postgres::support;

export {

    /** @brief Tracks forbidden target references while validating a locator projection. */
    struct batch_join_target_projection_probe {
        Index target_varno = 0;
        AttrNumber target_seq_attno = InvalidAttrNumber;
        const bioseq_stub_oids* oids = nullptr;
        bool allow_mmsearch_qualifier = false;
        bool forbidden = false;
    };

    /** @brief Finds target sequence Vars that would escape into ordinary SQL evaluation. */
    struct batch_join_target_projection_visitor {
        /** @brief Examines one projection node while treating owned helpers as atomic. */
        [[nodiscard]] pg_walk_step
        operator()(pg_expression_walk_cursor,
                   pg_node_view node,
                   batch_join_target_projection_probe& probe) const noexcept {
            /* A validated mmsearch restriction consumes the locator inside the
             * BatchJoin executor, while match-result helpers are evaluated by its
             * parent from the current match_result. Neither argument is a request to
             * let the locator-backed datum escape into ordinary SQL evaluation. A
             * projected mmsearch call remains forbidden. */
            if (FuncExpr* function = node.try_cast<FuncExpr>()) {
                if (probe.oids != nullptr &&
                    (is_bioseq_projection_funcid(function->funcid, *probe.oids) ||
                     (probe.allow_mmsearch_qualifier &&
                      is_mmsearch_funcid(function->funcid, *probe.oids)))) {
                    return pg_walk_step::skip_subtree;
                }
            }
            if (Var* var = node.try_cast<Var>()) {
                if (var->varno == probe.target_varno &&
                    (var->varattno == 0 || var->varattno == probe.target_seq_attno)) {
                    probe.forbidden = true;
                    return pg_walk_step::stop;
                }
            }
            return pg_walk_step::descend;
        }
    };

    /** @brief Checks whether an expression exposes a locator-backed target sequence. */
    [[nodiscard]] bool batch_join_target_expression_is_forbidden(
        Node* expression, const Var* target_var, const bool allow_mmsearch_qualifier) {
        if (expression == nullptr || target_var == nullptr) {
            return false;
        }
        batch_join_target_projection_probe probe{target_var->varno,
                                                 target_var->varattno,
                                                 &get_cached_stub_oids(),
                                                 allow_mmsearch_qualifier,
                                                 false};
        batch_join_target_projection_visitor visitor;
        walk_pg_expression_including_root(expression, probe, visitor);
        return probe.forbidden;
    }

    /** @brief Checks a restriction list for forbidden target-sequence evaluation. */
    [[nodiscard]] bool batch_join_target_restrictions_are_forbidden(
        List* restrictions, const Var* target_var, const bool allow_mmsearch_qualifier) {
        for (Node* node : postgres_list<Node>(restrictions)) {
            RestrictInfo* restriction = try_cast_pg_node<RestrictInfo>(node);
            if (restriction == nullptr || restriction->clause == nullptr ||
                batch_join_target_expression_is_forbidden(
                    reinterpret_cast<Node*>(restriction->clause),
                    target_var,
                    allow_mmsearch_qualifier)) {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Recursively detects parallel execution anywhere below a planner path.
     *
     * Reject a Gather hidden below the common unary planner wrappers. Looking
     * only at cheapest_total_path->pathtype misses Projection/Material wrappers
     * and lets a parallel child enter an executor that owns one backend-local
     * query materialization.
     */
    [[nodiscard]] bool batch_join_path_contains_parallel_execution(const Path* path) noexcept {
        if (path == nullptr) {
            return false;
        }
        if (path->pathtype == T_Gather || path->pathtype == T_GatherMerge || path->parallel_aware ||
            path->parallel_workers > 0) {
            return true;
        }
        switch (path->pathtype) {
        case T_Result:
            return batch_join_path_contains_parallel_execution(
                reinterpret_cast<const ProjectionPath*>(path)->subpath);
        case T_Material:
            return batch_join_path_contains_parallel_execution(
                reinterpret_cast<const MaterialPath*>(path)->subpath);
        case T_Memoize:
            return batch_join_path_contains_parallel_execution(
                reinterpret_cast<const MemoizePath*>(path)->subpath);
        case T_Sort:
            return batch_join_path_contains_parallel_execution(
                reinterpret_cast<const SortPath*>(path)->subpath);
        case T_IncrementalSort:
            return batch_join_path_contains_parallel_execution(
                reinterpret_cast<const IncrementalSortPath*>(path)->spath.subpath);
        case T_NestLoop:
        case T_MergeJoin:
        case T_HashJoin: {
            auto const* join = reinterpret_cast<JoinPath const*>(path);
            return batch_join_path_contains_parallel_execution(join->outerjoinpath) ||
                   batch_join_path_contains_parallel_execution(join->innerjoinpath);
        }
        case T_CustomScan: {
            auto const* custom = reinterpret_cast<CustomPath const*>(path);
            for (Path* child : postgres_list<Path>(custom->custom_paths)) {
                if (batch_join_path_contains_parallel_execution(child)) {
                    return true;
                }
            }
            return false;
        }
        case T_Append: {
            auto const* append = reinterpret_cast<AppendPath const*>(path);
            for (Path* child : postgres_list<Path>(append->subpaths)) {
                if (batch_join_path_contains_parallel_execution(child)) {
                    return true;
                }
            }
            return false;
        }
        case T_MergeAppend: {
            auto const* append = reinterpret_cast<MergeAppendPath const*>(path);
            for (Path* child : postgres_list<Path>(append->subpaths)) {
                if (batch_join_path_contains_parallel_execution(child)) {
                    return true;
                }
            }
            return false;
        }
        default:
            return false;
        }
    }

    /** @brief Checks whether a path target contains a structurally equal expression. */
    [[nodiscard]] bool path_target_has_equal_expression(const PathTarget* target,
                                                        const Node* expression) noexcept {
        if (target == nullptr || expression == nullptr) {
            return false;
        }
        for (Node* existing : postgres_list<Node>(target->exprs)) {
            if (equal(existing, expression)) {
                return true;
            }
        }
        return false;
    }

    /** @brief Decodes a match helper applied directly to the selected target relation. */
    [[nodiscard]] FuncExpr* batch_join_exact_target_helper(Node* expression,
                                                           const Index target_relid) noexcept {
        FuncExpr* helper = try_cast_pg_node<FuncExpr>(expression);
        if (!bioseq_is_concrete_rte_varno(target_relid) || helper == nullptr ||
            !is_bioseq_projection_funcid(helper->funcid, get_cached_stub_oids()) ||
            helper->funcretset || OidIsValid(helper->funcvariadic) ||
            get_func_rettype(helper->funcid) != helper->funcresulttype ||
            list_length(helper->args) != 1) {
            return nullptr;
        }
        Var* argument = try_cast_pg_node<Var>(reinterpret_cast<Node*>(linitial(helper->args)));
        return argument != nullptr && argument->varno == target_relid && argument->varattno > 0 &&
                       argument->varlevelsup == 0 && bms_is_empty(argument->varnullingrels)
                   ? helper
                   : nullptr;
    }

    /** @brief Serializes a target recipe and its base lineage as an owner descriptor. */
    [[nodiscard]] List* make_match_owner_descriptor(Node* target_recipe,
                                                    Index base_relid,
                                                    const bioseq_stub_oids& oids);

    /**
     * @brief Decodes a direct target helper wrapped in its exact single-relation PHV.
     *
     * Do not accept a generic PHV merely because a helper occurs somewhere below
     * it because its evaluation and nulling frontier may belong to unrelated SQL.
     */
    [[nodiscard]] FuncExpr*
    batch_join_exact_helper_phv_function(const PlaceHolderVar* placeholder,
                                         const Index target_relid) noexcept {
        if (placeholder == nullptr || !bioseq_is_concrete_rte_varno(target_relid) ||
            placeholder->phid == 0 || placeholder->phlevelsup != 0 ||
            !bms_is_empty(placeholder->phnullingrels) ||
            bms_num_members(placeholder->phrels) != 1 ||
            !bms_is_member(static_cast<int>(target_relid), placeholder->phrels)) {
            return nullptr;
        }
        return batch_join_exact_target_helper(reinterpret_cast<Node*>(placeholder->phexpr),
                                              target_relid);
    }

    /** @brief Checks for the exact single-relation helper PHV shape. */
    [[nodiscard]] bool batch_join_is_exact_helper_phv(const PlaceHolderVar* placeholder,
                                                      const Index target_relid) noexcept {
        return batch_join_exact_helper_phv_function(placeholder, target_relid) != nullptr;
    }

    /** @brief Tracks whether a projection delta contains only parent-safe helper support. */
    struct batch_join_projection_delta {
        const PathTarget* child_target = nullptr;
        Index target_relid = 0;
        bool saw_exact_helper_phv = false;
        bool invalid = false;
    };

    /** @brief Validates Vars and helper PHVs introduced by a projection wrapper. */
    struct batch_join_projection_delta_visitor {
        /** @brief Checks one wrapper-only expression node against the child target. */
        [[nodiscard]] pg_walk_step
        operator()(pg_expression_walk_cursor,
                   pg_node_view node,
                   batch_join_projection_delta& validation) const noexcept {
            if (PlaceHolderVar* placeholder = node.try_cast<PlaceHolderVar>()) {
                if (!batch_join_is_exact_helper_phv(placeholder, validation.target_relid)) {
                    validation.invalid = true;
                    return pg_walk_step::stop;
                }
                validation.saw_exact_helper_phv = true;
                return pg_walk_step::skip_subtree;
            }
            if (Var* var = node.try_cast<Var>()) {
                if (!path_target_has_equal_expression(validation.child_target,
                                                      reinterpret_cast<Node*>(var))) {
                    validation.invalid = true;
                    return pg_walk_step::stop;
                }
            }
            return pg_walk_step::descend;
        }
    };

    /**
     * @brief Checks that a wrapper-only expression can move to the BatchJoin parent.
     *
     * Validate that the complete wrapper-only expression can be evaluated by the
     * BatchJoin parent without changing semantics: it is nonvolatile, every Var
     * is already produced by the child, every PHV is the exact helper-support
     * shape, and at least one such helper PHV explains the wrapper delta.
     */
    [[nodiscard]] bool batch_join_projection_delta_is_parent_safe(
        Node* expression, const PathTarget* child_target, const Index target_relid) noexcept {
        if (expression == nullptr || child_target == nullptr ||
            contain_volatile_functions(expression)) {
            return false;
        }
        batch_join_projection_delta validation{child_target, target_relid, false, false};
        batch_join_projection_delta_visitor visitor;
        walk_pg_expression_including_root(expression, validation, visitor);
        return !validation.invalid && validation.saw_exact_helper_phv;
    }

    /**
     * @brief Checks whether an expression is a safe non-sequence target Var.
     *
     * A wrapper may also expose an ordinary target attribute needed above the
     * join (for example t.pk).  Carrying that base Var through the locator child
     * is exact; carrying the sequence datum, a whole row, a system attribute, a
     * nulled outer-join Var, or an expression assembled at this level is not.
     */
    [[nodiscard]] bool batch_join_is_safe_plain_target_var(
        Node* expression, const Index target_relid, const AttrNumber target_seq_attno) noexcept {
        Var* var = try_cast_pg_node<Var>(expression);
        return var != nullptr && var->varno == target_relid && var->varattno > 0 &&
               var->varattno != target_seq_attno && var->varlevelsup == 0 &&
               bms_is_empty(var->varnullingrels);
    }

    /**
     * @brief Checks whether a base ProjectionPath can be evaluated by the parent owner.
     *
     * A helper support function can add a ProjectionPath at the base relation.
     * BioseqBatchJoin may look through it only when every wrapper-only expression
     * is either an exact helper expression evaluated by the parent or one plain,
     * non-sequence target Var copied into the locator child. Any other expression
     * is a real evaluation boundary and disqualifies this candidate.
     */
    [[nodiscard]] bool
    batch_join_projection_is_parent_safe(const ProjectionPath* projection,
                                         const AttrNumber target_seq_attno) noexcept {
        if (projection == nullptr || projection->subpath == nullptr ||
            projection->path.pathtype != T_Result ||
            projection->path.parent != projection->subpath->parent ||
            projection->path.parent == nullptr ||
            projection->path.parent->reloptkind != RELOPT_BASEREL ||
            projection->path.parent->relid == 0 || projection->path.param_info != nullptr ||
            projection->subpath->param_info != nullptr ||
            !equal(projection->path.pathkeys, projection->subpath->pathkeys) ||
            projection->path.pathtarget == nullptr || projection->subpath->pathtarget == nullptr) {
            return false;
        }
        bool saw_safe_delta = false;
        for (Node* expression : postgres_list<Node>(projection->path.pathtarget->exprs)) {
            if (path_target_has_equal_expression(projection->subpath->pathtarget, expression)) {
                continue;
            }
            if (batch_join_is_safe_plain_target_var(
                    expression, projection->path.parent->relid, target_seq_attno)) {
                saw_safe_delta = true;
                continue;
            }
            if (!batch_join_projection_delta_is_parent_safe(
                    expression, projection->subpath->pathtarget, projection->path.parent->relid)) {
                return false;
            }
            saw_safe_delta = true;
        }
        return saw_safe_delta;
    }

    /** @brief Finds the cheapest unparameterized Bioseq scan below parent-safe projection wrappers. */
    [[nodiscard]] batch_join_bioseq_path_candidate
    find_unparameterized_bioseq_custom_path(RelOptInfo* rel,
                                            const AttrNumber target_seq_attno) noexcept {
        batch_join_bioseq_path_candidate best{};
        for (Path* path : postgres_list<Path>(rel->pathlist)) {
            if (path == nullptr || path->param_info != nullptr) {
                continue;
            }
            Path* cost_owner = path;
            const PathTarget* locator_target = path->pathtarget;
            /* Planner support can expose safe base attributes and wraps match-result helpers in
             * PlaceHolderVars at their originating base relation.  PostgreSQL can
             * consequently expose the otherwise eligible BioseqScan as the child
             * of a projection path.  The projection must not run in the locator
             * child: BioseqBatchJoin rewrites those helpers to parent-owned result
             * placeholders after the match_result exists.  Look through exactly
             * that validated parent-safe wrapper and retain the underlying scan. */
            if (IsA(path, ProjectionPath)) {
                auto* projection = reinterpret_cast<ProjectionPath*>(path);
                if (!batch_join_projection_is_parent_safe(projection, target_seq_attno) ||
                    batch_join_path_contains_parallel_execution(&projection->path)) {
                    continue;
                }
                locator_target = projection->path.pathtarget;
                path = projection->subpath;
            }
            if (path == nullptr || path->pathtype != T_CustomScan || path->param_info != nullptr ||
                batch_join_path_contains_parallel_execution(path)) {
                continue;
            }
            auto* custom = reinterpret_cast<CustomPath*>(path);
            if (custom->methods != bioseq_column_scan_path_methods()) {
                continue;
            }
            if (best.scan == nullptr ||
                compare_path_costs(cost_owner, best.cost_owner, TOTAL_COST) < 0) {
                best = batch_join_bioseq_path_candidate{custom, cost_owner, locator_target};
            }
        }
        return best;
    }

    /** @brief Builds an exact single-relation helper PHV for projection-guard tests. */
    [[nodiscard]] PlaceHolderVar* make_batch_join_projection_guard_phv(const Oid helper_oid,
                                                                       const Oid result_type,
                                                                       const Index relid,
                                                                       const Index phid) {
        auto* argument = makeVar(relid, 1, INT4OID, -1, InvalidOid, 0);
        auto* helper = makeFuncExpr(helper_oid,
                                    result_type,
                                    list_make1(argument),
                                    InvalidOid,
                                    InvalidOid,
                                    COERCE_EXPLICIT_CALL);
        auto* placeholder = makeNode(PlaceHolderVar);
        placeholder->phexpr = reinterpret_cast<Expr*>(helper);
        placeholder->phrels = bms_make_singleton(static_cast<int>(relid));
        placeholder->phnullingrels = nullptr;
        placeholder->phid = phid;
        placeholder->phlevelsup = 0;
        return placeholder;
    }

    /** @brief Builds a synthetic projection-wrapped Bioseq path for guard tests. */
    [[nodiscard]] batch_join_projection_guard_fixture
    make_batch_join_projection_guard_fixture(const Index relid,
                                             List* wrapper_delta,
                                             const Cost wrapper_startup,
                                             const Cost wrapper_total,
                                             const Cardinality wrapper_rows) {
        auto* rel = makeNode(RelOptInfo);
        rel->reloptkind = RELOPT_BASEREL;
        rel->relid = relid;

        auto* child_target = create_empty_pathtarget();
        add_column_to_pathtarget(
            child_target,
            reinterpret_cast<Expr*>(makeVar(relid, 1, INT4OID, -1, InvalidOid, 0)),
            0);
        auto* wrapper_target = copy_pathtarget(child_target);
        for (Node* expression : postgres_list<Node>(wrapper_delta)) {
            add_column_to_pathtarget(wrapper_target, reinterpret_cast<Expr*>(expression), 0);
        }

        auto* scan = makeNode(CustomPath);
        scan->path.pathtype = T_CustomScan;
        scan->path.parent = rel;
        scan->path.pathtarget = child_target;
        scan->path.param_info = nullptr;
        scan->path.parallel_aware = false;
        scan->path.parallel_safe = false;
        scan->path.parallel_workers = 0;
        scan->path.rows = wrapper_rows;
        scan->path.startup_cost = 1.0;
        scan->path.total_cost = 2.0;
        scan->path.pathkeys = NIL;
        scan->methods = bioseq_column_scan_path_methods();

        auto* projection = makeNode(ProjectionPath);
        projection->path.pathtype = T_Result;
        projection->path.parent = rel;
        projection->path.pathtarget = wrapper_target;
        projection->path.param_info = nullptr;
        projection->path.parallel_aware = false;
        projection->path.parallel_safe = false;
        projection->path.parallel_workers = 0;
        projection->path.rows = wrapper_rows;
        projection->path.startup_cost = wrapper_startup;
        projection->path.total_cost = wrapper_total;
        projection->path.pathkeys = NIL;
        projection->subpath = &scan->path;
        projection->dummypp = false;
        rel->pathlist = list_make1(projection);
        return batch_join_projection_guard_fixture{rel, scan, projection};
    }

    /** @brief Checks whether a path target contains an expression equal to a candidate. */
    [[nodiscard]] bool path_target_contains_expression(const PathTarget* target,
                                                       const Expr* expression) noexcept {
        if (target == nullptr || expression == nullptr) {
            return false;
        }
        for (Node* existing : postgres_list<Node>(target->exprs)) {
            if (equal(existing, expression)) {
                return true;
            }
        }
        return false;
    }

    /** @brief Collects target Vars needed below a parent-owned projection. */
    struct batch_join_underlying_target_var_collector {
        PathTarget* destination = nullptr;
        Index target_relid = 0;
        AttrNumber target_seq_attno = InvalidAttrNumber;
        bool invalid = false;
    };

    /** @brief Validates and copies safe target Vars into a locator child target. */
    struct batch_join_underlying_target_var_visitor {
        /** @brief Processes one Var dependency beneath a wrapper expression. */
        [[nodiscard]] pg_walk_step
        operator()(pg_expression_walk_cursor,
                   pg_node_view node,
                   batch_join_underlying_target_var_collector& collector) const {
            Var* var = node.try_cast<Var>();
            if (var == nullptr) {
                return pg_walk_step::descend;
            }
            if (var->varno != collector.target_relid) {
                return pg_walk_step::skip_subtree;
            }
            const bool exact_sequence_var = var->varattno == collector.target_seq_attno &&
                                            var->varattno > 0 && var->varlevelsup == 0 &&
                                            bms_is_empty(var->varnullingrels);
            if (!exact_sequence_var &&
                !batch_join_is_safe_plain_target_var(reinterpret_cast<Node*>(var),
                                                     collector.target_relid,
                                                     collector.target_seq_attno)) {
                collector.invalid = true;
                return pg_walk_step::stop;
            }
            /* The sequence Var is installed explicitly below.  Preserve every other
             * safe target Var, including a helper's load-bearing argument (normally
             * t.pk) hidden below a PlaceHolderVar/compound wrapper.  Skipping the
             * whole wrapper because it contains a bioseq helper must not erase that
             * base-column dependency from the locator child and combined scan tlist. */
            if (!exact_sequence_var && collector.destination != nullptr &&
                !path_target_contains_expression(collector.destination,
                                                 reinterpret_cast<Expr*>(var))) {
                add_column_to_pathtarget(
                    collector.destination, reinterpret_cast<Expr*>(copyObject(var)), 0);
            }
            return pg_walk_step::skip_subtree;
        }
    };

    /** @brief Appends all safe underlying target Vars required by a wrapper target. */
    [[nodiscard]] bool batch_join_append_underlying_target_vars(PathTarget* destination,
                                                                const PathTarget* source,
                                                                const Index target_relid,
                                                                const AttrNumber target_seq_attno) {
        if (destination == nullptr || source == nullptr ||
            !bioseq_is_concrete_rte_varno(target_relid) || target_seq_attno <= 0) {
            return false;
        }
        batch_join_underlying_target_var_collector collector{
            destination, target_relid, target_seq_attno, false};
        batch_join_underlying_target_var_visitor visitor;
        for (Node* expression : postgres_list<Node>(source->exprs)) {
            if (expression == nullptr) {
                continue;
            }
            walk_pg_expression_including_root(expression, collector, visitor);
            if (collector.invalid) {
                return false;
            }
        }
        return true;
    }

} // export custom-scan partition declarations

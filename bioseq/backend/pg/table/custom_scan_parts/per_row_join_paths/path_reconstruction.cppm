/**
 * @file
 * @brief Rebuilds an inner join after moving mmsearch execution to its parent scan.
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

export module bioseq.postgres.table:custom_scan_per_row_join_paths_path_reconstruction;

import std;
import bioseq.postgres.support;

import :custom_scan_join_ownership_carrier_lineage_contracts;
import :custom_scan_join_ownership_descendant_path_contracts;
import :custom_scan_slice_rewrite_path_tree;
export import :custom_scan_per_row_join_paths_reconstruction_contracts;

using namespace bioseq::postgres::support;

export {
    /** @brief Captures a path node tag and custom-path name for rejection diagnostics. */
    void per_row_record_path_identity(Path* path, int& tag, const char*& custom_name) noexcept {
        tag = path == nullptr ? -1 : static_cast<int>(nodeTag(path));
        custom_name = "none";
        if (path == nullptr || !IsA(path, CustomPath))
            return;
        auto* custom = reinterpret_cast<CustomPath*>(path);
        if (custom->methods != nullptr && custom->methods->CustomName != nullptr) {
            custom_name = custom->methods->CustomName;
        }
    }

    /**
     * @brief Rebuilds an inner join whose mmsearch predicate will run in the parent scan.
     *
     * Replace children that still execute the predicate or lack a Bioseq sequence
     * source. Preserve required columns and external parameters, then rebuild the
     * original join kind with usable merge or hash conditions.
     *
     * @return The replacement path, or null if these requirements cannot be met.
     */
    [[nodiscard]] Path*
    per_row_reconstruct_inner_join_path(PlannerInfo* root,
                                        RelOptInfo* joinrel,
                                        Path* incumbent,
                                        RestrictInfo* owned,
                                        JoinPathExtraData const& original_extra,
                                        PathTarget* required_target,
                                        List* required_vars,
                                        const Index target_relid,
                                        per_row_reconstruction_rejections& rejects) {
        ++rejects.incumbents;
        if (root == nullptr || joinrel == nullptr || incumbent == nullptr || owned == nullptr ||
            required_target == nullptr || required_vars == nullptr ||
            !bioseq_is_concrete_rte_varno(target_relid)) {
            ++rejects.invalid_join;
            return nullptr;
        }
        per_row_record_path_identity(
            incumbent, rejects.last_incumbent_tag, rejects.last_incumbent_custom_name);
        JoinPath* join = nullptr;
        if (IsA(incumbent, NestPath)) {
            join = &reinterpret_cast<NestPath*>(incumbent)->jpath;
        } else if (IsA(incumbent, MergePath)) {
            join = &reinterpret_cast<MergePath*>(incumbent)->jpath;
        } else if (IsA(incumbent, HashPath)) {
            join = &reinterpret_cast<HashPath*>(incumbent)->jpath;
        } else {
            ++rejects.unsupported_path;
            return nullptr;
        }
        if (join->jointype != JOIN_INNER || join->outerjoinpath == nullptr ||
            join->innerjoinpath == nullptr ||
            batch_join_path_contains_parallel_execution(incumbent)) {
            ++rejects.invalid_join;
            return nullptr;
        }
        per_row_record_path_identity(
            join->outerjoinpath, rejects.last_outer_tag, rejects.last_outer_custom_name);
        per_row_record_path_identity(
            join->innerjoinpath, rejects.last_inner_tag, rejects.last_inner_custom_name);
        Path* outer_path = join->outerjoinpath;
        Path* inner_path = join->innerjoinpath;
        const bool join_owns = list_member_ptr(join->joinrestrictinfo, owned);
        const bool outer_owns = per_row_descendant_owns_restriction(outer_path, owned);
        const bool inner_owns = per_row_descendant_owns_restriction(inner_path, owned);
        if (IsA(incumbent, NestPath)) {
            if (!join_owns && !outer_owns && !inner_owns) {
                ++rejects.missing_owned_identity;
                return nullptr;
            }
        } else if (!join_owns) {
            /* This merge or hash join must own the predicate at the join node. */
            ++rejects.missing_owned_identity;
            return nullptr;
        }
        const bool outer_is_target =
            outer_path->parent != nullptr &&
            bms_is_member(static_cast<int>(target_relid), outer_path->parent->relids);
        const bool inner_is_target =
            inner_path->parent != nullptr &&
            bms_is_member(static_cast<int>(target_relid), inner_path->parent->relids);
        const bool outer_needs_source =
            outer_is_target && !slice_project_path_tree_has_bioseq_scan(outer_path);
        const bool inner_needs_source =
            inner_is_target && !slice_project_path_tree_has_bioseq_scan(inner_path);
        /* A nested loop may have pushed mmsearch into a parameterized child.
         * Merge and hash joins can still need a child replacement to provide
         * sequence data. Keep their remaining merge or hash conditions. */
        if ((outer_owns || inner_owns) && !IsA(incumbent, NestPath)) {
            ++rejects.invalid_join;
            return nullptr;
        }
        if (outer_owns || inner_owns || outer_needs_source || inner_needs_source) {
            /* Choose replacement children that leave this mmsearch call to the
             * parent. Keep unrelated LATERAL parameters and check their dependencies
             * against the original join below. */
            if (outer_owns || outer_needs_source)
                outer_path = per_row_path_without_owned_qual(
                    join->outerjoinpath->parent, owned, outer_is_target);
            if (inner_owns || inner_needs_source)
                inner_path = per_row_path_without_owned_qual(
                    join->innerjoinpath->parent, owned, inner_is_target);
            if (outer_path == nullptr || inner_path == nullptr) {
                ++rejects.replacement_missing;
                return nullptr;
            }
        }
        outer_path = per_row_project_required_child_vars(root, outer_path, required_vars);
        inner_path = per_row_project_required_child_vars(root, inner_path, required_vars);
        if (outer_path == nullptr || inner_path == nullptr ||
            per_row_descendant_owns_restriction(outer_path, owned) ||
            per_row_descendant_owns_restriction(inner_path, owned)) {
            ++rejects.projection_or_leak;
            return nullptr;
        }
        List* filtered_restrictions = per_row_without_restriction(join->joinrestrictinfo, owned);
        JoinPathExtraData filtered_extra = original_extra;
        filtered_extra.inner_unique = join->inner_unique;
        filtered_extra.restrictlist =
            per_row_without_restriction(original_extra.restrictlist, owned);
        filtered_extra.mergeclause_list =
            per_row_without_restriction(original_extra.mergeclause_list, owned);
        JoinCostWorkspace workspace{};
        if (IsA(incumbent, NestPath)) {
            Relids calculated_required_outer =
                calc_nestloop_required_outer(outer_path->parent->relids,
                                             PATH_REQ_OUTER(outer_path),
                                             inner_path->parent->relids,
                                             PATH_REQ_OUTER(inner_path));
            Relids required_outer = bms_copy(PATH_REQ_OUTER(&join->path));
            /* Removing mmsearch may remove a dependency between the join inputs.
             * Keep the original external parameter set for rescans and reject
             * replacement paths that need an additional external parameter. */
            if (!bms_is_subset(calculated_required_outer, required_outer)) {
                ++rejects.new_external_parameter;
                bms_free(calculated_required_outer);
                bms_free(required_outer);
                return nullptr;
            }
            bms_free(calculated_required_outer);
            List* reconstructed_pathkeys =
                pathkeys_contained_in(join->path.pathkeys, outer_path->pathkeys)
                    ? join->path.pathkeys
                    : outer_path->pathkeys;
            workspace = initial_cost_serial_nested_loop(
                root, JOIN_INNER, outer_path, inner_path, &filtered_extra);
            Path* result = reinterpret_cast<Path*>(create_nestloop_path(root,
                                                                        joinrel,
                                                                        JOIN_INNER,
                                                                        &workspace,
                                                                        &filtered_extra,
                                                                        outer_path,
                                                                        inner_path,
                                                                        filtered_restrictions,
                                                                        reconstructed_pathkeys,
                                                                        required_outer));
            if (result != nullptr)
                ++rejects.reconstructed;
            return result;
        }
        Relids required_outer = calc_non_nestloop_required_outer(outer_path, inner_path);
        if (!bms_equal(required_outer, PATH_REQ_OUTER(&join->path))) {
            ++rejects.new_external_parameter;
            return nullptr;
        }
        if (IsA(incumbent, MergePath)) {
            auto* merge = reinterpret_cast<MergePath*>(incumbent);
            List* mergeclauses = per_row_without_restriction(merge->path_mergeclauses, owned);
            if (mergeclauses == NIL) {
                ++rejects.unusable_algorithm_clause;
                return nullptr;
            }
            /* A replacement BioseqScan may lose the order supplied by an index
             * scan. Recover the original merge input order, check it covers the
             * remaining merge clauses, and request a sort wherever the replacement
             * no longer supplies that order. */
            List* effective_outer_pathkeys =
                merge->outersortkeys != NIL ? merge->outersortkeys : join->outerjoinpath->pathkeys;
            List* effective_inner_pathkeys =
                merge->innersortkeys != NIL ? merge->innersortkeys : join->innerjoinpath->pathkeys;
            if (effective_outer_pathkeys == NIL || effective_inner_pathkeys == NIL) {
                ++rejects.unusable_algorithm_clause;
                return nullptr;
            }
            List* ordered_mergeclauses =
                find_mergeclauses_for_outer_pathkeys(root, effective_outer_pathkeys, mergeclauses);
            List* required_inner_pathkeys =
                ordered_mergeclauses == NIL
                    ? NIL
                    : make_inner_pathkeys_for_merge(
                          root, ordered_mergeclauses, effective_outer_pathkeys);
            if (ordered_mergeclauses == NIL ||
                list_length(ordered_mergeclauses) != list_length(mergeclauses) ||
                required_inner_pathkeys == NIL ||
                !pathkeys_contained_in(required_inner_pathkeys, effective_inner_pathkeys)) {
                ++rejects.unusable_algorithm_clause;
                return nullptr;
            }
            List* outer_sortkeys =
                pathkeys_contained_in(effective_outer_pathkeys, outer_path->pathkeys)
                    ? NIL
                    : effective_outer_pathkeys;
            List* inner_sortkeys =
                pathkeys_contained_in(effective_inner_pathkeys, inner_path->pathkeys)
                    ? NIL
                    : effective_inner_pathkeys;
            filtered_extra.mergeclause_list = ordered_mergeclauses;
#if PG_VERSION_NUM >= 180000
            // Use a full sort when needed. The replacement may not preserve the
            // sorted prefix of the original path.
            constexpr int outer_presorted_keys = 0;
#endif
            initial_cost_mergejoin(root,
                                   &workspace,
                                   JOIN_INNER,
                                   ordered_mergeclauses,
                                   outer_path,
                                   inner_path,
                                   outer_sortkeys,
                                   inner_sortkeys,
#if PG_VERSION_NUM >= 180000
                                   outer_presorted_keys,
#endif
                                   &filtered_extra);
            Path* result = reinterpret_cast<Path*>(create_mergejoin_path(root,
                                                                         joinrel,
                                                                         JOIN_INNER,
                                                                         &workspace,
                                                                         &filtered_extra,
                                                                         outer_path,
                                                                         inner_path,
                                                                         filtered_restrictions,
                                                                         join->path.pathkeys,
                                                                         required_outer,
                                                                         ordered_mergeclauses,
                                                                         outer_sortkeys,
                                                                         inner_sortkeys
#if PG_VERSION_NUM >= 180000
                                                                         , outer_presorted_keys
#endif
                                                                         ));
            if (result != nullptr)
                ++rejects.reconstructed;
            return result;
        }
        auto* hash = reinterpret_cast<HashPath*>(incumbent);
        List* hashclauses = per_row_without_restriction(hash->path_hashclauses, owned);
        if (hashclauses == NIL) {
            ++rejects.unusable_algorithm_clause;
            return nullptr;
        }
        const bool parallel_hash = inner_path->parallel_aware;
        initial_cost_hashjoin(root,
                              &workspace,
                              JOIN_INNER,
                              hashclauses,
                              outer_path,
                              inner_path,
                              &filtered_extra,
                              parallel_hash);
        Path* result = reinterpret_cast<Path*>(create_hashjoin_path(root,
                                                                    joinrel,
                                                                    JOIN_INNER,
                                                                    &workspace,
                                                                    &filtered_extra,
                                                                    outer_path,
                                                                    inner_path,
                                                                    parallel_hash,
                                                                    filtered_restrictions,
                                                                    required_outer,
                                                                    hashclauses));
        if (result != nullptr)
            ++rejects.reconstructed;
        return result;
    }

    /** @brief Returns false at a named admission check. The stage argument is currently unused. */
    [[nodiscard]] bool per_row_decline_transformed_path(const char* stage) {
        (void)stage;
        return false;
    }

} // export custom-scan partition declarations

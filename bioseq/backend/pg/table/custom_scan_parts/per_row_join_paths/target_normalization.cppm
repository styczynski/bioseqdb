/**
 * @file
 * @brief Normalizes authoritative targets and private path spines.
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

export module bioseq.postgres.table:custom_scan_per_row_join_paths_target_normalization;

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

import :custom_scan_join_ownership;

import :custom_scan_per_row_join_paths_carrier_analysis;

using namespace bioseq::postgres::support;

export {
    /**
     * @brief Builds a child target containing available helpers and required carrier Vars.
     * @param changed Set when the returned target differs from the source target.
     * @param invalid Set when helper or carrier metadata cannot be preserved.
     */
    [[nodiscard]] PathTarget* per_row_normalized_path_target(PlannerInfo* root,
                                                             Path* path,
                                                             List* available,
                                                             List* candidate_helpers,
                                                             List* required_vars,
                                                             bool& changed,
                                                             bool& invalid) {
        changed = false;
        if (root == nullptr || path == nullptr || path->parent == nullptr ||
            path->parent->relids == nullptr || path->pathtarget == nullptr) {
            invalid = true;
            return nullptr;
        }
        List* expressions = NIL;
        std::vector<Index> sortgrouprefs;
        sortgrouprefs.reserve(static_cast<std::size_t>(list_length(path->pathtarget->exprs) +
                                                       list_length(required_vars)));
        std::vector<Index> source_sortgrouprefs;
        if (!per_row_authoritative_sortgrouprefs(
                root, path->pathtarget->exprs, source_sortgrouprefs)) {
            invalid = true;
            return nullptr;
        }
        std::size_t source_index = 0u;
        for (Node* expression : postgres_list<Node>(path->pathtarget->exprs)) {
            bool saw_helper = false;
            const bool supported =
                per_row_expression_uses_available_helpers(expression, available, saw_helper);
            const Index sortgroupref = source_sortgrouprefs[source_index++];
            if (!supported) {
                /* Pathkeys were audited against the actually available helpers
                 * before target normalization.  A current-owner helper may therefore
                 * be removed from this private child even when its public TLE carries
                 * a sortgroupref; the exact outer owner target retains that ref. */
                bool saw_candidate_helper = false;
                const bool covered_by_current_owner = per_row_expression_uses_available_helpers(
                    expression, candidate_helpers, saw_candidate_helper);
                if (!saw_helper || !saw_candidate_helper || !covered_by_current_owner) {
                    invalid = true;
                    return nullptr;
                }
                changed = true;
                continue;
            }
            expressions = lappend(expressions, copyObject(expression));
            sortgrouprefs.push_back(sortgroupref);
        }
        for (Node* expression : postgres_list<Node>(required_vars)) {
            Var* var = try_cast_pg_node<Var>(expression);
            const bool row_identity = per_row_is_exact_row_identity_var(var);
            if (var == nullptr || !bioseq_is_concrete_rte_varno(var->varno) ||
                (var->varattno <= 0 && !row_identity) || var->varlevelsup != 0 ||
                !bms_is_empty(var->varnullingrels)) {
                invalid = true;
                return nullptr;
            }
            if (!bms_is_member(static_cast<int>(var->varno), path->parent->relids) ||
                per_row_expression_list_contains(expressions, expression)) {
                continue;
            }
            expressions = lappend(expressions, copyObject(expression));
            sortgrouprefs.push_back(0);
            changed = true;
        }
        if (sortgrouprefs.size() != static_cast<std::size_t>(list_length(expressions))) {
            invalid = true;
            return nullptr;
        }
        PathTarget* target = create_empty_pathtarget();
        target->exprs = expressions;
        // Avoid allocating the optional PathTarget array when every reference is zero.
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
        for (Node* expression : postgres_list<Node>(target->exprs)) {
            bool saw_helper = false;
            if (!per_row_expression_uses_available_helpers(expression, available, saw_helper)) {
                invalid = true;
                return nullptr;
            }
        }
        return target;
    }

    /** @brief Checks that append children expose the parent's output types in the same order. */
    [[nodiscard]] bool per_row_append_targets_are_ordinally_compatible(PathTarget* parent,
                                                                       List* children) {
        if (parent == nullptr || children == NIL)
            return false;
        const int columns = list_length(parent->exprs);
        for (Path* child : postgres_list<Path>(children)) {
            if (child == nullptr || child->pathtarget == nullptr ||
                list_length(child->pathtarget->exprs) != columns)
                return false;
            for (int index = 0; index < columns; ++index) {
                Node* parent_expression = reinterpret_cast<Node*>(list_nth(parent->exprs, index));
                Node* child_expression =
                    reinterpret_cast<Node*>(list_nth(child->pathtarget->exprs, index));
                if (parent_expression == nullptr || child_expression == nullptr ||
                    exprType(parent_expression) != exprType(child_expression) ||
                    exprTypmod(parent_expression) != exprTypmod(child_expression) ||
                    exprCollation(parent_expression) != exprCollation(child_expression))
                    return false;
            }
        }
        return true;
    }

    /** @brief Compares target expressions and their authoritative sort-group references. */
    [[nodiscard]] bool
    per_row_authoritative_targets_match(PlannerInfo* root, PathTarget* left, PathTarget* right) {
        if (root == nullptr || left == nullptr || right == nullptr ||
            list_length(left->exprs) != list_length(right->exprs) ||
            !equal(left->exprs, right->exprs))
            return false;
        std::vector<Index> left_sortgrouprefs;
        std::vector<Index> right_sortgrouprefs;
        return per_row_authoritative_sortgrouprefs(root, left->exprs, left_sortgrouprefs) &&
               per_row_authoritative_sortgrouprefs(root, right->exprs, right_sortgrouprefs) &&
               left_sortgrouprefs == right_sortgrouprefs;
    }

    /**
     * @brief Normalizes every node in a private path spine to its available helper carriers.
     * @return True when all metadata and wrapper contracts remain valid after normalization.
     */
    [[nodiscard]] bool per_row_normalize_private_path_spine(PlannerInfo* root,
                                                            Path* path,
                                                            List* candidate_helpers,
                                                            List* validated_inherited_helpers,
                                                            List* required_vars) {
        if (root == nullptr || path == nullptr)
            return false;
        check_stack_depth();
        List* available = NIL;
        if (!per_row_collect_available_path_helpers(
                path, candidate_helpers, validated_inherited_helpers, available) ||
            !per_row_pathkeys_use_available_helpers(path, path->pathkeys, available) ||
            (path->param_info != nullptr &&
             !per_row_restrictinfo_list_has_no_helper(path->param_info->ppi_clauses))) {
            return false;
        }
        bool target_changed = false;
        bool target_invalid = false;
        PathTarget* normalized_target = per_row_normalized_path_target(root,
                                                                       path,
                                                                       available,
                                                                       candidate_helpers,
                                                                       required_vars,
                                                                       target_changed,
                                                                       target_invalid);
        if (target_invalid || normalized_target == nullptr)
            return false;

        if (IsA(path, CustomPath)) {
            auto* custom = reinterpret_cast<CustomPath*>(path);
            if (custom->methods != bioseq_column_scan_path_methods() &&
                custom->methods != bioseq_batch_join_path_methods() &&
                custom->methods != bioseq_per_row_join_path_methods() &&
                custom->methods != &bioseq_slice_project_path_methods()) {
                return false;
            }
            if (!per_row_restrictinfo_list_has_no_helper(custom->custom_restrictinfo) ||
                !per_row_path_metadata_has_no_helper(
                    reinterpret_cast<Node*>(custom->custom_private)))
                return false;
            path->pathtarget = normalized_target;
            for (Path* child : postgres_list<Path>(custom->custom_paths)) {
                if (!per_row_normalize_private_path_spine(
                        root, child, candidate_helpers, validated_inherited_helpers, required_vars))
                    return false;
            }
            return true;
        }

        JoinPath* join = nullptr;
        if (IsA(path, NestPath)) {
            join = &reinterpret_cast<NestPath*>(path)->jpath;
        } else if (IsA(path, MergePath)) {
            join = &reinterpret_cast<MergePath*>(path)->jpath;
        } else if (IsA(path, HashPath)) {
            join = &reinterpret_cast<HashPath*>(path)->jpath;
        }
        if (join != nullptr) {
            if (!per_row_restrictinfo_list_has_no_helper(join->joinrestrictinfo))
                return false;
            if (IsA(path, MergePath)) {
                auto* merge = reinterpret_cast<MergePath*>(path);
                List* outer_available = NIL;
                List* inner_available = NIL;
                if (!per_row_restrictinfo_list_has_no_helper(merge->path_mergeclauses) ||
                    !per_row_collect_available_path_helpers(join->outerjoinpath,
                                                            candidate_helpers,
                                                            validated_inherited_helpers,
                                                            outer_available) ||
                    !per_row_collect_available_path_helpers(join->innerjoinpath,
                                                            candidate_helpers,
                                                            validated_inherited_helpers,
                                                            inner_available) ||
                    !per_row_pathkeys_use_available_helpers(
                        join->outerjoinpath, merge->outersortkeys, outer_available) ||
                    !per_row_pathkeys_use_available_helpers(
                        join->innerjoinpath, merge->innersortkeys, inner_available))
                    return false;
            } else if (IsA(path, HashPath) &&
                       !per_row_restrictinfo_list_has_no_helper(
                           reinterpret_cast<HashPath*>(path)->path_hashclauses)) {
                return false;
            }
            path->pathtarget = normalized_target;
            return per_row_normalize_private_path_spine(root,
                                                        join->outerjoinpath,
                                                        candidate_helpers,
                                                        validated_inherited_helpers,
                                                        required_vars) &&
                   per_row_normalize_private_path_spine(root,
                                                        join->innerjoinpath,
                                                        candidate_helpers,
                                                        validated_inherited_helpers,
                                                        required_vars);
        }

        Path* unary = nullptr;
        bool target_preserving = false;
        if (IsA(path, ProjectionPath)) {
            auto* projection = reinterpret_cast<ProjectionPath*>(path);
            unary = projection->subpath;
            if (target_changed)
                projection->dummypp = false;
        } else if (IsA(path, ProjectSetPath)) {
            unary = reinterpret_cast<ProjectSetPath*>(path)->subpath;
        } else if (IsA(path, SortPath)) {
            unary = reinterpret_cast<SortPath*>(path)->subpath;
            target_preserving = true;
        } else if (IsA(path, IncrementalSortPath)) {
            unary = reinterpret_cast<IncrementalSortPath*>(path)->spath.subpath;
            target_preserving = true;
        } else if (IsA(path, MaterialPath)) {
            unary = reinterpret_cast<MaterialPath*>(path)->subpath;
            target_preserving = true;
        } else if (IsA(path, MemoizePath)) {
            auto* memoize = reinterpret_cast<MemoizePath*>(path);
            if (!per_row_path_metadata_has_no_helper(reinterpret_cast<Node*>(memoize->param_exprs)))
                return false;
            unary = memoize->subpath;
            target_preserving = true;
        } else if (IsA(path, UniquePath)) {
            auto* unique = reinterpret_cast<UniquePath*>(path);
            if (!per_row_path_metadata_has_no_helper(reinterpret_cast<Node*>(unique->uniq_exprs)))
                return false;
            unary = unique->subpath;
            target_preserving = true;
        } else if (IsA(path, GatherPath)) {
            unary = reinterpret_cast<GatherPath*>(path)->subpath;
            target_preserving = true;
        } else if (IsA(path, GatherMergePath)) {
            unary = reinterpret_cast<GatherMergePath*>(path)->subpath;
            target_preserving = true;
        }
        if (unary != nullptr) {
            path->pathtarget = normalized_target;
            if (!per_row_normalize_private_path_spine(
                    root, unary, candidate_helpers, validated_inherited_helpers, required_vars))
                return false;
            return !target_preserving ||
                   per_row_authoritative_targets_match(root, path->pathtarget, unary->pathtarget);
        }

        List* children = NIL;
        if (IsA(path, AppendPath)) {
            children = reinterpret_cast<AppendPath*>(path)->subpaths;
        } else if (IsA(path, MergeAppendPath)) {
            children = reinterpret_cast<MergeAppendPath*>(path)->subpaths;
        }
        if (children != NIL) {
            path->pathtarget = normalized_target;
            for (Path* child : postgres_list<Path>(children)) {
                if (!per_row_normalize_private_path_spine(
                        root, child, candidate_helpers, validated_inherited_helpers, required_vars))
                    return false;
            }
            return per_row_append_targets_are_ordinally_compatible(path->pathtarget, children);
        }

        /* Ordinary base scan paths are immutable leaves in the existing COW
         * cloner.  They may be shared only when normalization is a no-op; direct
         * BioseqScan leaves were handled above and are private CustomPaths. */
        if (path->parent != nullptr && (path->parent->reloptkind == RELOPT_BASEREL ||
                                        path->parent->reloptkind == RELOPT_OTHER_MEMBER_REL)) {
            return !target_changed;
        }
        return false;
    }

    /** @brief Returns methods for the unsupported-wrapper integration probe path. */
    [[nodiscard]] CustomPathMethods const&
    match_helper_wrapper_probe_unknown_path_methods() noexcept {
        static const CustomPathMethods methods{
            .CustomName = "BioseqMatchHelperWrapperProbeUnknown",
            .PlanCustomPath = nullptr,
            .ReparameterizeCustomPathByChild = nullptr,
        };
        return methods;
    }

    /** @brief Initializes common Path fields for a wrapper integration probe. */
    void match_helper_wrapper_probe_init_path(Path* path,
                                              NodeTag plan_tag,
                                              RelOptInfo* parent,
                                              PathTarget* target) noexcept {
        Assert(path != nullptr);
        path->pathtype = plan_tag;
        path->parent = parent;
        path->pathtarget = target;
        path->param_info = nullptr;
        path->parallel_aware = false;
        path->parallel_safe = false;
        path->parallel_workers = 0;
        path->rows = 1.0;
        path->startup_cost = 1.0;
        path->total_cost = 2.0;
        path->pathkeys = NIL;
    }

    /** @brief Allocates a minimal planner relation for wrapper integration probes. */
    [[nodiscard]] RelOptInfo*
    match_helper_wrapper_probe_rel(RelOptKind kind, Index relid, Relids relids) {
        auto* rel = makeNode(RelOptInfo);
        rel->reloptkind = kind;
        rel->relid = relid;
        rel->relids = relids;
        rel->rows = 1.0;
        rel->reltarget = create_empty_pathtarget();
        return rel;
    }

    /** @brief Accumulates per-stage rejection and reconstruction counts for path admission. */
    struct per_row_reconstruction_rejections {
        std::size_t incumbents = 0u;
        std::size_t unsupported_path = 0u;
        std::size_t invalid_join = 0u;
        std::size_t missing_owned_identity = 0u;
        std::size_t replacement_missing = 0u;
        std::size_t projection_or_leak = 0u;
        std::size_t new_external_parameter = 0u;
        std::size_t unusable_algorithm_clause = 0u;
        std::size_t reconstructed = 0u;
        std::size_t cloned = 0u;
        std::size_t tag_missing = 0u;
        std::size_t source_projection_missing = 0u;
        std::size_t nonfinite_cost = 0u;
        std::size_t owners_created = 0u;
        int last_incumbent_tag = -1;
        int last_outer_tag = -1;
        int last_inner_tag = -1;
        const char* last_incumbent_custom_name = "none";
        const char* last_outer_custom_name = "none";
        const char* last_inner_custom_name = "none";
    };

} // export custom-scan partition declarations

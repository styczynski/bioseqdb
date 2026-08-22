/**
 * @file
 * @brief Builds and installs aggregate-input SliceProject owners.
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

export module bioseq.postgres.table:custom_scan_slice_rewrite_aggregate_rewrite;

import std;
import bioseq.postgres.support;

import :custom_scan_oid_expression_support_oid_catalog;
import :custom_scan_slice_path_codec_contract;
import :custom_scan_slice_path_carrier_lineage;
import :custom_scan_slice_path_plan_construction;
import :custom_scan_slice_rewrite_path_cloning;
import :custom_scan_slice_rewrite_owner_admission;

using namespace bioseq::postgres::support;

export {
    /* Build the lower SliceProject that materializes aggregate arguments lazily. */
    [[nodiscard]] inline Path* slice_project_build_aggregate_input_owner(
        PlannerInfo* root,
        AggPath* aggregate,
        slice_project_aggregate_input_validation const& validation) {
        Path* source = aggregate == nullptr ? nullptr : aggregate->subpath;
        Relids source_relids = slice_project_aggregate_source_relids(source);
        if (root == nullptr || source == nullptr || source->parent == nullptr ||
            source->param_info != nullptr || validation.candidates == NIL ||
            validation.oids == nullptr || source_relids == nullptr ||
            !bms_equal(source_relids, validation.available_relids) ||
            !slice_project_path_tree_has_bioseq_scan(source))
            return nullptr;

        PathTarget* lower_target = copy_pathtarget(source->pathtarget);
        /* Aggref arguments remain owned by the parent aggregate PathTarget.  Keep
         * only structural equality across the setrefs boundary: neither later
         * child-target processing nor the SliceProject codec may share those exact
         * mutable Node objects with the parent Aggref. */
        List* owned_candidates = static_cast<List*>(copyObject(validation.candidates));
        List* owned_inputs = static_cast<List*>(copyObject(validation.input_expressions));
        List* required = NIL;
        constexpr int pull_flags =
            PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
        for (Node* input : postgres_list<Node>(owned_inputs)) {
            required = list_concat(required, pull_var_clause(input, pull_flags));
        }
        /* The copied source target retains ordered carriers (notably the resjunk
         * id Var) and all existing sortgrouprefs.  Append only any missing raw Vars
         * on private copies before adding the exact lazy argument expressions. */
        add_new_columns_to_pathtarget(lower_target, static_cast<List*>(copyObject(required)));
        add_new_columns_to_pathtarget(lower_target, owned_candidates);
        set_pathtarget_cost_width(root, lower_target);
        slice_project_ir_builder builder{
            .oids = validation.oids,
            /* Aggregate inputs are internal executor values, not client output. */
            .command_type = CMD_UNKNOWN,
        };
        if (!slice_project_discover_target(lower_target, builder) ||
            !is_parallel_safe(root, reinterpret_cast<Node*>(lower_target->exprs)))
            return nullptr;

        PathTarget* support_target = copy_pathtarget(source->pathtarget);
        add_new_columns_to_pathtarget(support_target, static_cast<List*>(copyObject(required)));
        set_pathtarget_cost_width(root, support_target);

        Path* child = nullptr;
        if (slice_project_is_bioseq_scan_path(source)) {
            child = slice_project_tag_source_child(source, support_target, builder);
        } else {
            bool tagged_source = false;
            Path* private_child = slice_project_clone_mutable_path_spine(source);
            private_child =
                slice_project_tag_owned_path_tree(private_child, builder, tagged_source);
            if (!tagged_source)
                return nullptr;
            child = reinterpret_cast<Path*>(
                create_projection_path(root, private_child->parent, private_child, support_target));
        }
        if (child == nullptr || child->param_info != nullptr ||
            !pathkeys_contained_in(source->pathkeys, child->pathkeys) ||
            !pathkeys_contained_in(child->pathkeys, source->pathkeys)) {
            return nullptr;
        }

        const double rows = std::max(0.0, child->rows);
        const double metadata_per_row =
            cpu_operator_cost *
            static_cast<double>(std::min<std::uint64_t>(
                builder.piece_boundaries, std::numeric_limits<std::uint32_t>::max()));
        const double boundary_per_row =
            cpu_operator_cost *
            static_cast<double>(std::min<std::uint64_t>(
                builder.planned_symbols, std::numeric_limits<std::uint32_t>::max()));
        auto* owner = makeNode(CustomPath);
        owner->path.pathtype = T_CustomScan;
        owner->path.parent = source->parent;
        owner->path.pathtarget = lower_target;
        owner->path.param_info = child->param_info;
        owner->path.parallel_aware = child->parallel_aware;
        owner->path.parallel_safe = child->parallel_safe;
        owner->path.parallel_workers = child->parallel_workers;
        owner->path.rows = child->rows;
        /* AggPath already charges evaluation of its arguments.  This wrapper only
         * adds source-owner metadata overhead; charging the argument twice would
         * distort serial/partial aggregate selection. */
        owner->path.startup_cost = child->startup_cost;
        owner->path.total_cost = child->total_cost + rows * (metadata_per_row + boundary_per_row);
        owner->path.pathkeys = child->pathkeys;
        owner->flags = CUSTOMPATH_SUPPORT_PROJECTION;
        owner->custom_paths = list_make1(child);
        owner->custom_private = list_make5(slice_project_header(builder),
                                           copyObject(builder.expressions),
                                           copyObject(builder.nodes),
                                           copyObject(builder.records),
                                           copyObject(builder.output_bindings));
        owner->custom_restrictinfo = NIL;
        owner->methods = &bioseq_slice_project_path_methods();

        return reinterpret_cast<Path*>(owner);
    }

    struct slice_project_aggregate_path_rewrite {
        Path* path = nullptr;
        bool changed = false;
        bool unsafe = false;
    };

    /* Detect an eager semantic alternative that this AggPath rewrite cannot own. */
    [[nodiscard]] inline bool
    slice_project_non_aggregate_path_has_recipe(Path* path, const bioseq_stub_oids* oids) {
        if (path == nullptr || oids == nullptr)
            return false;
        if (path->pathtarget != nullptr && path->pathtarget->exprs != NIL &&
            slice_project_aggregate_expression_has_recipe(
                reinterpret_cast<Node*>(path->pathtarget->exprs), oids)) {
            return true;
        }
        List* quals = NIL;
        if (IsA(path, GroupPath)) {
            quals = reinterpret_cast<GroupPath*>(path)->qual;
        } else if (IsA(path, GroupingSetsPath)) {
            quals = reinterpret_cast<GroupingSetsPath*>(path)->qual;
        } else if (IsA(path, MinMaxAggPath)) {
            quals = reinterpret_cast<MinMaxAggPath*>(path)->quals;
        }
        return quals != NIL &&
               slice_project_aggregate_expression_has_recipe(reinterpret_cast<Node*>(quals), oids);
    }

    /* Clone one supported raw-input AggPath over its new SliceProject owner. */
    [[nodiscard]] inline slice_project_aggregate_path_rewrite slice_project_rewrite_aggregate_path(
        PlannerInfo* root, UpperRelationKind stage, Path* path, const bioseq_stub_oids* oids) {
        if (path == nullptr)
            return {.path = path};
        if (!IsA(path, AggPath)) {
            return {
                .path = path,
                .unsafe = slice_project_non_aggregate_path_has_recipe(path, oids),
            };
        }
        auto* aggregate = reinterpret_cast<AggPath*>(path);
        if (aggregate->path.pathtarget == nullptr) {
            return {.path = path, .unsafe = true};
        }
        if (DO_AGGSPLIT_COMBINE(aggregate->aggsplit))
            return {.path = path};
        const bool supported_split =
            (stage == UPPERREL_PARTIAL_GROUP_AGG &&
             aggregate->aggsplit == AGGSPLIT_INITIAL_SERIAL) ||
            (stage == UPPERREL_GROUP_AGG && aggregate->aggsplit == AGGSPLIT_SIMPLE);
        slice_project_aggregate_input_validation validation =
            slice_project_collect_aggregate_inputs(root, aggregate, oids);
        if (!validation.saw_recipe)
            return {.path = path};
        if (!supported_split || validation.invalid || validation.candidates == NIL ||
            (stage == UPPERREL_GROUP_AGG &&
             slice_project_path_tree_has_exchange(aggregate->subpath))) {
            return {.path = path, .unsafe = true};
        }
        Path* owner = slice_project_build_aggregate_input_owner(root, aggregate, validation);
        if (owner == nullptr)
            return {.path = path, .unsafe = true};

        auto* clone = makeNode(AggPath);
        *clone = *aggregate;
        clone->subpath = owner;
        const Cost startup_delta =
            (aggregate->aggstrategy == AGG_SORTED || aggregate->aggstrategy == AGG_MIXED)
                ? owner->startup_cost - aggregate->subpath->startup_cost
                : owner->total_cost - aggregate->subpath->total_cost;
        clone->path.startup_cost += startup_delta;
        clone->path.total_cost += owner->total_cost - aggregate->subpath->total_cost;
        return {
            .path = reinterpret_cast<Path*>(clone),
            .changed = true,
        };
    }

    struct slice_project_aggregate_path_list_rewrite {
        List* paths = NIL;
        bool changed = false;
        bool unsafe = false;
    };

    /* Copy-on-write rewrite one complete planner path list or reject it whole. */
    [[nodiscard]] inline slice_project_aggregate_path_list_rewrite
    slice_project_rewrite_aggregate_path_list(PlannerInfo* root,
                                              UpperRelationKind stage,
                                              List* paths,
                                              const bioseq_stub_oids* oids) {
        slice_project_aggregate_path_list_rewrite result;
        for (Path* path : postgres_list<Path>(paths)) {
            slice_project_aggregate_path_rewrite rewritten =
                slice_project_rewrite_aggregate_path(root, stage, path, oids);
            if (rewritten.unsafe) {
                result.unsafe = true;
                return result;
            }
            result.paths = lappend(result.paths, rewritten.path);
            result.changed = result.changed || rewritten.changed;
        }
        return result;
    }

    /* Atomically install safe aggregate-input owners and refresh cheapest paths. */
    inline void slice_project_try_install_aggregate_input_owners(PlannerInfo* root,
                                                                 UpperRelationKind stage,
                                                                 RelOptInfo* output_rel) {
        const bioseq_stub_oids* oids = try_get_cached_stub_oids();
        if (oids == nullptr)
            return;
        slice_project_aggregate_path_list_rewrite ordinary =
            slice_project_rewrite_aggregate_path_list(root, stage, output_rel->pathlist, oids);
        slice_project_aggregate_path_list_rewrite partial;
        if (stage == UPPERREL_PARTIAL_GROUP_AGG) {
            partial = slice_project_rewrite_aggregate_path_list(
                root, stage, output_rel->partial_pathlist, oids);
        }
        if (ordinary.unsafe || partial.unsafe || (!ordinary.changed && !partial.changed))
            return;
        if (ordinary.changed)
            output_rel->pathlist = ordinary.paths;
        if (partial.changed)
            output_rel->partial_pathlist = partial.paths;
        if (ordinary.changed) {
            output_rel->cheapest_startup_path = nullptr;
            output_rel->cheapest_total_path = nullptr;
            output_rel->cheapest_unique_path = nullptr;
            output_rel->cheapest_parameterized_paths = NIL;
            set_cheapest(output_rel);
        }
    }

} // export custom-scan partition declarations

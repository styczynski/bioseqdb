/**
 * @file
 * @brief Installs SliceProject ownership at upper-planner stages.
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

export module bioseq.postgres.table:custom_scan_slice_rewrite_upper_path_callbacks;

export import :custom_scan_slice_rewrite_upper_path_callback_contracts;
import std;
import bioseq.postgres.configuration.core;
import bioseq.postgres.support;

import :custom_scan_oid_expression_support_oid_catalog;
import :custom_scan_slice_path_codec_contract;
import :custom_scan_slice_path_carrier_lineage;
import :custom_scan_slice_path_plan_construction;
import :custom_scan_slice_rewrite_path_cloning;
import :custom_scan_slice_rewrite_owner_admission;
import :custom_scan_slice_rewrite_aggregate_rewrite;

using namespace bioseq::postgres::support;

export {
    /* Install SliceProject ownership at the relevant upper-planner stages. */
    void bioseq_slice_project_create_upper_paths(PlannerInfo* root,
                                                 UpperRelationKind stage,
                                                 RelOptInfo* input_rel,
                                                 RelOptInfo* output_rel,
                                                 void*) {
        if (!bioseq::settings::planner::custom_scan::enabled || root == nullptr ||
            input_rel == nullptr || output_rel == nullptr || output_rel->reltarget == nullptr ||
            root->parse == nullptr || root->parse->hasRowSecurity ||
            root->parse->commandType != CMD_SELECT)
            return;
        if (stage == UPPERREL_PARTIAL_GROUP_AGG || stage == UPPERREL_GROUP_AGG) {
            slice_project_try_install_aggregate_input_owners(root, stage, output_rel);
            return;
        }
        if (!(stage == UPPERREL_WINDOW || stage == UPPERREL_DISTINCT || stage == UPPERREL_ORDERED ||
              stage == UPPERREL_FINAL))
            return;
        /*
         * PostgreSQL can defer the final SELECT projection until after the
         * UPPERREL_FINAL hook has run.  In that shape output_rel->reltarget still
         * describes the pass-through input (and a later core ProjectionPath becomes
         * the visible Result), while grouping_planner has already published the
         * stage's authoritative projection in root->upper_targets.  Discovering
         * recipes only in output_rel->reltarget therefore misses the common
         *
         *     Result (slice expression)
         *       -> BioseqScan (raw sequence Var)
         *
         * plan.  Build the owner against the same stage target core will apply
         * after this hook.  Falling back to the relation target preserves stages
         * for which core has no separate upper target.
         */
        PathTarget* projection_target = root->upper_targets[stage];
        if (projection_target == nullptr) {
            projection_target = output_rel->reltarget;
        }
        if (projection_target == nullptr ||
            slice_project_target_has_semantic_owner(root, projection_target))
            return;
        const bioseq_stub_oids* oids = try_get_cached_stub_oids();
        if (oids == nullptr)
            return;
        if (!is_parallel_safe(root, reinterpret_cast<Node*>(projection_target->exprs)))
            return;
        slice_project_ir_builder builder{.oids = oids, .command_type = root->parse->commandType};
        if (!slice_project_discover_target(projection_target, builder))
            return;
        for (Node* expression : postgres_list<Node>(projection_target->exprs)) {
            if (bioseq_expr_uses_lazy_sequence_projection(expression) &&
                contain_volatile_functions(expression))
                return;
        }
        Path* child = input_rel->cheapest_total_path;
        if (child == nullptr && input_rel->pathlist != NIL) {
            child = reinterpret_cast<Path*>(linitial(input_rel->pathlist));
        }
        if (child == nullptr)
            return;
        while (IsA(child, ProjectionPath) &&
               bioseq_expr_uses_lazy_sequence_projection(
                   reinterpret_cast<Node*>(child->pathtarget->exprs))) {
            child = reinterpret_cast<ProjectionPath*>(child)->subpath;
        }
        if (slice_project_is_owner_below_target_preserving_ordering(child, projection_target))
            return;

        List* required = NIL;
        constexpr int pull_flags =
            PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
        /* Only the owned recipe graph needs raw child inputs.  Ordinary projection
         * expressions are already represented by the chosen child's PathTarget and
         * must remain atomic here.  In particular, a raw BatchJoin output contract
         * publishes each match_result helper as one exact output identity;
         * descending through bioseq.query_start(t.pk) would synthesize a new t.pk
         * requirement above the owner even though that raw Var is intentionally
         * absent from its public target.  Core setrefs would then fail while binding
         * the intermediate ProjectionPath.  The IR expression list contains every
         * lazy producer,
         * source, concat operand, and bound expression, so pulling from it retains
         * the complete SliceProject dependency closure without opening non-lazy
         * helper identities. */
        for (Node* expression : postgres_list<Node>(builder.expressions)) {
            required = list_concat(required, pull_var_clause(expression, pull_flags));
        }
        PathTarget* child_target = copy_pathtarget(child->pathtarget);
        add_new_columns_to_pathtarget(child_target, required);
        set_pathtarget_cost_width(root, child_target);
        if (slice_project_is_bioseq_scan_path(child)) {
            child = slice_project_tag_source_child(child, child_target, builder);
        } else {
            /* A join/upper child is immutable and may be shared by many candidates.
             * Deep-copy supported core path shapes and tag every BioseqScan source
             * below them before installing hidden leaf outputs through core's
             * projection constructor.  The final target remains wrapper-only. */
            bool tagged_source = false;
            Path* private_child = slice_project_clone_mutable_path_spine(child);
            private_child =
                slice_project_tag_owned_path_tree(private_child, builder, tagged_source);
            if (!tagged_source)
                return;
            child = reinterpret_cast<Path*>(
                create_projection_path(root, private_child->parent, private_child, child_target));
        }

        QualCost expression_cost{};
        cost_qual_eval(&expression_cost, projection_target->exprs, root);
        const double rows = std::max(0.0, child->rows);
        const double metadata_per_row =
            cpu_operator_cost *
            static_cast<double>(std::min<std::uint64_t>(
                builder.piece_boundaries, std::numeric_limits<std::uint32_t>::max()));
        const double boundary_per_row =
            cpu_operator_cost *
            static_cast<double>(std::min<std::uint64_t>(
                builder.planned_symbols, std::numeric_limits<std::uint32_t>::max()));

        auto* path = makeNode(CustomPath);
        path->path.pathtype = T_CustomScan;
        path->path.parent = output_rel;
        path->path.pathtarget = copy_pathtarget(projection_target);
        path->path.param_info = child->param_info;
        path->path.parallel_aware = child->parallel_aware;
        path->path.parallel_safe = child->parallel_safe;
        path->path.parallel_workers = child->parallel_workers;
        path->path.rows = child->rows;
        path->path.startup_cost = child->startup_cost + expression_cost.startup;
        path->path.total_cost =
            child->total_cost + expression_cost.startup +
            rows * (expression_cost.per_tuple + metadata_per_row + boundary_per_row);
        path->path.pathkeys = child->pathkeys;
        path->flags = CUSTOMPATH_SUPPORT_PROJECTION;
        path->custom_paths = list_make1(child);
        path->custom_private = list_make5(slice_project_header(builder),
                                          builder.expressions,
                                          builder.nodes,
                                          builder.records,
                                          builder.output_bindings);
        path->custom_restrictinfo = NIL;
        path->methods = &bioseq_slice_project_path_methods();

        /* The core ProjectionPath remains fractionally cheaper because PostgreSQL's
         * generic cost model cannot represent avoided full-sequence hydration.  Do
         * not falsify either cost.  Once a complete source-tagged wrapper exists,
         * remove the incompatible unowned alternatives so selection is structural.
         * Every early/fallback return above leaves the original lists untouched. */
        output_rel->pathlist = NIL;
        output_rel->partial_pathlist = NIL;
        output_rel->cheapest_startup_path = nullptr;
        output_rel->cheapest_total_path = nullptr;
#if PG_VERSION_NUM < 190000
        output_rel->cheapest_unique_path = nullptr;
#endif
        output_rel->cheapest_parameterized_paths = NIL;
        add_path(output_rel, reinterpret_cast<Path*>(path));
        set_cheapest(output_rel);
    }

} // export custom-scan partition declarations

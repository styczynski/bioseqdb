/**
 * @file
 * @brief Validates base and aggregate SliceProject ownership.
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

export module bioseq.postgres.table:custom_scan_slice_rewrite_owner_admission;

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

import :custom_scan_slice_path;
import :custom_scan_slice_execution;

import :custom_scan_slice_rewrite_path_cloning;

using namespace bioseq::postgres::support;

export {
    struct slice_project_base_target_validation {
        Index relid = 0;
        bool allow_row_identity = false;
        bool saw_base_var = false;
        bool invalid = false;
    };

    [[nodiscard]] bool slice_project_is_target_row_identity_var(const Var* var,
                                                                const Index relid) noexcept {
        if (var == nullptr || !bioseq_is_concrete_rte_varno(relid) || var->varno != relid ||
            var->varlevelsup != 0 || !bms_is_empty(var->varnullingrels))
            return false;
        return (var->varattno == SelfItemPointerAttributeNumber && var->vartype == TIDOID &&
                var->vartypmod == -1 && !OidIsValid(var->varcollid)) ||
               (var->varattno == TableOidAttributeNumber && var->vartype == OIDOID &&
                var->vartypmod == -1 && !OidIsValid(var->varcollid));
    }

    struct slice_project_base_target_visitor {
        [[nodiscard]] pg_walk_step
        operator()(pg_expression_walk_cursor,
                   pg_node_view node,
                   slice_project_base_target_validation& validation) const noexcept {
            if (node.try_cast<PlaceHolderVar>() != nullptr) {
                /* A PHV belongs to a planner-selected evaluation/nulling frontier.
                 * A base SliceProject must not move it below that frontier. */
                validation.invalid = true;
                return pg_walk_step::stop;
            }
            if (Var* var = node.try_cast<Var>()) {
                if (validation.allow_row_identity &&
                    slice_project_is_target_row_identity_var(var, validation.relid)) {
                    return pg_walk_step::skip_subtree;
                }
                if (var->varno != validation.relid || var->varattno <= 0 || var->varlevelsup != 0 ||
                    !bms_is_empty(var->varnullingrels)) {
                    validation.invalid = true;
                    return pg_walk_step::stop;
                }
                validation.saw_base_var = true;
                return pg_walk_step::skip_subtree;
            }
            return pg_walk_step::descend;
        }
    };

    [[nodiscard]] bool slice_project_target_has_semantic_owner(PlannerInfo* root,
                                                               PathTarget* target) {
        if (root == nullptr || root->parse == nullptr || target == nullptr) {
            return true;
        }
        Node* expressions = reinterpret_cast<Node*>(target->exprs);
        return root->parse->hasAggs || root->parse->hasWindowFuncs || root->parse->hasTargetSRFs ||
               root->parse->groupClause != NIL || root->parse->groupingSets != NIL ||
               root->parse->havingQual != nullptr || root->parse->setOperations != nullptr ||
               contain_agg_clause(expressions) || contain_window_function(expressions) ||
               contain_volatile_functions(expressions) || contain_subplans(expressions) ||
               expression_returns_set(expressions);
    }

    /* UPDATE's RETURNING target belongs to ModifyTable, not to its scan/join
     * subplan.  We may describe a nonvolatile lazy subtree below a volatile final
     * consumer, but we must never move or execute that consumer here.  This
     * validation checks only the base-local Var/semantic frontier and deliberately
     * does not reject volatile ancestors; slice_project_discover_target() rejects
     * every recognized lazy subtree which itself contains volatility. */
    [[nodiscard]] bool slice_project_update_recipe_target_is_safe(PlannerInfo* root,
                                                                  RelOptInfo* rel,
                                                                  PathTarget* target) {
        if (root == nullptr || rel == nullptr || root->parse == nullptr || target == nullptr ||
            target->exprs == NIL || contain_agg_clause(reinterpret_cast<Node*>(target->exprs)) ||
            contain_window_function(reinterpret_cast<Node*>(target->exprs)) ||
            contain_subplans(reinterpret_cast<Node*>(target->exprs)) ||
            expression_returns_set(reinterpret_cast<Node*>(target->exprs))) {
            return false;
        }
        slice_project_base_target_validation validation{.relid = rel->relid};
        slice_project_base_target_visitor visitor;
        walk_pg_expression_including_root(
            reinterpret_cast<Node*>(target->exprs), validation, visitor);
        return validation.saw_base_var && !validation.invalid;
    }

    /* Validate that the final processed projection can be evaluated immediately
     * above this one base relation.  The single-baserel restriction is intentionally
     * strict: a wider query needs a join/upper owner which preserves its own
     * nulling and evaluation frontier. */
    [[nodiscard]] bool
    slice_project_base_target_is_safe(PlannerInfo* root, RelOptInfo* rel, PathTarget* target) {
        if (root == nullptr || rel == nullptr || root->parse == nullptr || target == nullptr ||
            target->exprs == NIL || root->parse->hasRowSecurity ||
            !(root->parse->commandType == CMD_SELECT || root->parse->commandType == CMD_INSERT ||
              root->parse->commandType == CMD_UPDATE) ||
            rel->relid == 0 || rel->reloptkind != RELOPT_BASEREL ||
            bms_num_members(root->all_baserels) != 1 ||
            !bms_is_member(static_cast<int>(rel->relid), root->all_baserels) ||
            !bms_is_empty(rel->lateral_relids) ||
            slice_project_target_has_semantic_owner(root, target)) {
            return false;
        }
        slice_project_base_target_validation validation{
            .relid = rel->relid,
            .allow_row_identity = root->parse->commandType == CMD_UPDATE,
        };
        slice_project_base_target_visitor visitor;
        walk_pg_expression_including_root(
            reinterpret_cast<Node*>(target->exprs), validation, visitor);
        return (validation.saw_base_var || root->parse->commandType == CMD_UPDATE) &&
               !validation.invalid;
    }

    [[nodiscard]] bool slice_project_update_sources_are_stable(
        PlannerInfo* root, RelOptInfo* rel, slice_project_ir_builder const& builder) {
        if (root == nullptr || rel == nullptr || root->update_colnos == NIL) {
            return false;
        }
        constexpr int pull_flags =
            PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
        bool saw_sequence_source = false;
        for (Node* expression : postgres_list<Node>(builder.expressions)) {
            List* vars = pull_var_clause(expression, pull_flags);
            for (Var* var : postgres_list<Var>(vars)) {
                if (var->vartype != builder.oids->bioseq_seq_type_oid)
                    continue;
                if (var->varno != rel->relid || var->varattno <= 0 || var->varlevelsup != 0 ||
                    !bms_is_empty(var->varnullingrels) ||
                    list_member_int(root->update_colnos, var->varattno)) {
                    return false;
                }
                saw_sequence_source = true;
            }
        }
        return saw_sequence_source;
    }

    /* Install the lowest valid owner for a base-local recipe.  Every surviving
     * alternative is a real SliceProject -> BioseqScan pair; incompatible index,
     * heap, and untagged paths are removed only after at least one complete owner
     * has been constructed.  Costs remain ordinary nonnegative PostgreSQL costs. */
    void
    slice_project_try_install_base_owner(PlannerInfo* root, RelOptInfo* rel, Relation relation) {
        if (!bioseq::settings::planner::custom_scan::enabled || root == nullptr || rel == nullptr ||
            relation == nullptr || root->processed_tlist == NIL || root->parse == nullptr)
            return;
        const bool update_returning = root->parse->commandType == CMD_UPDATE;
        if (update_returning &&
            (root->parse->resultRelation != rel->relid || root->parse->returningList == NIL ||
             relation->trigdesc != nullptr))
            return;
        PathTarget* projection_target = make_pathtarget_from_tlist(root->processed_tlist);
        set_pathtarget_cost_width(root, projection_target);
        if (!slice_project_base_target_is_safe(root, rel, projection_target))
            return;
        if (!is_parallel_safe(root, reinterpret_cast<Node*>(projection_target->exprs)))
            return;
        const bioseq_stub_oids* oids = try_get_cached_stub_oids();
        if (oids == nullptr)
            return;
        slice_project_ir_builder builder{.oids = oids, .command_type = root->parse->commandType};
        PathTarget* recipe_target = projection_target;
        if (update_returning) {
            recipe_target = make_pathtarget_from_tlist(root->parse->returningList);
            set_pathtarget_cost_width(root, recipe_target);
            if (!slice_project_update_recipe_target_is_safe(root, rel, recipe_target))
                return;
        }
        if (!slice_project_discover_target(recipe_target, builder))
            return;
        if (!is_parallel_safe(root, reinterpret_cast<Node*>(builder.expressions)))
            return;
        if (update_returning) {
            /* RETURNING is evaluated by ModifyTable against the actual updated
             * tuple.  These bindings use RETURNING resnos and must never be applied
             * to the distinct UPDATE subplan output descriptor. */
            builder.output_bindings = NIL;
            if (!slice_project_update_sources_are_stable(root, rel, builder))
                return;
        }

        List* required = NIL;
        constexpr int pull_flags =
            PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
        for (Node* expression : postgres_list<Node>(projection_target->exprs)) {
            required = list_concat(required, pull_var_clause(expression, pull_flags));
        }
        if (update_returning) {
            for (Node* expression : postgres_list<Node>(builder.expressions)) {
                required = list_concat(required, pull_var_clause(expression, pull_flags));
            }
        }
        QualCost expression_cost{};
        cost_qual_eval(&expression_cost, projection_target->exprs, root);
        const double metadata_per_row =
            cpu_operator_cost *
            static_cast<double>(std::min<std::uint64_t>(builder.piece_boundaries, UINT32_MAX));
        const double boundary_per_row =
            cpu_operator_cost *
            static_cast<double>(std::min<std::uint64_t>(builder.planned_symbols, UINT32_MAX));

        postgres_list<Path> owners;
        postgres_list<Path> partial_owners;
        auto build_owner = [root,
                            rel,
                            projection_target,
                            required,
                            &builder,
                            &expression_cost,
                            metadata_per_row,
                            boundary_per_row](Path* source) -> Path* {
            PathTarget* child_target = copy_pathtarget(source->pathtarget);
            add_new_columns_to_pathtarget(child_target, required);
            set_pathtarget_cost_width(root, child_target);
            Path* child = slice_project_tag_source_child(source, child_target, builder);

            auto* owner = makeNode(CustomPath);
            owner->path.pathtype = T_CustomScan;
            owner->path.parent = rel;
            owner->path.pathtarget = copy_pathtarget(projection_target);
            owner->path.param_info = child->param_info;
            owner->path.parallel_aware = child->parallel_aware;
            owner->path.parallel_safe = child->parallel_safe;
            owner->path.parallel_workers = child->parallel_workers;
            owner->path.rows = child->rows;
            owner->path.startup_cost = child->startup_cost + expression_cost.startup;
            const double rows = std::max(0.0, child->rows);
            owner->path.total_cost =
                child->total_cost + expression_cost.startup +
                rows * (expression_cost.per_tuple + metadata_per_row + boundary_per_row);
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
        };
        for (Path* source : postgres_list<Path>(rel->pathlist)) {
            if (!slice_project_is_bioseq_scan_path(source))
                continue;
            owners.append(build_owner(source));
        }
        for (Path* source : postgres_list<Path>(rel->partial_pathlist)) {
            if (!slice_project_is_bioseq_scan_path(source))
                continue;
            partial_owners.append(build_owner(source));
        }
        if (owners.is_empty())
            return;

        /* Gather paths are derived after this hook from reltarget.  Since every
         * surviving ordinary and partial path publishes the processed projection,
         * make that target canonical before core constructs Gather; otherwise it
         * asks a final-expression SliceProject child to publish the raw Var. */
        rel->reltarget = copy_pathtarget(projection_target);
        rel->pathlist = NIL;
        rel->partial_pathlist = NIL;
        rel->cheapest_startup_path = nullptr;
        rel->cheapest_total_path = nullptr;
        rel->cheapest_unique_path = nullptr;
        rel->cheapest_parameterized_paths = NIL;
        for (Path* owner : owners)
            add_path(rel, owner);
        for (Path* owner : partial_owners)
            add_partial_path(rel, owner);
    }

    struct slice_project_aggregate_recipe_presence {
        const bioseq_stub_oids* oids = nullptr;
        bool found = false;
    };

    struct slice_project_aggregate_recipe_presence_visitor {
        /* Stop at the first planner expression represented by the projection IR. */
        [[nodiscard]] pg_walk_step
        operator()(pg_expression_walk_cursor,
                   pg_node_view node,
                   slice_project_aggregate_recipe_presence& analysis) const noexcept {
            if (slice_project_node_opcode(node.get(), *analysis.oids).has_value()) {
                analysis.found = true;
                return pg_walk_step::stop;
            }
            return pg_walk_step::descend;
        }
    };

    /* Report whether an expression tree contains a recognized projection recipe. */
    [[nodiscard]] inline bool
    slice_project_aggregate_expression_has_recipe(Node* expression, const bioseq_stub_oids* oids) {
        if (expression == nullptr || oids == nullptr)
            return false;
        slice_project_aggregate_recipe_presence analysis{.oids = oids};
        slice_project_aggregate_recipe_presence_visitor visitor;
        walk_pg_expression_including_root(expression, analysis, visitor);
        return analysis.found;
    }

    struct slice_project_aggregate_argument_validation {
        const bioseq_stub_oids* oids = nullptr;
        Relids available_relids = nullptr;
        bool saw_sequence_var = false;
        bool invalid = false;
    };

    struct slice_project_aggregate_argument_visitor {
        /* Validate that every Var belongs to the aggregate input relation. */
        [[nodiscard]] pg_walk_step
        operator()(pg_expression_walk_cursor,
                   pg_node_view node,
                   slice_project_aggregate_argument_validation& validation) const noexcept {
            if (node.try_cast<PlaceHolderVar>() != nullptr || node.try_cast<Param>() != nullptr) {
                validation.invalid = true;
                return pg_walk_step::stop;
            }
            if (Var* var = node.try_cast<Var>()) {
                if (!bioseq_is_concrete_rte_varno(var->varno) ||
                    !bms_is_member(static_cast<int>(var->varno), validation.available_relids) ||
                    var->varattno <= 0 || var->varlevelsup != 0 ||
                    !bms_is_empty(var->varnullingrels)) {
                    validation.invalid = true;
                    return pg_walk_step::stop;
                }
                if (var->vartype == validation.oids->bioseq_seq_type_oid) {
                    validation.saw_sequence_var = true;
                }
                return pg_walk_step::skip_subtree;
            }
            return pg_walk_step::descend;
        }
    };

    /* Accept only parallel-safe aggregate inputs rooted in a sequence Var. */
    [[nodiscard]] inline bool
    slice_project_aggregate_argument_is_safe(PlannerInfo* root,
                                             Relids available_relids,
                                             Node* expression,
                                             const bioseq_stub_oids* oids,
                                             bool require_sequence_var = true) {
        if (root == nullptr || available_relids == nullptr || expression == nullptr ||
            oids == nullptr || contain_agg_clause(expression) ||
            contain_window_function(expression) || contain_subplans(expression) ||
            expression_returns_set(expression) || contain_volatile_functions(expression) ||
            !is_parallel_safe(root, expression))
            return false;
        slice_project_aggregate_argument_validation validation{
            .oids = oids, .available_relids = available_relids};
        slice_project_aggregate_argument_visitor visitor;
        walk_pg_expression_including_root(expression, validation, visitor);
        return !validation.invalid && (!require_sequence_var || validation.saw_sequence_var);
    }

    struct slice_project_aggregate_input_validation {
        PlannerInfo* root = nullptr;
        const bioseq_stub_oids* oids = nullptr;
        Relids available_relids = nullptr;
        List* candidates = NIL;
        List* input_expressions = NIL;
        List* visited_aggregates = NIL;
        bool saw_recipe = false;
        bool invalid = false;
    };

    /* Append one structurally distinct whole aggregate-argument expression. */
    inline void slice_project_add_unique_aggregate_candidate(
        slice_project_aggregate_input_validation& validation, Node* expression) {
        for (Node* candidate : postgres_list<Node>(validation.candidates)) {
            if (equal(candidate, expression))
                return;
        }
        validation.candidates = lappend(validation.candidates, expression);
    }

    /* Retain every ordinary argument expression so raw Vars needed by resjunk
     * ORDER BY carriers are published through the lower owner as well. */
    inline void
    slice_project_add_unique_aggregate_input(slice_project_aggregate_input_validation& validation,
                                             Node* expression) {
        for (Node* input : postgres_list<Node>(validation.input_expressions)) {
            if (equal(input, expression))
                return;
        }
        validation.input_expressions = lappend(validation.input_expressions, expression);
    }

    struct slice_project_aggregate_input_visitor {
        /* Collect movable whole arguments while enforcing aggregate barriers. */
        [[nodiscard]] pg_walk_step
        operator()(pg_expression_walk_cursor,
                   pg_node_view node,
                   slice_project_aggregate_input_validation& validation) const {
            if (node.get_tag() == T_GroupingFunc || node.get_tag() == T_WindowFunc ||
                node.get_tag() == T_SubPlan) {
                return pg_walk_step::skip_subtree;
            }
            Aggref* aggregate = node.try_cast<Aggref>();
            if (aggregate == nullptr)
                return pg_walk_step::descend;
            if (aggregate->agglevelsup != 0 ||
                list_member_ptr(validation.visited_aggregates, aggregate)) {
                return pg_walk_step::skip_subtree;
            }
            validation.visited_aggregates = lappend(validation.visited_aggregates, aggregate);

            List* aggregate_candidates = NIL;
            bool malformed_arguments = false;
            bool other_argument_is_unsafe = false;
            for (Node* raw_argument : postgres_list<Node>(aggregate->args)) {
                TargetEntry* argument = try_cast_pg_node<TargetEntry>(raw_argument);
                if (argument == nullptr || argument->expr == nullptr) {
                    malformed_arguments = true;
                    continue;
                }
                Node* expression = reinterpret_cast<Node*>(argument->expr);
                other_argument_is_unsafe =
                    other_argument_is_unsafe || contain_agg_clause(expression) ||
                    contain_window_function(expression) || contain_subplans(expression) ||
                    expression_returns_set(expression) || contain_volatile_functions(expression);
                if (slice_project_aggregate_expression_has_recipe(expression, validation.oids)) {
                    aggregate_candidates = lappend(aggregate_candidates, expression);
                }
            }
            bool barrier_has_recipe = false;
            for (Node* direct : postgres_list<Node>(aggregate->aggdirectargs)) {
                barrier_has_recipe =
                    barrier_has_recipe ||
                    slice_project_aggregate_expression_has_recipe(direct, validation.oids);
            }
            if (aggregate->aggfilter != nullptr) {
                barrier_has_recipe =
                    barrier_has_recipe ||
                    slice_project_aggregate_expression_has_recipe(
                        reinterpret_cast<Node*>(aggregate->aggfilter), validation.oids);
            }
            if (aggregate_candidates == NIL && !barrier_has_recipe) {
                return pg_walk_step::skip_subtree;
            }
            validation.saw_recipe = true;
            /* FILTER controls whether ordinary arguments are evaluated.  Direct
             * arguments and ordered-set/hypothetical aggregates have group-level
             * rather than per-input-row semantics.  None may cross this Agg node. */
            if (malformed_arguments || aggregate->aggkind != AGGKIND_NORMAL ||
                aggregate->aggdirectargs != NIL || aggregate->aggfilter != nullptr ||
                barrier_has_recipe || other_argument_is_unsafe) {
                validation.invalid = true;
                return pg_walk_step::stop;
            }
            /* Validate every ordinary input, including resjunk ORDER BY carriers,
             * against the same sole source relation.  The owner copies the complete
             * input PathTarget, so these expressions retain their original carrier
             * positions and sort-group wiring. */
            for (Node* raw_argument : postgres_list<Node>(aggregate->args)) {
                TargetEntry* argument = try_cast_pg_node<TargetEntry>(raw_argument);
                Node* expression = reinterpret_cast<Node*>(argument->expr);
                if (!slice_project_aggregate_argument_is_safe(validation.root,
                                                              validation.available_relids,
                                                              expression,
                                                              validation.oids,
                                                              false)) {
                    validation.invalid = true;
                    return pg_walk_step::stop;
                }
                slice_project_add_unique_aggregate_input(validation, expression);
            }
            for (Node* expression : postgres_list<Node>(aggregate_candidates)) {
                if (!slice_project_aggregate_argument_is_safe(validation.root,
                                                              validation.available_relids,
                                                              expression,
                                                              validation.oids)) {
                    validation.invalid = true;
                    return pg_walk_step::stop;
                }
                /* Publish the exact argument expression, never its TargetEntry.  The
                 * Aggref therefore retains its resno/ressortgroupref and aggorder
                 * wiring while setrefs maps only the argument to the owner output. */
                slice_project_add_unique_aggregate_candidate(validation, expression);
            }
            return pg_walk_step::skip_subtree;
        }
    };

    /* Resolve relation identity from the sole source-owning scan rather than an
     * upper SortPath's parent RelOptInfo (upper relations intentionally have no
     * base relids).  Only target-preserving unary containers may be crossed: this
     * keeps every ordered-aggregate carrier and its PathTarget sortgroupref intact. */
    [[nodiscard]] inline Relids slice_project_aggregate_source_relids(Path* path) noexcept {
        while (path != nullptr) {
            if (slice_project_is_bioseq_scan_path(path)) {
                return path->parent != nullptr && path->parent->relids != nullptr &&
                               bms_num_members(path->parent->relids) == 1
                           ? path->parent->relids
                           : nullptr;
            }
            Path* child = nullptr;
            if (IsA(path, ProjectionPath)) {
                child = reinterpret_cast<ProjectionPath*>(path)->subpath;
            } else if (IsA(path, SortPath)) {
                child = reinterpret_cast<SortPath*>(path)->subpath;
            } else if (IsA(path, IncrementalSortPath)) {
                child = reinterpret_cast<IncrementalSortPath*>(path)->spath.subpath;
            } else if (IsA(path, MaterialPath)) {
                child = reinterpret_cast<MaterialPath*>(path)->subpath;
            } else {
                return nullptr;
            }
            if (child == nullptr ||
                !slice_project_targets_match(path->pathtarget, child->pathtarget))
                return nullptr;
            path = child;
        }
        return nullptr;
    }

    /* Discover every supported projection argument consumed by one AggPath. */
    [[nodiscard]] inline slice_project_aggregate_input_validation
    slice_project_collect_aggregate_inputs(PlannerInfo* root,
                                           AggPath* aggregate,
                                           const bioseq_stub_oids* oids) {
        slice_project_aggregate_input_validation validation{
            .root = root,
            .oids = oids,
            .available_relids = aggregate != nullptr
                                    ? slice_project_aggregate_source_relids(aggregate->subpath)
                                    : nullptr,
        };
        if (aggregate == nullptr || aggregate->path.pathtarget == nullptr ||
            aggregate->path.pathtarget->exprs == NIL || validation.available_relids == nullptr) {
            validation.invalid = true;
            return validation;
        }
        slice_project_aggregate_input_visitor visitor;
        walk_pg_expression_including_root(
            reinterpret_cast<Node*>(aggregate->path.pathtarget->exprs), validation, visitor);
        if (!validation.invalid && aggregate->qual != NIL) {
            walk_pg_expression_including_root(
                reinterpret_cast<Node*>(aggregate->qual), validation, visitor);
        }
        return validation;
    }

    /* Report whether a path spine crosses a Gather or GatherMerge boundary. */
    [[nodiscard]] inline bool slice_project_path_tree_has_exchange(Path* path) noexcept {
        if (path == nullptr)
            return false;
        if (IsA(path, GatherPath) || IsA(path, GatherMergePath))
            return true;
        if (IsA(path, NestPath)) {
            auto* join = &reinterpret_cast<NestPath*>(path)->jpath;
            return slice_project_path_tree_has_exchange(join->outerjoinpath) ||
                   slice_project_path_tree_has_exchange(join->innerjoinpath);
        }
        if (IsA(path, MergePath)) {
            auto* join = &reinterpret_cast<MergePath*>(path)->jpath;
            return slice_project_path_tree_has_exchange(join->outerjoinpath) ||
                   slice_project_path_tree_has_exchange(join->innerjoinpath);
        }
        if (IsA(path, HashPath)) {
            auto* join = &reinterpret_cast<HashPath*>(path)->jpath;
            return slice_project_path_tree_has_exchange(join->outerjoinpath) ||
                   slice_project_path_tree_has_exchange(join->innerjoinpath);
        }
        if (IsA(path, CustomPath)) {
            for (Path* child :
                 postgres_list<Path>(reinterpret_cast<CustomPath*>(path)->custom_paths)) {
                if (slice_project_path_tree_has_exchange(child))
                    return true;
            }
            return false;
        }
        Path* unary = nullptr;
        if (IsA(path, ProjectionPath)) {
            unary = reinterpret_cast<ProjectionPath*>(path)->subpath;
        } else if (IsA(path, ProjectSetPath)) {
            unary = reinterpret_cast<ProjectSetPath*>(path)->subpath;
        } else if (IsA(path, SortPath)) {
            unary = reinterpret_cast<SortPath*>(path)->subpath;
        } else if (IsA(path, IncrementalSortPath)) {
            unary = reinterpret_cast<IncrementalSortPath*>(path)->spath.subpath;
        } else if (IsA(path, MaterialPath)) {
            unary = reinterpret_cast<MaterialPath*>(path)->subpath;
        } else if (IsA(path, MemoizePath)) {
            unary = reinterpret_cast<MemoizePath*>(path)->subpath;
        } else if (IsA(path, UniquePath)) {
            unary = reinterpret_cast<UniquePath*>(path)->subpath;
        }
        if (unary != nullptr)
            return slice_project_path_tree_has_exchange(unary);
        List* children = NIL;
        if (IsA(path, AppendPath)) {
            children = reinterpret_cast<AppendPath*>(path)->subpaths;
        } else if (IsA(path, MergeAppendPath)) {
            children = reinterpret_cast<MergeAppendPath*>(path)->subpaths;
        }
        for (Path* child : postgres_list<Path>(children)) {
            if (slice_project_path_tree_has_exchange(child))
                return true;
        }
        return false;
    }

} // export custom-scan partition declarations

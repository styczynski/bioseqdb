/**
 * @file
 * @brief Constructs recursive and parameterized Bioseq scan paths.
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

// Keep libc++ declarations textual and ahead of the PostgreSQL dependency BMIs.
// Importing the complete standard-library module here pushes Clang 22's source
// location table past its 2 GiB limit before the final planner contracts load.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>

export module bioseq.postgres.table:custom_scan_scan_paths_path_construction;

import bioseq.common.checked_arithmetic;
import bioseq.common.logging;
import bioseq.indexing.type;
import bioseq.pipeline.minimap_parameters_contract;
import bioseq.postgres.column.table_access;
import bioseq.postgres.configuration.core;
import bioseq.postgres.external.path_builder_slot;
import bioseq.postgres.support;
import bioseq.postgres.table.batch_sort_scan;

import :api;
import :custom_scan_join_ownership;
import :custom_scan_join_paths;
import :custom_scan_batch_sort_source_route_contracts;
import :custom_scan_oid_expression_support;
import :custom_scan_plan_abi_constants;
import :custom_scan_planner_aligner_contracts;
import :custom_scan_planner_diagnostic_contracts;
import :custom_scan_planner_hook_state_contracts;
import :custom_scan_planner_method_contracts;
import :custom_scan_planner_path_contracts;
import :custom_scan_runtime_buffer_contracts;
import :custom_scan_scan_paths_admission_contracts;
import :custom_scan_scan_paths_cost_finalization_contracts;
import :custom_scan_slice_path_codec_contract;
import :custom_scan_slice_path_carrier_contracts;
import :custom_scan_slice_path_cost_contracts;
import :custom_scan_slice_rewrite_path_contracts;
import :custom_scan_slice_rewrite_path_tagging_contracts;
import :custom_scan_slice_rewrite_owner_admission_contracts;
import :custom_scan_slice_rewrite_upper_path_callback_contracts;

using namespace bioseq::postgres::support;

export {
    /**
     * @brief Enumerates bounded parameterization subsets and adds columnar custom paths.
     *
     * Recursively searches the
     * parameterization space and add CustomPaths for columnar scans.
     *
     * The set paramRelids is the parameterization at the current level, and
     * candidateRelids is the set from which we draw to generate paths with
     * greater parameterization.
     *
     * BioseqColumn tables resemble indexes because of the ability to push down
     * quals. Ordinary quals, such as x = 7, can be pushed down easily. But join
     * quals of the form "x = y" (where "y" comes from another rel) require the
     * proper parameterization.
     *
     * Paths that require more outer rels can push down more join clauses that
     * depend on those outer rels. But requiring more outer rels gives the planner
     * fewer options for the shape of the plan. That means there is a trade-off,
     * and we should generate plans of various parameterizations, then let the
     * planner choose. We always need to generate one minimally-parameterized path
     * (parameterized only by lateral refs, if present) to make sure that at least
     * one path can be chosen. Then, we generate as many parameterized paths as we
     * reasonably can.
     *
     * The set of all possible parameterizations is the power set of
     * candidateRelids. The power set has cardinality 2^N, where N is the
     * cardinality of candidateRelids. To avoid creating a huge number of paths,
     * limit the depth of the search; the depthLimit is equivalent to the maximum
     * number of required outer rels (beyond the minimal parameterization) for the
     * path. A depthLimit of zero means that only the minimally-parameterized path
     * will be generated.
     */
    void add_bioseq_column_scan_paths_rec(PlannerInfo* root,
                                          RelOptInfo* rel,
                                          RangeTblEntry* rte,
                                          Relids param_relids,
                                          Relids candidate_relids,
                                          int depth_limit) {
        CHECK_FOR_INTERRUPTS();
        check_stack_depth();

        Assert(!bms_overlap(param_relids, candidate_relids));

        Path* bioseq_column_scan_path = add_bioseq_column_scan_path(root, rel, rte, param_relids);
        CustomPath* owner_path = IsA(bioseq_column_scan_path, CustomPath)
                                     ? reinterpret_cast<CustomPath*>(bioseq_column_scan_path)
                                     : nullptr;
        if (owner_path != nullptr)
            for (RestrictInfo* owner :
                 postgres_list<RestrictInfo>(owner_path->custom_restrictinfo)) {
                // Removes paths that execute an mmsearch restriction claimed by another owner.
                auto prune_competing_owner_execution = [rel, owner](List* paths) {
                    postgres_list<Path> survivors;
                    for (Path* candidate : postgres_list<Path>(paths)) {
                        const bool executes_owner =
                            list_member_ptr(rel->baserestrictinfo, owner) ||
                            (candidate->param_info != nullptr &&
                             list_member_ptr(candidate->param_info->ppi_clauses, owner));
                        const bool owns_restriction =
                            IsA(candidate, CustomPath) &&
                            list_member_ptr(
                                reinterpret_cast<CustomPath*>(candidate)->custom_restrictinfo,
                                owner);
                        if (!executes_owner || owns_restriction)
                            survivors.append(candidate);
                    }
                    return survivors.release();
                };
                rel->pathlist = prune_competing_owner_execution(rel->pathlist);
                rel->partial_pathlist = prune_competing_owner_execution(rel->partial_pathlist);
            }
        if (bioseq::settings::execution::parallel::enabled)
            bioseq_column_scan_path->total_cost += bioseq_column_scan_path->rows * 0.1;
        add_path(rel, bioseq_column_scan_path);

        /* For columnar custom scan we should also do planning for parallel exeuction
         * if it is enabled with GUC.
         */
        if (bioseq::settings::execution::parallel::enabled) {
            int bioseq_min_parallel_process_running =
                bioseq::settings::execution::parallel::minimum_processes;

            if (bioseq::settings::execution::parallel::minimum_processes > max_parallel_workers) {
                bioseq_log_pg(
                    DEBUG1,
                    "bioseq.min_parallel_proceses is set higher than max_parallel_workers.");
                bioseq_log_pg(DEBUG1,
                              "Using max_parallel_workers instead for parallel columnar scan.");
                bioseq_min_parallel_process_running = std::min(
                    max_parallel_workers, bioseq::settings::execution::parallel::minimum_processes);
            }

            if (parallel_leader_participation)
                bioseq_min_parallel_process_running--;

            /*
             * We know how many workers are we going to run. Since workers are using single stripe
             * probably it doesn't have much sense to spawn more workers than stripes in table.
             */
            bioseq_min_parallel_process_running = std::min<int64_t>(
                bioseq_min_parallel_process_running,
                parallel_leader_participation ? bioseq_column_table_stripe_count(rte->relid) - 1
                                              : bioseq_column_table_stripe_count(rte->relid));

            /* Apply the configured minimum parallel process count unconditionally. */
            const bool force_parallel = true;
            if (force_parallel) {
                bioseq_log<logging_level::debug>(
                    "[bioseq] pg: AddBioseqColumnScanPathsRec() force_parallel override active");
                bioseq_min_parallel_process_running =
                    bioseq::settings::execution::parallel::minimum_processes;
            }

            if (force_parallel || (rel->consider_parallel && rel->lateral_relids == nullptr &&
                                   bioseq_min_parallel_process_running > 0)) {
                bioseq_log<logging_level::debug>("[bioseq] pg: AddBioseqColumnScanPathsRec() "
                                                 "add_partial_path for parallel scan");
                /*
                 * Passing NULL for JOIN quals in parallel execution.
                 */
                Path* parallel_bioseq_column_scan_path =
                    add_bioseq_column_scan_path(root, rel, rte, nullptr);

                if (parallel_bioseq_column_scan_path->parallel_safe) {
                    parallel_bioseq_column_scan_path->parallel_workers =
                        bioseq_min_parallel_process_running;
                    parallel_bioseq_column_scan_path->parallel_aware = true;
                    adjust_bioseq_column_parallel_scan_cost(parallel_bioseq_column_scan_path);
                    add_partial_path(rel, parallel_bioseq_column_scan_path);
                } else {
                    bioseq_log<logging_level::debug>("[bioseq] pg: suppressing parallel partial "
                                                     "path for alignment-owning custom scan");
                }
            }
        }

        bioseq_log<logging_level::debug>(
            "[bioseq] pg: AddBioseqColumnScanPathsRec() recursing into candidate relids");

        /* recurse for all candidateRelids, unless we hit the depth limit */
        Assert(depth_limit >= 0);
        if (depth_limit-- == 0) {
            return;
        }

        /*
         * Iterate through parameter combinations depth-first. Deeper levels
         * generate paths of greater parameterization (and hopefully lower
         * cost).
         */
        bioseq_log<logging_level::debug>("[bioseq] pg: AddBioseqColumnScanPathsRec() iterating "
                                         "parameter combinations depth-first");
        Relids tmp_candidate_relids = bms_copy(candidate_relids);
        int relid = -1;
        while ((relid = bms_next_member(candidate_relids, relid)) >= 0) {
            Relids tmp_param_relids = bms_add_member(bms_copy(param_relids), relid);

            /*
             * Because we are generating combinations (not permutations), remove
             * the relid from the set of candidates at this level as we descend to
             * the next.
             */
            tmp_candidate_relids = bms_del_member(tmp_candidate_relids, relid);

            add_bioseq_column_scan_paths_rec(
                root, rel, rte, tmp_param_relids, tmp_candidate_relids, depth_limit);
        }

        bms_free(tmp_candidate_relids);
    }

    /** @brief Generates bounded parameterized custom paths for a columnar base relation. */
    void add_bioseq_column_scan_paths(PlannerInfo* root, RelOptInfo* rel, RangeTblEntry* rte) {
        bioseq_log<logging_level::debug>("[bioseq] pg: AddBioseqColumnScanPaths() entered");
        List* join_clauses = find_pushdown_join_clauses(root, rel);
        Relids candidate_relids = find_candidate_relids(root, rel, join_clauses);

        int depth_limit = choose_depth_limit(bms_num_members(candidate_relids));

        /* must always parameterize by lateral refs */
        Relids param_relids = bms_copy(rel->lateral_relids);

        add_bioseq_column_scan_paths_rec(
            root, rel, rte, param_relids, candidate_relids, depth_limit);

        /* Once a CustomPath claims a restriction, remove only alternatives that
         * would execute that same RestrictInfo through the SQL function.  This is
         * structural selection; costs remain honest and nonnegative. */
        postgres_list<RestrictInfo> owners;
        for (Path* candidate : postgres_list<Path>(rel->pathlist)) {
            if (!IsA(candidate, CustomPath))
                continue;
            CustomPath* custom = reinterpret_cast<CustomPath*>(candidate);
            bool is_mmsearch_owner = false;
            for (Node* private_node : postgres_list<Node>(custom->custom_private)) {
                if (private_node == nullptr || !IsA(private_node, List))
                    continue;
                List* tag = reinterpret_cast<List*>(private_node);
                if (list_length(tag) == 3 && IsA(reinterpret_cast<Node*>(linitial(tag)), Integer) &&
                    intVal(linitial(tag)) == k_bioseq_mmsearch_owner_magic) {
                    is_mmsearch_owner = true;
                    break;
                }
            }
            if (!is_mmsearch_owner)
                continue;
            for (RestrictInfo* owner : postgres_list<RestrictInfo>(custom->custom_restrictinfo)) {
                if (!list_member_ptr(owners.get(), owner))
                    owners.append(owner);
            }
        }
        // Removes paths that execute a restriction now owned by a custom scan.
        auto prune_owned = [rel, &owners](List* paths) {
            postgres_list<Path> survivors;
            for (Path* candidate : postgres_list<Path>(paths)) {
                bool remove = false;
                for (RestrictInfo* owner : owners) {
                    const bool executes_owner =
                        list_member_ptr(rel->baserestrictinfo, owner) ||
                        (candidate->param_info != nullptr &&
                         list_member_ptr(candidate->param_info->ppi_clauses, owner));
                    const bool owns_restriction =
                        IsA(candidate, CustomPath) &&
                        list_member_ptr(
                            reinterpret_cast<CustomPath*>(candidate)->custom_restrictinfo, owner);
                    if (executes_owner && !owns_restriction) {
                        remove = true;
                        break;
                    }
                }
                if (!remove)
                    survivors.append(candidate);
            }
            return survivors.release();
        };
        if (!owners.is_empty()) {
            rel->pathlist = prune_owned(rel->pathlist);
            rel->partial_pathlist = prune_owned(rel->partial_pathlist);
        }

        bioseq_log<logging_level::debug>("[bioseq] pg: AddBioseqColumnScanPaths() returning");
    }

    /**
     * @brief Appends a human-readable parameterization set to caller-owned storage.
     *
     * Returns the string representation of the set of
     * rels given in paramRelids.
     *
     * Takes a StringInfo so that it doesn't return palloc'd memory. This makes it
     * easy to call this function as an argument to ereport(), such that it won't
     * be evaluated unless the message is going to be output somewhere.
     */
    char* parameterization_as_string(PlannerInfo* root, Relids param_relids, StringInfo buf) {
        bioseq::postgres::support::postgres_bitmapset param_relids_view{param_relids};

        if (param_relids_view.is_empty()) {
            return "unparameterized";
        }

        bool first_time = true;
        appendStringInfoString(buf, "parameterized by rels {");
        for (int relid : param_relids_view) {
            RangeTblEntry* rte = root->simple_rte_array[relid];
            const char* relname = quote_identifier(rte->eref->aliasname);

            appendStringInfo(buf, "%s%s", first_time ? "" : ", ", relname);

            if (relname != rte->eref->aliasname) {
                pfree(const_cast<char*>(relname));
            }

            first_time = false;
        }
        appendStringInfoString(buf, "}");
        return buf->data;
    }

    /** @brief Stateless context used while searching an expression for PARAM_EXEC nodes. */
    struct exec_params_probe_state {};

    /** @brief Stops an expression walk at the first PARAM_EXEC node. */
    struct exec_params_visitor {
        /** @brief Examines one expression node for an executor parameter. */
        [[nodiscard]] pg_walk_step
        operator()(pg_expression_walk_cursor, pg_node_view node, exec_params_probe_state&) const {
            if (Param* param = node.try_cast<Param>()) {
                if (param->paramkind == PARAM_EXEC) {
                    return pg_walk_step::stop;
                }
            }
            return pg_walk_step::descend;
        }
    };

    /** @brief Reports whether an expression contains any PARAM_EXEC node. */
    [[nodiscard]] bool contains_exec_params(Node* node) {
        exec_params_probe_state state;
        exec_params_visitor visitor;
        return walk_pg_expression_including_root(node, state, visitor);
    }

    /**
     * @brief Maps parent permission columns to an inheritance child's attribute numbers.
     *
     * Adapted from PostgreSQL 16's static fixup_inherited_columns helper.
     *
     * When user is querying on a table with children, it implicitly accesses
     * child tables also. So, we also need to check security label of child
     * tables and columns, but there is no guarantee attribute numbers are
     * same between the parent and children.
     * It returns a bitmapset which contains attribute number of the child
     * table based on the given bitmapset of the parent.
     */
    Bitmapset* fixup_inherited_columns(Oid parent_id, Oid child_id, Bitmapset* columns) {
        bioseq::postgres::support::postgres_bitmapset result;

        /*
         * obviously, no need to do anything here
         */
        if (parent_id == child_id) {
            return columns;
        }

        for (int index : bioseq::postgres::support::postgres_bitmapset(columns)) {
            /* bit numbers are offset by FirstLowInvalidHeapAttributeNumber */
            AttrNumber attno = index + FirstLowInvalidHeapAttributeNumber;

            /*
             * whole-row-reference shall be fixed-up later
             */
            if (attno == InvalidAttrNumber) {
                result.add(index);
                continue;
            }

            char* attname = get_attname(parent_id, attno, false);
            attno = get_attnum(child_id, attname);
            if (attno == InvalidAttrNumber) {
                bioseq_log<logging_level::error, logging_target::postgres>(
                    "cache lookup failed for attribute {} of relation {}", attname, child_id);
            }

            result.add(attno - FirstLowInvalidHeapAttributeNumber);

            pfree(attname);
        }

        return result.release();
    }

    /** @brief Traces relation metadata referenced while diagnosing planner costs. */
    struct cost_test_visitor {
        /** @brief Logs the range-table entry associated with each visited Var. */
        [[nodiscard]] pg_walk_step
        operator()(pg_expression_walk_cursor, pg_node_view node, Query& parse) const {
            bioseq_log<logging_level::debug>("[bioseq] pg: cost_test_visitor() entered");
            if (Var* var_node = node.try_cast<Var>()) {
                const auto var_no = var_node->varno;
                List* range_table = parse.rtable;
                if (range_table == nullptr) {
                    bioseq_log<logging_level::debug>(
                        "[bioseq] pg: cost_test_visitor() var rtable is null");
                } else if (!bioseq_is_concrete_rte_varno(var_no) ||
                           var_no > static_cast<Index>(list_length(range_table))) {
                    bioseq_log<logging_level::debug>("[bioseq] pg: cost_test_visitor() var varno "
                                                     "out of bounds varno={} rtable_len={}",
                                                     var_no,
                                                     list_length(range_table));
                } else {
                    RangeTblEntry* rte = rt_fetch(var_no, range_table);
                    bioseq_log<logging_level::debug>(
                        "[bioseq] pg: cost_test_visitor() rte_kind={} relid={}",
                        static_cast<int>(rte->rtekind),
                        static_cast<int>(rte->relid));
                }
            }

            return pg_walk_step::descend;
        }
    };

    /**
     * @brief Creates and costs a custom scan path with the requested outer relations.
     *
     * This follows foreign-path construction while supplying parameterization
     * through param_info and custom_private.
     */
    Path* add_bioseq_column_scan_path(PlannerInfo* root,
                                      RelOptInfo* rel,
                                      RangeTblEntry* rte,
                                      Relids param_relids) {
        bioseq_log<logging_level::debug>("[bioseq] pg: AddBioseqColumnScanPath() entered");
        /*
         * Must return a CustomPath, not a larger structure containing a
         * CustomPath as the first field. Otherwise, nodeToString() will fail to
         * output the additional fields.
         */
        CustomPath* cpath = makeNode(CustomPath);

        cpath->methods = bioseq_column_scan_path_methods();

        /*
         * populate generic path information
         */
        Path* path = &cpath->path;
        path->pathtype = T_CustomScan;
        path->parent = rel;
        path->pathtarget = copy_pathtarget(rel->reltarget);

        const bioseq_stub_oids& stub_oids = get_cached_stub_oids();

        bioseq_log<logging_level::debug>(
            "[bioseq] pg: AddBioseqColumnScanPath() mmsearch_oid={} score_oid={} cigar_oid={} "
            "query_start_oid={} symbol_count_oid={}",
            stub_oids.mmsearch_oid,
            stub_oids.score_oid,
            stub_oids.cigar_oid,
            stub_oids.query_start_oid,
            stub_oids.symbol_count_oid);

        bioseq_log<logging_level::debug>(
            "[bioseq] pg: AddBioseqColumnScanPath() walking pathtarget expressions");
        cost_test_visitor cost_visitor;
        for (auto expr : postgres_list<Node>(path->pathtarget->exprs)) {
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: AddBioseqColumnScanPath() pathtarget expression");
            walk_pg_expression_including_root(expr, *root->parse, cost_visitor);
        }
        bioseq_log<logging_level::debug>(
            "[bioseq] pg: AddBioseqColumnScanPath() pathtarget walk complete");

        /* columnar scans are not parallel-aware, but they are parallel-safe */
        path->parallel_safe = rel->consider_parallel;

        path->param_info = get_baserel_parampathinfo(root, rel, param_relids);

        /*
         * Usable clauses for this parameterization exist in baserestrictinfo and
         * ppi_clauses.
         */
        postgres_list<RestrictInfo> clause_references;
        for (RestrictInfo* rinfo : postgres_list<RestrictInfo>(rel->baserestrictinfo)) {
            if (!list_member_ptr(clause_references.get(), rinfo)) {
                clause_references.append(rinfo);
            }
        }
        if (path->param_info != nullptr) {
            for (RestrictInfo* rinfo : postgres_list<RestrictInfo>(path->param_info->ppi_clauses)) {
                if (!list_member_ptr(clause_references.get(), rinfo)) {
                    clause_references.append(rinfo);
                }
            }
        }
        owned_mmsearch_restriction owned_mmsearch = choose_owned_mmsearch_restriction(
            root,
            rel,
            rte,
            clause_references.get(),
            path->param_info == nullptr ? NIL : path->param_info->ppi_clauses,
            param_relids,
            stub_oids);
        std::optional<base_local_mmsearch_ownership> owned_mmsearch_resolution;
        if (owned_mmsearch.normalized_expr != nullptr) {
            List* candidate_expressions =
                list_concat(list_make1(owned_mmsearch.normalized_expr),
                            list_copy(owned_mmsearch.extra_normalized_exprs));
            bool exact = true;
            Node* canonical_target = nullptr;
            for (FuncExpr* candidate : postgres_list<FuncExpr>(candidate_expressions)) {
                auto resolution =
                    resolve_base_local_mmsearch_ownership(candidate, rel, param_relids, stub_oids);
                Node* target = candidate == nullptr || list_length(candidate->args) < 1
                                   ? nullptr
                                   : reinterpret_cast<Node*>(linitial(candidate->args));
                if (!resolution.has_value() ||
                    (canonical_target != nullptr && !equal(canonical_target, target))) {
                    exact = false;
                    break;
                }
                bioseq::simd::minimap2_parameters option_probe = build_params_from_gucs();
                bioseq::indexing::index_read_requirement requirement_probe =
                    bioseq::indexing::index_read_requirement::allow;
                (void)apply_aligner_options_override(
                    candidate, option_probe, requirement_probe, stub_oids);
                if (resolution->any_evaluated_operand &&
                    requirement_probe ==
                        bioseq::indexing::index_read_requirement::require_compatible_persisted) {
                    exact = false;
                    break;
                }
                if (canonical_target == nullptr) {
                    canonical_target = target;
                    owned_mmsearch_resolution = *resolution;
                }
            }
            /* The bounded two-call fallback has one target capture and one query
             * configuration.  Effectful/evaluated operands would either be shared
             * across SQL occurrences or duplicated, so keep that contract raw-only. */
            if (exact && list_length(candidate_expressions) > 1 &&
                owned_mmsearch_resolution.has_value() &&
                owned_mmsearch_resolution->any_evaluated_operand) {
                exact = false;
            }
            list_free(candidate_expressions);
            if (!exact) {
                owned_mmsearch = {};
                owned_mmsearch_resolution.reset();
            }
        }

        bioseq_log<logging_level::debug>(
            "[bioseq] pg: AddBioseqColumnScanPath() iterating allClauses (no-op pass for tracing)");
        for (auto restrict_info : clause_references) {
            (void)restrict_info;
        }

        postgres_list<RestrictInfo> all_clauses(
            filter_pushdown_clauses(root, rel, list_copy(clause_references.get())));
        List* owned_mmsearch_restrictions =
            owned_mmsearch.restriction == nullptr
                ? NIL
                : list_concat(list_make1(owned_mmsearch.restriction),
                              list_copy(owned_mmsearch.extra_restrictions));
        List* owned_mmsearch_exprs =
            owned_mmsearch.normalized_expr == nullptr
                ? NIL
                : list_concat(list_make1(owned_mmsearch.normalized_expr),
                              list_copy(owned_mmsearch.extra_normalized_exprs));
        if (owned_mmsearch.restriction != nullptr) {
            all_clauses = postgres_list<RestrictInfo>{
                restrictions_without_owned_mmsearch(all_clauses.release(),
                                                    owned_mmsearch_restrictions,
                                                    /*restrictions_are_private_copies=*/true)};
            /* Alignment statistics are backend-local and have no DSM aggregation.
             * A worker-local snapshot must not be exposed as statement statistics,
             * so every alignment-owning path is parallel unsafe. */
            path->parallel_safe = false;
        }

        slice_project_ir_builder base_operand_sources{
            .oids = &stub_oids,
            .command_type = CMD_SELECT,
            .planner_root = root,
            .target_leaf_support_relid = rel->relid,
            .source_bindings_only = true,
        };
        const bool needs_evaluated_operands = owned_mmsearch_resolution.has_value() &&
                                              owned_mmsearch_resolution->any_evaluated_operand;
        // Adds a distinct base Var needed to evaluate an owned operand or residual qual.
        auto append_support_var = [&base_operand_sources](Var* variable) {
            if (variable == nullptr)
                return;
            for (Var* existing :
                 postgres_list<Var>(base_operand_sources.target_leaf_support_vars)) {
                if (equal(existing, variable))
                    return;
            }
            base_operand_sources.target_leaf_support_vars =
                lappend(base_operand_sources.target_leaf_support_vars, variable);
        };
        if (needs_evaluated_operands) {
            // Registers an evaluated operand and the physical sources that keep it valid.
            auto register_operand = [&base_operand_sources, &append_support_var](
                                        const base_local_mmsearch_operand_resolution& operand) {
                const std::uint32_t producer =
                    slice_project_build_ir_node(base_operand_sources, operand.expression);
                slice_project_note_source_binding(
                    base_operand_sources, operand.expression, producer);
                for (Var* source : postgres_list<Var>(operand.sequence_vars)) {
                    append_support_var(source);
                }
            };
            register_operand(owned_mmsearch_resolution->target);
            if (owned_mmsearch_resolution->local_query.has_value()) {
                register_operand(*owned_mmsearch_resolution->local_query);
            }
            /* Residual quals execute before operand ExprStates.  Discover them from
             * the full baserestrictinfo + parameterized-clause set, not allClauses:
             * FilterPushdownClauses deliberately omits executor-only quals, while
             * PlanCustomPath installs those full residuals in scan.plan.qual. */
            List* executable_residual_restrictions =
                restrictions_without_owned_mmsearch(clause_references.get(),
                                                    owned_mmsearch_restrictions,
                                                    /*restrictions_are_private_copies=*/false);
            constexpr int pull_flags =
                PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
            for (RestrictInfo* residual :
                 postgres_list<RestrictInfo>(executable_residual_restrictions)) {
                if (residual == nullptr || residual->clause == nullptr)
                    continue;
                List* variables =
                    pull_var_clause(reinterpret_cast<Node*>(residual->clause), pull_flags);
                for (Var* variable : postgres_list<Var>(variables)) {
                    if (variable == nullptr || variable->varno != rel->relid ||
                        variable->varattno <= 0 || variable->varlevelsup != 0 ||
                        !bms_is_empty(variable->varnullingrels) ||
                        variable->vartype != stub_oids.bioseq_seq_type_oid)
                        continue;
                    append_support_var(variable);
                    bool already_bound = false;
                    for (auto const& binding : base_operand_sources.source_bindings) {
                        if (equal(binding.first, variable)) {
                            already_bound = true;
                            break;
                        }
                    }
                    if (!already_bound) {
                        base_operand_sources.source_bindings.emplace_back(
                            reinterpret_cast<Node*>(copyObject(variable)), 0u);
                    }
                }
                list_free(variables);
            }
            list_free(executable_residual_restrictions);
        }

        /*
         * Plain clauses may contain extern params, but not exec params, and can
         * be evaluated at init time or rescan time. Track them in another list
         * that is a subset of allClauses.
         *
         * Note: although typically baserestrictinfo contains plain clauses,
         * that's not always true. It can also contain a qual referencing a Var at
         * a higher query level, which can be turned into an exec param, and
         * therefore it won't be a plain clause.
         */
        postgres_list<RestrictInfo> plain_clauses;
        for (auto rinfo : all_clauses) {
            if (bms_is_subset(rinfo->required_relids, rel->relids) &&
                !contains_exec_params(reinterpret_cast<Node*>(rinfo->clause))) {
                plain_clauses.append(rinfo);
            }
        }

        /*
         * We can't make our own CustomPath structure, so we need to put
         * everything in the custom_private list. To keep the two lists separate,
         * we make them sublists in a 2-element list.
         */
        if (bioseq::settings::planner::qual_pushdown::enabled) {
            cpath->custom_private =
                list_make2(copyObject(plain_clauses.get()), copyObject(all_clauses.get()));
        } else {
            cpath->custom_private = list_make2(NIL, NIL);
        }
        if (owned_mmsearch.restriction != nullptr) {
            cpath->custom_restrictinfo = list_copy(owned_mmsearch_restrictions);
            cpath->custom_private = lappend(cpath->custom_private,
                                            list_make3(makeInteger(k_bioseq_mmsearch_owner_magic),
                                                       makeInteger(k_bioseq_mmsearch_owner_version),
                                                       copyObject(owned_mmsearch_exprs)));
        } else {
            cpath->custom_restrictinfo = NIL;
        }

        int number_of_columns_read = 0;
        if (rte->perminfoindex > 0) {
            /*
             * If perminfoindex > 0, that means that this relation's permission info
             * is directly found in the list of rteperminfos of the Query(root->parse)
             * So, all we have to do here is retrieve that info.
             */
            RTEPermissionInfo* perminfo = getRTEPermissionInfo(root->parse->rteperminfos, rte);
            number_of_columns_read = bms_num_members(perminfo->selectedCols);
        } else {
            /*
             * If perminfoindex = 0, that means we are skipping the check for permission info
             * for this relation, which means that it's either a partition or an inheritance child.
             * In these cases, we need to access the permission info of the top parent of this
             * relation. After thorough checking, we found that the index of the top parent pointing
             * to the correct range table entry in Query's range tables (root->parse->rtable) is
             * found under RelOptInfo rel->top_parent->relid. For reference, check
             * expand_partitioned_rtentry and expand_inherited_rtentry PG functions
             */
            Assert(rel->top_parent);
            RangeTblEntry* parent_rte = rt_fetch(rel->top_parent->relid, root->parse->rtable);
            RTEPermissionInfo* perminfo =
                getRTEPermissionInfo(root->parse->rteperminfos, parent_rte);
            number_of_columns_read = bms_num_members(
                fixup_inherited_columns(perminfo->relid, rte->relid, perminfo->selectedCols));
        }
        int number_of_clauses_pushed = all_clauses.size();

        /* Queries that contain only aggregate with STAR doesn't have any
         * selectedCols so we should consider at least one column that needs
         * read (for better cost calculation).
         */
        if (number_of_columns_read == 0)
            number_of_columns_read = 1;

        cost_bioseq_column_scan(
            root, rel, rte->relid, cpath, number_of_columns_read, number_of_clauses_pushed);

        for (FuncExpr* owned_expr : postgres_list<FuncExpr>(owned_mmsearch_exprs)) {
            QualCost operand_cost{};
            List* operand_expressions = mmsearch_operand_cost_exprs(owned_expr, stub_oids);
            cost_qual_eval(&operand_cost, operand_expressions, root);
            postgres_list<Node> operands(owned_expr->args);
            const std::uint64_t target_symbols =
                estimate_mmsearch_operand_symbols(operands.get_first(), stub_oids);
            const std::uint64_t query_symbols =
                estimate_mmsearch_operand_symbols(operands.get_second(), stub_oids);
            const std::uint64_t admitted_bytes =
                bioseq::common::saturating_add(target_symbols, query_symbols);
            bioseq::simd::minimap2_parameters planned_params = build_params_from_gucs();
            bioseq::indexing::index_read_requirement planned_requirement =
                bioseq::indexing::index_read_requirement::allow;
            (void)apply_aligner_options_override(
                owned_expr, planned_params, planned_requirement, stub_oids);
            if (owned_mmsearch_resolution.has_value() &&
                owned_mmsearch_resolution->any_evaluated_operand) {
                planned_requirement = bioseq::indexing::index_read_requirement::require_absent;
            }
            const owned_mmsearch_cost alignment = estimate_owned_mmsearch_cost(
                target_symbols,
                query_symbols,
                path->rows,
                planned_requirement != bioseq::indexing::index_read_requirement::require_absent,
                static_cast<std::uint32_t>(std::min<std::size_t>(
                    k_bioseq_runtime_buffer_reserve, (std::numeric_limits<std::uint32_t>::max)())),
                admitted_bytes);
            const Cost owner_startup = std::max<Cost>(0, operand_cost.startup + alignment.startup);
            const Cost owner_per_row =
                std::max<Cost>(0, operand_cost.per_tuple + alignment.per_input_tuple);
            path->startup_cost += owner_startup;
            path->total_cost += owner_startup + owner_per_row * std::max(0.0, path->rows);
        }
        if (!std::isfinite(path->startup_cost) || path->startup_cost < 0) {
            path->startup_cost = 0;
        }
        if (!std::isfinite(path->total_cost)) {
            path->total_cost = std::numeric_limits<Cost>::max() / 4.0;
        }
        path->total_cost = std::max(path->startup_cost, path->total_cost);

        bioseq_report_pg_lazy(
            planner_debug_level(), [rte, root, param_relids, number_of_clauses_pushed] {
                StringInfoData buf;
                initStringInfo(&buf);
                bioseq_report_pg(planner_debug_level())
                    .with_message("columnar planner: adding CustomScan path for {}",
                                  rte->eref->aliasname)
                    .with_detail("{}; {} clauses pushed down",
                                 parameterization_as_string(root, param_relids, &buf),
                                 number_of_clauses_pushed);
            });

        if (needs_evaluated_operands) {
            Path* private_path = slice_project_clone_mutable_path_spine(path);
            bool tagged_source = false;
            private_path = slice_project_tag_owned_path_tree(
                private_path, base_operand_sources, tagged_source);
            if (!tagged_source) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqScan evaluated mmsearch operand has no source provider")));
            }
            return private_path;
        }
        return path;
    }

    /** @brief Add Bioseq scan paths for a base relation while preserving the prior hook. */
    void bioseq_column_set_rel_pathlist_hook(PlannerInfo* root,
                                             RelOptInfo* rel,
                                             Index rti,
                                             RangeTblEntry* rte) {
        bioseq_log<logging_level::debug>("[bioseq] pg: BioseqColumnSetRelPathlistHook() entered");
        /* call into previous hook if assigned */
        if (previous_set_rel_pathlist_hook) {
            previous_set_rel_pathlist_hook(root, rel, rti, rte);
        }

        if (!OidIsValid(rte->relid) || rte->rtekind != RTE_RELATION || rte->inh) {
            /* some calls to the pathlist hook don't have a valid relation set. Do nothing */
            return;
        }

        /*
         * Here we want to inspect if this relation pathlist hook is accessing a columnar table.
         * If that is the case we want to insert an extra path that pushes down the projection
         * into the scan of the table to minimize the data read.
         */
        Relation relation = RelationIdGetRelation(rte->relid);
        if (relation->rd_tableam == column_table_access_method()) {
            if (rte->tablesample != nullptr) {
                bioseq_report<logging_level::error, logging_target::postgres>()
                    .with_sql_error_code(ERRCODE_FEATURE_NOT_SUPPORTED)
                    .with_message("sample scans not supported on columnar tables");
            }

            /*
             * There are cases where IndexPath is normally more preferrable over
             * SeqPath for heapAM but not for columnarAM. In such cases, an
             * IndexPath could wrongly dominate a SeqPath based on the costs
             * estimated by postgres earlier. For this reason, here we manually
             * create a SeqPath, estimate the cost based on columnarAM and append
             * to pathlist.
             *
             * Before doing that, we first re-cost all the existing paths so that
             * add_path makes correct cost comparisons when appending our SeqPath.
             */
            cost_bioseq_column_paths(root, rel, rte->relid);

            Path* seq_path = create_bioseq_column_seq_scan_path(root, rel, rte->relid);
            add_path(rel, seq_path);

            if (bioseq::settings::planner::custom_scan::enabled) {
                /*
                 * When columnar custom scan is enabled
                 * (bioseq.planner.custom_scan.enabled),
                 * we only consider BioseqColumnScanPath's & IndexPath's. For this reason,
                 * we remove other paths and re-estimate IndexPath costs to make accurate
                 * comparisons between them.
                 *
                 * Even more, we might calculate an equal cost for a
                 * BioseqColumnCustomScan and a SeqPath if we are reading all columns
                 * of given table since we don't consider chunk group filtering
                 * when costing BioseqColumnCustomScan.
                 * In that case, if we don't remove SeqPath's, we might wrongly choose
                 * SeqPath thinking that its cost would be equal to BioseqColumnCustomScan.
                 */
                remove_paths_by_predicate(rel, is_not_index_path);
                add_bioseq_column_scan_paths(root, rel, rte);
                slice_project_try_install_base_owner(root, rel, relation);
            }
        } else {
            /* External logical relations use a distinct table AM. The
             * always-owned slot returns false (this columnar hook simply falls
             * through) unless the external provider stack is compiled in and has
             * registered a path builder, in which case the builder replaces the
             * physical scan with the external CustomScan. */
            (void)bioseq::postgres::external::try_add_scan_paths(root, rel, rte, relation);
        }
        bioseq_log<logging_level::debug>(
            "[bioseq] pg: BioseqColumnSetRelPathlistHook() relation_close");
        RelationClose(relation);
    }

    /** @brief Apply table-AM planner capabilities after the prior relation-info hook. */
#if PG_VERSION_NUM >= 190000
    void bioseq_column_build_simple_rel_hook(PlannerInfo* root,
                                             RelOptInfo* rel,
                                             RangeTblEntry* rte) {
        if (previous_build_simple_rel_hook) {
            previous_build_simple_rel_hook(root, rel, rte);
        }
        // PG19 moved this hook after relation setup and also calls it for
        // functions, subqueries and CTEs. Only tables have a catalog OID.
        if (rte->rtekind != RTE_RELATION) {
            return;
        }
        const Oid relation_object_id = rte->relid;
#else
    void bioseq_column_get_relation_info_hook(PlannerInfo* root,
                                              Oid relation_object_id,
                                              bool inhparent,
                                              RelOptInfo* rel) {
        if (previous_get_relation_info_hook) {
            previous_get_relation_info_hook(root, relation_object_id, inhparent, rel);
        }
#endif
        bioseq_log<logging_level::debug>("[bioseq] pg: BioseqColumnRelationInfoHook() entered");

        if (relation_oid_uses_column_table_access_method(relation_object_id)) {
            /* disable parallel query */
            rel->rel_parallel_workers = 0;

            /* disable index-only scan */
            for (auto index_opt_info : postgres_list<IndexOptInfo>(rel->indexlist)) {
                memset(index_opt_info->canreturn, false, index_opt_info->ncolumns * sizeof(bool));
            }
        }
    }

    /** @brief Install Bioseq planner hooks and register custom-scan callback tables. */
    void bioseq_customscan_init() {
        previous_set_rel_pathlist_hook = set_rel_pathlist_hook;
        set_rel_pathlist_hook = bioseq_column_set_rel_pathlist_hook;
        previous_set_join_pathlist_hook = set_join_pathlist_hook;
        set_join_pathlist_hook = bioseq_batch_join_set_join_pathlist_hook;

#if PG_VERSION_NUM >= 190000
        previous_build_simple_rel_hook = build_simple_rel_hook;
        build_simple_rel_hook = bioseq_column_build_simple_rel_hook;
#else
        previous_get_relation_info_hook = get_relation_info_hook;
        get_relation_info_hook = bioseq_column_get_relation_info_hook;
#endif

        RegisterCustomScanMethods(bioseq_column_scan_scan_methods());
        RegisterCustomScanMethods(bioseq_batch_join_scan_methods());
        RegisterCustomScanMethods(&bioseq_slice_project_scan_methods());
        bioseq_register_slice_project_upper_path_provider(bioseq_slice_project_create_upper_paths);
        bioseq_register_batch_sort_source_providers(batch_sort_tag_source_path,
                                                    batch_sort_attach_source_routes);
    }

} // export custom-scan partition declarations

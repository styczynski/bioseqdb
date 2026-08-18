/**
 * @file
 * @brief Declares planner, executor, hook, EXPLAIN, and test callback contracts.
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

export module bioseq.postgres.table:custom_scan_semantic_payload_callback_contracts;

import std;
import bioseq.postgres.configuration.core;
import bioseq.indexing.type;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.pipeline.stats;
import bioseq.postgres.table.scan_state;
import bioseq.postgres.support;

import :custom_scan_runtime_support;
import :custom_scan_oid_expression_support;
import :custom_scan_slice_path_runtime_contract;
export import :custom_scan_execution_method_contracts;
export import :custom_scan_planner_method_contracts;

import :custom_scan_semantic_payload_payload_codec;
import :custom_scan_semantic_payload_aligner_semantics;

using namespace bioseq::postgres::support;

export {
    /**
     * @brief Carries placeholder and owner metadata through PostgreSQL's mutator callback.
     *
     * Passed to VarInjectorMutator through the void* context arg.
     * Bundles the allocated placeholder Consts with the resolved OIDs so
     * the mutator can dispatch on funcid -> Const* without a global.
     *
     * `current_relation` is used exclusively when rewriting
     * `symbol_count(Var, 'L')` call sites to resolve the column name
     * (attname) referenced by the Var — so the EXPLAIN output can say
     * "Fetched from meta: seq.symbol_count('A') AS ...".
     */
    struct var_injector_context {
        placeholder_constants* consts;
        const bioseq_stub_oids* oids;
        Relation current_relation;
        Index match_sequence_varno;
        AttrNumber match_sequence_attno;
        Node* match_target_recipe;
        List* match_target_lineage;
        List* match_helper_sites;
        int stable_match_id;
    };

    /** @brief Apply parallel-worker adjustments to an existing Bioseq scan path. */
    void adjust_bioseq_column_parallel_scan_cost(Path* path);
    /** @brief Cost a Bioseq custom scan from relation size, projection width, and clauses. */
    void cost_bioseq_column_scan(PlannerInfo* root,
                                 RelOptInfo* rel,
                                 Oid relation_id,
                                 CustomPath* cpath,
                                 int number_of_columns_read,
                                 int n_clauses);

    /** @brief Construct a Bioseq base-scan path for one required-outer relation set. */
    Path* add_bioseq_column_scan_path(PlannerInfo* root,
                                      RelOptInfo* rel,
                                      RangeTblEntry* rte,
                                      Relids required_relids);

    /** @brief Estimate the per-stripe cost of reading the requested column count. */
    Cost bioseq_column_per_stripe_scan_cost(RelOptInfo* rel,
                                            Oid relation_id,
                                            int number_of_columns_read);
    /** @brief Count physical stripes belonging to a Bioseq table. */
    uint64_t bioseq_column_table_stripe_count(Oid relation_id);
    /** @brief Convert the selected Bioseq scan path into its executable CustomScan plan. */
    Plan* bioseq_column_scan_path_plan_custom_path(PlannerInfo* root,
                                                   RelOptInfo* rel,
                                                   CustomPath* best_path,
                                                   List* tlist,
                                                   List* clauses,
                                                   List* custom_plans);
    /** @brief Allocate executor state for a planned Bioseq base scan. */
    Node* bioseq_column_scan_create_custom_scan_state(CustomScan* cscan);
    /** @brief Convert a selected batched-join path into its executable plan. */
    Plan* bioseq_batch_join_plan_custom_path(PlannerInfo* root,
                                             RelOptInfo* rel,
                                             CustomPath* best_path,
                                             List* tlist,
                                             List* clauses,
                                             List* custom_plans);
    /** @brief Allocate executor state for a planned batched join. */
    Node* bioseq_batch_join_create_custom_scan_state(CustomScan* cscan);
    /** @brief Convert a selected per-row join path into its executable plan. */
    Plan* bioseq_per_row_join_plan_custom_path(PlannerInfo* root,
                                               RelOptInfo* rel,
                                               CustomPath* best_path,
                                               List* tlist,
                                               List* clauses,
                                               List* custom_plans);

    /** @brief Initialize a Bioseq scan within the executor query context. */
    void bioseq_column_scan_begin_custom_scan(CustomScanState* node, EState* estate, int eflags);

    /** @brief Produce the next tuple from a Bioseq base scan. */
    TupleTableSlot* bioseq_column_scan_exec_custom_scan(CustomScanState* node);
    /** @brief Release Bioseq scan resources before query-context teardown. */
    void bioseq_column_scan_end_custom_scan(CustomScanState* node);
    /** @brief Reset a Bioseq scan for a new executor pass. */
    void bioseq_column_scan_rescan_custom_scan(CustomScanState* node);
    /** @brief Render Bioseq scan planning and runtime properties for EXPLAIN. */
    void bioseq_column_scan_explain_custom_scan(CustomScanState* node,
                                                List* ancestors,
                                                ExplainState* es);
    /** @brief Initialize a per-row join and its child executor state. */
    void bioseq_per_row_join_begin_custom_scan(CustomScanState* node, EState* estate, int eflags);
    /** @brief Produce the next projected match from a per-row join. */
    TupleTableSlot* bioseq_per_row_join_exec_custom_scan(CustomScanState* node);
    /** @brief Release per-row join resources before query-context teardown. */
    void bioseq_per_row_join_end_custom_scan(CustomScanState* node);
    /** @brief Reset a per-row join and its child for another executor pass. */
    void bioseq_per_row_join_rescan_custom_scan(CustomScanState* node);
    /** @brief Render per-row join planning and execution properties for EXPLAIN. */
    void bioseq_per_row_join_explain_custom_scan(CustomScanState* node,
                                                 List* ancestors,
                                                 ExplainState* es);
    /** @brief Select the query-sketch route required by persisted-index policy. */
    [[nodiscard]] constexpr bioseq::simd::pipe::query_sketch_route query_sketch_plan_route_for(
        const bioseq::indexing::index_read_requirement requirement) noexcept {
        return requirement == bioseq::indexing::index_read_requirement::require_compatible_persisted
                   ? bioseq::simd::pipe::query_sketch_route::compatible_persisted_only
                   : bioseq::simd::pipe::query_sketch_route::recompute_capable;
    }
    /** @brief Render pipeline counters and selected alignment policy for EXPLAIN ANALYZE. */
    void
    explain_pipeline_execution_stats(const bioseq::pipeline_stats::pipeline_execution_stats& stats,
                                     ExplainState* es,
                                     const bioseq::simd::minimap2_parameters* aligner_params,
                                     const bioseq::simd::pipe::pipeline_description* pipeline_desc,
                                     bioseq::simd::pipe::query_sketch_route query_sketch_route,
                                     bool highway_dispatch_already_rendered);

    /** @brief Deparse pushed-down clauses into query-context-owned EXPLAIN text. */
    const char* bioseq_column_pushdown_clauses_str(List* context, List* clauses);
    /** @brief Deparse projected columns into query-context-owned EXPLAIN text. */
    const char* bioseq_column_projected_columns_str(List* context, List* projected_columns);
    /** @brief Build a deparse context rooted at an executor plan state. */
    List* set_deparse_context_planstate(List* dpcontext, Node* node, List* ancestors);

    /** @brief Return the physical attribute bitmap required by a Bioseq scan. */
    List* bioseq_column_var_needed(scan_state* bioseq_scan_state);

    /* saved hook value in case of unload */
    set_rel_pathlist_hook_type previous_set_rel_pathlist_hook = nullptr;
    set_join_pathlist_hook_type previous_set_join_pathlist_hook = nullptr;
    get_relation_info_hook_type previous_get_relation_info_hook = nullptr;

    /** @brief Return the configured PostgreSQL planner diagnostic severity. */
    [[nodiscard]] int planner_debug_level() noexcept {
        return static_cast<int>(bioseq::settings::planner::diagnostics::log_level.get_value());
    }

    /** @brief Arm one backend-local SliceProject DSM diagnostic fixture. */
    void bioseq_arm_slice_dsm_fixture_for_test(std::string_view mode) {
#ifdef BIOSEQ_TEST_INDEXES
        int requested_mode = k_slice_dsm_test_none;
        if (mode == "incomplete") {
            requested_mode = k_slice_dsm_test_incomplete;
        } else if (mode == "malformed") {
            requested_mode = k_slice_dsm_test_malformed;
        } else if (mode != "none") {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("unknown SliceProject DSM diagnostic fixture"),
                     errdetail("fixture=%.*s", static_cast<int>(mode.size()), mode.data()),
                     errhint("Expected incomplete, malformed, or none.")));
        }
        if (requested_mode == k_slice_dsm_test_none) {
            pending_slice_dsm_test_fixture = k_slice_dsm_test_none;
            return;
        }
        if (pending_slice_dsm_test_fixture != k_slice_dsm_test_none) {
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("a SliceProject DSM diagnostic fixture is already armed"),
                     errhint("Run the analyzed SliceProject or disarm it with fixture 'none'.")));
        }
        pending_slice_dsm_test_fixture = requested_mode;
#else
        (void)mode;
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("SliceProject DSM diagnostics require a test build"),
                 errdetail("The extension was built without BIOSEQ_TEST_INDEXES.")));
#endif
    }

    /** Returns the stable PostgreSQL executor callback table for batched joins. */
    [[nodiscard]] const CustomExecMethods* bioseq_batch_join_execute_methods() noexcept;

} // export custom-scan partition declarations

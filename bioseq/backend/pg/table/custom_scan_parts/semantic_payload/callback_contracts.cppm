/**
 * @file
 * @brief Declares planner, executor, hook, EXPLAIN, and test callback contracts.
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

export module bioseq.postgres.table:custom_scan_semantic_payload_callback_contracts;

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

import :custom_scan_runtime_support;
import :custom_scan_oid_expression_support;

import :custom_scan_semantic_payload_payload_codec;
import :custom_scan_semantic_payload_aligner_semantics;

using namespace bioseq::postgres::support;

export {
    /*
     * Context passed to VarInjectorMutator through the void* context arg.
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

    /* functions to cost paths in-place */
    void adjust_bioseq_column_parallel_scan_cost(Path* path);
    void cost_bioseq_column_scan(PlannerInfo* root,
                                 RelOptInfo* rel,
                                 Oid relation_id,
                                 CustomPath* cpath,
                                 int number_of_columns_read,
                                 int n_clauses);

    /* functions to add new paths */
    Path* add_bioseq_column_scan_path(PlannerInfo* root,
                                      RelOptInfo* rel,
                                      RangeTblEntry* rte,
                                      Relids required_relids);

    /* helper functions to be used when costing paths or altering them */
    Cost bioseq_column_per_stripe_scan_cost(RelOptInfo* rel,
                                            Oid relation_id,
                                            int number_of_columns_read);
    uint64_t bioseq_column_table_stripe_count(Oid relation_id);
    /* hooks and callbacks */
    Plan* bioseq_column_scan_path_plan_custom_path(PlannerInfo* root,
                                                   RelOptInfo* rel,
                                                   CustomPath* best_path,
                                                   List* tlist,
                                                   List* clauses,
                                                   List* custom_plans);
    Node* bioseq_column_scan_create_custom_scan_state(CustomScan* cscan);
    Plan* bioseq_batch_join_plan_custom_path(PlannerInfo* root,
                                             RelOptInfo* rel,
                                             CustomPath* best_path,
                                             List* tlist,
                                             List* clauses,
                                             List* custom_plans);
    Node* bioseq_batch_join_create_custom_scan_state(CustomScan* cscan);
    Plan* bioseq_per_row_join_plan_custom_path(PlannerInfo* root,
                                               RelOptInfo* rel,
                                               CustomPath* best_path,
                                               List* tlist,
                                               List* clauses,
                                               List* custom_plans);

    void bioseq_column_scan_begin_custom_scan(CustomScanState* node, EState* estate, int eflags);

    TupleTableSlot* bioseq_column_scan_exec_custom_scan(CustomScanState* node);
    void bioseq_column_scan_end_custom_scan(CustomScanState* node);
    void bioseq_column_scan_rescan_custom_scan(CustomScanState* node);
    void bioseq_column_scan_explain_custom_scan(CustomScanState* node,
                                                List* ancestors,
                                                ExplainState* es);
    void bioseq_per_row_join_begin_custom_scan(CustomScanState* node, EState* estate, int eflags);
    TupleTableSlot* bioseq_per_row_join_exec_custom_scan(CustomScanState* node);
    void bioseq_per_row_join_end_custom_scan(CustomScanState* node);
    void bioseq_per_row_join_rescan_custom_scan(CustomScanState* node);
    void bioseq_per_row_join_explain_custom_scan(CustomScanState* node,
                                                 List* ancestors,
                                                 ExplainState* es);
    [[nodiscard]] constexpr bioseq::simd::pipe::query_sketch_route query_sketch_plan_route_for(
        const bioseq::indexing::index_read_requirement requirement) noexcept {
        return requirement == bioseq::indexing::index_read_requirement::require_compatible_persisted
                   ? bioseq::simd::pipe::query_sketch_route::compatible_persisted_only
                   : bioseq::simd::pipe::query_sketch_route::recompute_capable;
    }
    void
    explain_pipeline_execution_stats(const bioseq::pipeline_stats::pipeline_execution_stats& stats,
                                     ExplainState* es,
                                     const bioseq::simd::minimap2_parameters* aligner_params,
                                     const bioseq::simd::pipe::pipeline_description* pipeline_desc,
                                     bioseq::simd::pipe::query_sketch_route query_sketch_route,
                                     bool highway_dispatch_already_rendered);

    /* helper functions to build strings for EXPLAIN */
    inline const char* bioseq_column_pushdown_clauses_str(List* context, List* clauses);
    inline const char* bioseq_column_projected_columns_str(List* context, List* projected_columns);
    List* set_deparse_context_planstate(List* dpcontext, Node* node, List* ancestors);

    /* other helpers */
    List* bioseq_column_var_needed(scan_state* bioseq_scan_state);

    /* saved hook value in case of unload */
    set_rel_pathlist_hook_type previous_set_rel_pathlist_hook = nullptr;
    set_join_pathlist_hook_type previous_set_join_pathlist_hook = nullptr;
    get_relation_info_hook_type previous_get_relation_info_hook = nullptr;

    [[nodiscard]] int planner_debug_level() noexcept {
        return static_cast<int>(bioseq::settings::planner::diagnostics::log_level.get_value());
    }

#ifdef BIOSEQ_TEST_INDEXES
    enum slice_project_dsm_test_fixture : int {
        k_slice_dsm_test_none = 0,
        k_slice_dsm_test_incomplete = 1,
        k_slice_dsm_test_malformed = 2,
    };

    /* Backend-local, one-shot diagnostic state.  The test hook arms one
     * mode; the next analyzed SliceProject takes it before running any fixture
     * code, so ERROR during the fixture cannot leave a latent injection behind. */
    int pending_slice_dsm_test_fixture = k_slice_dsm_test_none;
#endif

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

    /** Returns the stable PostgreSQL path callback table for base Bioseq scans. */
    [[nodiscard]] const CustomPathMethods* bioseq_column_scan_path_methods() noexcept;

    /** Returns the stable PostgreSQL scan callback table for base Bioseq scans. */
    [[nodiscard]] const CustomScanMethods* bioseq_column_scan_scan_methods() noexcept;

    /** Returns the stable PostgreSQL path callback table for batched joins. */
    [[nodiscard]] const CustomPathMethods* bioseq_batch_join_path_methods() noexcept;

    /** Returns the stable PostgreSQL scan callback table for batched joins. */
    [[nodiscard]] const CustomScanMethods* bioseq_batch_join_scan_methods() noexcept;

    /** Returns the stable PostgreSQL executor callback table for batched joins. */
    [[nodiscard]] const CustomExecMethods* bioseq_batch_join_execute_methods() noexcept;

    /** Returns the stable PostgreSQL path callback table for per-row joins. */
    [[nodiscard]] const CustomPathMethods* bioseq_per_row_join_path_methods() noexcept;

    /** Returns the stable PostgreSQL executor callback table for base Bioseq scans. */
    [[nodiscard]] const CustomExecMethods* bioseq_column_scan_execute_methods() noexcept;

} // export custom-scan partition declarations

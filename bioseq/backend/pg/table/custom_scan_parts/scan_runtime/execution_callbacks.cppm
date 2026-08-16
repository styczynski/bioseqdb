/**
 * @file
 * @brief Owns base-scan execution, cleanup, and rescan callbacks.
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

export module bioseq.postgres.table:custom_scan_scan_runtime_execution_callbacks;

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
import :custom_scan_join_paths;
import :custom_scan_scan_paths;
import :custom_scan_plan_builders;
import :custom_scan_match_ownership;
import :custom_scan_join_execution;

import :custom_scan_scan_runtime_symbol_count_materialization;
import :custom_scan_scan_runtime_source_lifecycle;
import :custom_scan_scan_runtime_tuple_production;

using namespace bioseq::postgres::support;

export {
    /** @brief Installs executor placeholders once and advances the base-scan state machine. */
    TupleTableSlot* custom_exec_scan(scan_state* bioseq_scan_state,
                                     ExecScanAccessMtd access_mtd,
                                     ExecScanRecheckMtd recheck_mtd) {

        // Testing custom exec scan
        bioseq_log<logging_level::debug>("[bioseq] pg: CustomExecScan() entered");

        ScanState* node = reinterpret_cast<ScanState*>(&bioseq_scan_state->custom_scan_state.ss);
        PlanState ps = node->ps;
        bioseq_scan_runtime& runtime = bioseq_runtime(bioseq_scan_state);

        bioseq_log<logging_level::debug>("[bioseq] pg: CustomExecScan() reading plan targetlist");
        auto targetlist = ps.plan->targetlist;
        bioseq_log<logging_level::debug>("[bioseq] pg: CustomExecScan() targetlist_is_null={}",
                                         targetlist == nullptr);
        bioseq_log<logging_level::debug>(
            "[bioseq] pg: CustomExecScan() targetlist iteration complete");

        if (runtime.placeholder_consts.dp_score_node == nullptr) {
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: CustomExecScan() allocating placeholder const nodes");

            /* INT4: byval, len=4, no collation. Used for the int32_t match_result
             * fields (dp_score, mapq, chain_score, dp_max_score, seed_count,
             * secondary_chain_score). */
            const auto int_placeholder = []() {
                return makeConst(INT4OID,
                                 -1,
                                 InvalidOid,
                                 4,
                                 static_cast<Datum>(static_cast<int32_t>(0)),
                                 /*constisnull=*/true,
                                 /*constbyval=*/true);
            };
            /* INT8: byval on LP64 (pg_type.typbyval for bigint is 't'), len=8.
             * Used for the eight sequence_size_t (uint64) coordinate/length
             * fields (query_len … block_length) so positions in sequences longer
             * than INT32_MAX are not truncated. Matches the SQL
             * DDL where those eight projections RETURN BIGINT. */
            const auto bigint_placeholder = []() {
                return makeConst(
                    INT8OID,
                    -1,
                    InvalidOid,
                    8,
                    bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(0)).raw(),
                    /*constisnull=*/true,
                    /*constbyval=*/true);
            };
            /* BOOL: byval, len=1, no collation. */
            const auto bool_placeholder = []() {
                return makeConst(BOOLOID,
                                 -1,
                                 InvalidOid,
                                 1,
                                 bioseq::postgres::support::pg_datum::from(false).raw(),
                                 /*constisnull=*/true,
                                 /*constbyval=*/true);
            };
            /* FLOAT4: byval, len=4, no collation. */
            const auto float_placeholder = []() {
                return makeConst(FLOAT4OID,
                                 -1,
                                 InvalidOid,
                                 4,
                                 bioseq::postgres::support::pg_datum::from(0.0F).raw(),
                                 /*constisnull=*/true,
                                 /*constbyval=*/true);
            };
            /* TEXT: byref, len=-1 (varlena), default collation. */
            const auto text_placeholder = []() {
                return makeConst(TEXTOID,
                                 -1,
                                 DEFAULT_COLLATION_OID,
                                 -1,
                                 bioseq::postgres::support::pg_datum::from_pointer(nullptr).raw(),
                                 /*constisnull=*/true,
                                 /*constbyval=*/false);
            };

            runtime.placeholder_consts.symbol_count_sites.clear();
            runtime.placeholder_consts.dp_score_node = int_placeholder();
            runtime.placeholder_consts.query_len_node = bigint_placeholder();
            runtime.placeholder_consts.query_start_node = bigint_placeholder();
            runtime.placeholder_consts.query_end_node = bigint_placeholder();
            runtime.placeholder_consts.target_len_node = bigint_placeholder();
            runtime.placeholder_consts.target_start_node = bigint_placeholder();
            runtime.placeholder_consts.target_end_node = bigint_placeholder();
            runtime.placeholder_consts.num_matches_node = bigint_placeholder();
            runtime.placeholder_consts.block_length_node = bigint_placeholder();
            runtime.placeholder_consts.edit_distance_node = bigint_placeholder();
            runtime.placeholder_consts.mapq_node = int_placeholder();
            runtime.placeholder_consts.chain_score_node = int_placeholder();
            runtime.placeholder_consts.dp_max_score_node = int_placeholder();
            runtime.placeholder_consts.seed_count_node = int_placeholder();
            runtime.placeholder_consts.secondary_chain_score_node = int_placeholder();
            runtime.placeholder_consts.is_reverse_node = bool_placeholder();
            runtime.placeholder_consts.is_primary_node = bool_placeholder();
            runtime.placeholder_consts.has_cigar_node = bool_placeholder();
            runtime.placeholder_consts.divergence_node = float_placeholder();
            runtime.placeholder_consts.cigar_node = text_placeholder();

            /*
             * Reset only the slot buffer; keep the mmsearch_context that
             * BeginCustomScan configured earlier for this scan.
             */
            runtime.buffer.clear_slots();

            const bioseq_stub_oids& oids_snapshot = get_cached_stub_oids();
            var_injector_context injector_ctx;
            injector_ctx.consts = &runtime.placeholder_consts;
            injector_ctx.oids = &oids_snapshot;
            injector_ctx.current_relation =
                bioseq_scan_state->custom_scan_state.ss.ss_currentRelation;
            injector_ctx.match_sequence_varno = runtime.match_sequence_varno;
            injector_ctx.match_sequence_attno = runtime.match_sequence_attno;
            injector_ctx.match_target_recipe = runtime.match_target_recipe;
            injector_ctx.match_target_lineage = runtime.match_target_lineage;
            injector_ctx.match_helper_sites = runtime.match_helper_sites;
            injector_ctx.stable_match_id = runtime.stable_match_id;

            runtime.newtargetlist = reinterpret_cast<List*>(
                var_injector(reinterpret_cast<Node*>(targetlist), &injector_ctx));
            derive_mmsearch_source_lifetime_roles(
                runtime,
                ps.plan,
                bioseq_scan_state->custom_scan_state.ss.ss_ScanTupleSlot->tts_tupleDescriptor);
        }

        bioseq_log<logging_level::debug>(
            "[bioseq] pg: CustomExecScan() dispatching to Inner_CustomExecScan");

        auto tuple_slot = inner_custom_exec_scan(bioseq_scan_state, access_mtd, recheck_mtd);
        return tuple_slot;
    }

    /**
     * @brief Opens the table-AM scan lazily and fetches its next scalar or vector slot.
     * @return A populated scan slot, or null when the table scan is exhausted.
     */
    TupleTableSlot* bioseq_column_scan_next(scan_state* bioseq_scan_state) {
        bioseq_log<logging_level::debug>("[bioseq] pg: BioseqColumnScanNext() entered");
        CustomScanState* node = bioseq_scan_state->as_custom_scan_state();

        /*
         * get information from the estate and scan state
         */
        TableScanDesc scandesc = node->ss.ss_currentScanDesc;
        EState* estate = node->ss.ps.state;
        ScanDirection direction = estate->es_direction;
        const bool is_vectorization_enabled = bioseq_scan_state->vectorization.is_enabled;

        TupleTableSlot* slot = nullptr;

        if (is_vectorization_enabled)
            slot = bioseq_scan_state->vectorization.scan_vector_slot;
        else
            slot = node->ss.ss_ScanTupleSlot;

        if (scandesc != nullptr && scandesc->rs_rd == nullptr) {
            bioseq_log<logging_level::debug>("[bioseq] pg: BioseqColumnScanNext() found stale scan "
                                             "descriptor with null relation; reopening");
            node->ss.ss_currentScanDesc = nullptr;
            scandesc = nullptr;
        }

        if (scandesc == nullptr) {
            /* the columnar access method does not use the flags, they are specific to heap */
            uint32_t flags = 0;

            /*
             * We reach here if the scan is not parallel, or if we're serially
             * executing a scan that was planned to be parallel.
             */
            bioseq_scan_runtime& runtime = bioseq_runtime(bioseq_scan_state);
            const bool needs_deferred_hydration =
                (runtime.locator_passthrough || runtime.lazy_source_projection ||
                 runtime.buffer.mmsearch_active() || runtime.param_rebind.active) &&
                !runtime.buffer.per_row_self_alignment();

            /*
             * The batched mmsearch and locator-passthrough paths
             * consume locator stubs directly.  A lazy SliceProject source also stays
             * deferred, then promotes only its bound columns through the owning
             * provider immediately before projection.  Plain scans may be projected
             * by an upper Result node, so they use the ordinary table-AM scan and
             * return fully hydrated bioseq.seq varlenas. Same-column self-alignment
             * (`mmsearch(seq, seq)`) stays non-deferred: submit_scan can copy the
             * hydrated value once as both target and per-row query, avoiding the cold
             * post-load descriptor path while preserving deferred hydration for real
             * target/query scans.
             */
            if (needs_deferred_hydration) {
                scandesc = bioseq_beginscan_deferred_c(node->ss.ss_currentRelation,
                                                       bioseq_scan_state->snapshot,
                                                       0,
                                                       nullptr,
                                                       nullptr,
                                                       flags,
                                                       bioseq_scan_state->attributes_needed.get(),
                                                       bioseq_scan_state->qualifiers.get(),
                                                       bioseq_scan_state->parallel_scan,
                                                       is_vectorization_enabled);
            } else {
                scandesc = bioseq_beginscan_extended_c(node->ss.ss_currentRelation,
                                                       bioseq_scan_state->snapshot,
                                                       0,
                                                       nullptr,
                                                       nullptr,
                                                       flags,
                                                       bioseq_scan_state->attributes_needed.get(),
                                                       bioseq_scan_state->qualifiers.get(),
                                                       bioseq_scan_state->parallel_scan,
                                                       is_vectorization_enabled);
            }

            node->ss.ss_currentScanDesc = scandesc;
        }

        if (scandesc == nullptr || scandesc->rs_rd == nullptr) {
            ereport(ERROR,
                    (errmsg("bioseq: failed to initialize columnar scan descriptor "
                            "for relation %u",
                            node->ss.ss_currentRelation == nullptr
                                ? InvalidOid
                                : RelationGetRelid(node->ss.ss_currentRelation))));
        }

        if (table_scan_getnextslot(scandesc, direction, slot)) {
            /*
             * Setup custom scan state for reading tuples directly from stored
             * vector tuple table slot
             */
            if (is_vectorization_enabled) {
                bioseq_scan_state->vectorization.pending_row_number =
                    reinterpret_cast<vector_tuple_table_slot_storage*>(slot)->row_count;
                bioseq_scan_state->vectorization.row_index = 0;
            }
            return slot;
        }

        return nullptr;
    }

    /** @brief Accepts an EvalPlanQual tuple already rechecked by the table access method. */
    bool bioseq_column_scan_recheck(scan_state* node, TupleTableSlot* slot) {
        return true;
    }

    /** @brief Executes one base-scan step and translates C++ failures to PostgreSQL errors. */
    TupleTableSlot* bioseq_column_scan_exec_custom_scan(CustomScanState* node) {
        TupleTableSlot* result = nullptr;
        ErrorData* exact_error = nullptr;
        bool cpp_error = false;
        bool unknown_cpp_error = false;
        std::array<char, 512> detail{"std::exception without detail"};
        try {
            scan_state* state = scan_state::from_custom_scan_state(node);
            bioseq_scan_runtime& runtime = bioseq_runtime(state);
            if (runtime.mmsearch_null_input && !runtime.match_evaluated_submission) {
                /* A raw target with a NULL owned query is statement-empty.  An
                 * evaluated target must still run once for every residual-admitted row
                 * before strict NULL semantics discard the result, so only the raw
                 * owner may bypass the table scan here. */
                if (runtime.alignment_registration.is_registered()) {
                    bioseq::postgres::table::alignment_execution::mark_alignment_pass_fully_drained(
                        runtime.alignment_registration);
                }
                return ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
            }
            bioseq::postgres::table::alignment_execution::ensure_alignment_node_registered(
                runtime.alignment_registration);
            result =
                custom_exec_scan(state,
                                 reinterpret_cast<ExecScanAccessMtd>(bioseq_column_scan_next),
                                 reinterpret_cast<ExecScanRecheckMtd>(bioseq_column_scan_recheck));
            return result;
        } catch (bioseq::postgres::support::postgres_error& error) {
            exact_error = error.release_error_data();
        } catch (const std::exception& error) {
            strlcpy(detail.data(),
                    error.what() == nullptr ? "std::exception without detail" : error.what(),
                    detail.size());
            cpp_error = true;
        } catch (...) {
            unknown_cpp_error = true;
        }

        scan_state* state = scan_state::from_custom_scan_state(node);
        if (state != nullptr && state->runtime_state != nullptr) {
            auto* runtime = static_cast<bioseq_scan_runtime*>(state->runtime_state);
            runtime->buffer.abandon_join_runtime_after_pg_error_noexcept();
        }
        if (exact_error != nullptr) {
            ReThrowError(exact_error);
        }
        if (cpp_error) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqColumnScan Exec raised a C++ exception"),
                     errdetail_internal("%s", detail.data())));
        }
        if (unknown_cpp_error) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqColumnScan Exec raised an unknown C++ exception")));
        }
        return result;
    }

    /**
     * @brief Releases scan slots, providers, snapshots, and table-AM state in ownership order.
     *
     * The C++ runtime remains alive until its query-memory-context callback runs because the
     * callback record is embedded in that runtime.
     */
    void bioseq_column_scan_end_custom_scan(CustomScanState* node) {
        TableScanDesc scan_desc = node->ss.ss_currentScanDesc;
        scan_state* bioseq_scan_state = scan_state::from_custom_scan_state(node);

        /* Emit one profiler summary per scan, then reset process-local counters. */
        if (bioseq::aligner::settings::profiling::enabled) {
            bioseq::instrumentation::summary::write_summary_to_log();
            bioseq::instrumentation::summary::reset_metrics();
        }

        /*
         * Free buffered scan slots HERE, while the per-query ResourceOwner
         * that pinned their TupleDesc refs is still active. Deferring this
         * to the next query's init would leak TupleDesc refcounts onto a
         * dead ResourceOwner and produce
         *   "WARNING: resource was not closed: TupleDesc ..."
         * followed by
         *   "ERROR: tupdesc reference ... is not owned by resource owner Portal"
         * when the next query tries to release them.
         *
         * After clear_slots, free the bioseq_scan_runtime itself. Owning the
         * std::vector / std::map storage on the C++ heap (rather than in a
         * Postgres MemoryContext) means we have to reach the destructor
         * here — Postgres' MemoryContext teardown wouldn't run them.
         */
        if (bioseq_scan_state->runtime_state != nullptr) {
            bioseq_scan_runtime* runtime_ptr =
                static_cast<bioseq_scan_runtime*>(bioseq_scan_state->runtime_state);
            /*
             * Clear MinimalTuple slots while the per-query ResourceOwner
             * is still alive — Postgres tuple-desc refcounts are tracked
             * against it. The MemoryContext reset callback fires later
             * (during es_query_cxt teardown) and is forbidden from
             * touching slots, so we MUST do this here.
             *
             * We deliberately do NOT delete the runtime here. The
             * MemoryContextCallback struct that bioseq_runtime_cleanup_callback
             * uses lives inside the runtime; freeing the runtime now would
             * dangle the callback pointer in es_query_cxt's reset chain
             * and SIGSEGV on context teardown. The callback handles the
             * delete safely once the context begins to tear down.
             */
            /* Drop every tuple-visible reference before resetting the expression
             * contexts that own promoted EOHs. Result and scan slots may alias in a
             * projection-free shape, so clear the physical slot exactly once. */
            clear_recipe_scan_parent_tuple_references(node);

            /* Locator promotion uses the ordinary executor ExprContext. The private
             * runtime context can independently retain evaluated Params, so drain it
             * too while the exact source provider remains live. */
            ExprContext* expression_context = node->ss.ps.ps_ExprContext;
            if (bioseq_scan_state->runtime_expression_context != nullptr &&
                bioseq_scan_state->runtime_expression_context != expression_context) {
                ReScanExprContext(bioseq_scan_state->runtime_expression_context);
            }

            /* Tuple-visible EOH references are gone; the retained batch arena may
             * now release its wrappers and source-domain edges. */
            runtime_ptr->buffer.clear_slots();

            if (runtime_ptr->lazy_source_provider != nullptr) {
                bioseq::storage::close_pg_sequence_source_provider(
                    runtime_ptr->lazy_source_provider, /*is_commit=*/true);
                runtime_ptr->lazy_source_provider = nullptr;
            }
            runtime_ptr->buffer.clear_sequence_source_provider_binding();
            if (runtime_ptr->lazy_trace_attachment != nullptr) {
                release_slice_trace_attachment(runtime_ptr->lazy_trace_attachment);
                runtime_ptr->lazy_trace_attachment = nullptr;
            }
            runtime_ptr->cleanup_done = true;
        }

        /*
         * Cleanup BMS of selected scan attributes
         */
        bioseq_scan_state->attributes_needed.clear();

        /* A failed/partial Begin may not have allocated runtime state. */
        if (bioseq_scan_state->runtime_state == nullptr) {
            TupleTableSlot* result_slot = node->ss.ps.ps_ResultTupleSlot;
            TupleTableSlot* scan_slot = node->ss.ss_ScanTupleSlot;
            if (result_slot != nullptr)
                ExecClearTuple(result_slot);
            if (scan_slot != nullptr && scan_slot != result_slot) {
                ExecClearTuple(scan_slot);
            }
        }

        /*
         * close heap scan
         */
        if (scan_desc != nullptr) {
            table_endscan(scan_desc);
            node->ss.ss_currentScanDesc = nullptr;
        }

        /* Unregister snapshot */
        if (bioseq_scan_state->owns_snapshot_registration) {
            UnregisterSnapshot(bioseq_scan_state->snapshot);
        }
    }

    /** @brief Clears buffered output, folds current Params, and rescans the table access method. */
    void bioseq_column_scan_rescan_custom_scan(CustomScanState* node) {
        CustomScan* cscan = reinterpret_cast<CustomScan*>(node->ss.ps.plan);
        scan_state* bioseq_scan_state = scan_state::from_custom_scan_state(node);
        bioseq_scan_runtime& runtime = bioseq_runtime(bioseq_scan_state);
        bioseq::postgres::table::alignment_execution::record_alignment_rescan(
            runtime.alignment_registration);

        clear_recipe_scan_parent_tuple_references(node);
        if (bioseq_scan_state->runtime_expression_context != node->ss.ps.ps_ExprContext) {
            ResetExprContext(bioseq_scan_state->runtime_expression_context);
        }
        List* all_clauses = postgres_list<List>(cscan->custom_exprs).get_second();
        List* storage_clauses =
            storage_prunable_quals_without_mmsearch(all_clauses, get_cached_stub_oids());
        bioseq_scan_state->qualifiers = bioseq::postgres::support::postgres_list<Node>{
            reinterpret_cast<List*>(eval_params(reinterpret_cast<Node*>(storage_clauses),
                                                bioseq_scan_state->runtime_expression_context))};

        /*
         * Drop every buffered tuple, output cursor and match map left over
         * from the previous scan iteration. Without this, an inner-side
         * scan that drained its cursor on iteration N would re-enter the reading
         * stage immediately on iteration N+1 (because output_position stayed
         * pointing past the cursor end) and return EOF without ever pulling
         * a tuple from the access method. ReScan is the natural reset
         * point: BeginCustomScan only fires once per query, so we cannot
         * defer this to there.
         */
        runtime.buffer.clear_slots();
        runtime.source_exhausted = false;

        /*
         * Param-driven mmsearch rebind: when the planner rewrote the
         * second mmsearch argument as a Param (parameterized inner-side
         * scan in a Nested Loop), each rescan needs to install the current
         * Param value as the new query. The Param lives in the scan
         * node's PlanState ExprContext (filled by the NL parent before
         * triggering rescan), so we resolve it via EvalParamsMutator
         * against a copy of the original FuncExpr argument.
         *
         * mmsearch_parameter_rebind retains the exact plan FuncExpr because the copied
         * storage qual has already folded Params and planner order may put a Const
         * extra before the Param-driven primary.
         */
        if (runtime.param_rebind.active) {
            /* The stashed FuncExpr identifies the exact primary Param even when
             * planner qual ordering puts a Const extra first.  NULL clears only
             * the primary context; BMO rejects NULL Const extras during planning,
             * while registered non-NULL extras survive for a later non-NULL rescan. */
            (void)resolve_mmsearch_param_query(
                runtime, node->ss.ps.ps_ExprContext, "ReScanCustomScan");
        }

        TableScanDesc scan_desc = node->ss.ss_currentScanDesc;

        if (scan_desc != nullptr) {
            /* Pass the retained qualifier storage through the table AM ScanKey ABI. */
            ScanKey scan_keys = reinterpret_cast<ScanKey>(bioseq_scan_state->qualifiers.get());
            table_rescan(node->ss.ss_currentScanDesc, scan_keys);
        }
    }

} // export custom-scan partition declarations

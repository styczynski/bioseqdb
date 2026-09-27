/**
 * @file
 * @brief Owns base-scan execution, cleanup, and rescan callbacks.
 */
module;
#include <bioseq/pg_compat.h>
#include <new>

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

export import :custom_scan_scan_runtime_execution_callbacks_contracts;

import std;
import bioseq.common.logging;
import bioseq.instrumentation.summary_writer;
import bioseq.postgres.configuration.aligner;
import bioseq.postgres.support;
import bioseq.postgres.table.alignment_execution;
import bioseq.postgres.table.scan_state;
import bioseq.postgres.vectorization.types;

import :access_method_contract;
import :custom_scan_batch_join_execution_parent_tuple_contracts;
import :custom_scan_join_runtime_parameter_binding_contracts;
import :custom_scan_join_runtime_tuple_injection_contracts;
import :custom_scan_oid_expression_support_oid_catalog_core_contracts;
import :custom_scan_oid_expression_support_storage_pruning_contracts;
import :custom_scan_scan_runtime_source_lifecycle_contracts;
import :custom_scan_scan_runtime_state_contracts;

using namespace bioseq::postgres::support;

export {
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

            release_scan_runtime_lazy_resources(*runtime_ptr, /*is_commit=*/true);
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

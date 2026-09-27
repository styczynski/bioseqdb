/**
 * @file
 * @brief Fetches base-scan input and handles cleanup and rescans.
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

        /* Use the executor's current scan direction and slot format. */
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
            /* Bioseq's scan entry points do not use these table-AM flags. */
            uint32_t flags = 0;

            /* Open the descriptor on first use, with any shared parallel-scan state. */
            bioseq_scan_runtime& runtime = bioseq_runtime(bioseq_scan_state);
            const bool needs_deferred_hydration =
                (runtime.locator_passthrough || runtime.lazy_source_projection ||
                 runtime.buffer.mmsearch_active() || runtime.param_rebind.active) &&
                !runtime.buffer.per_row_self_alignment();

            /* Alignment batches and lazy source scans keep sequence locators until
             * their data is needed. Ordinary scans return loaded sequences for parent
             * expressions. Same-column self-alignment also uses loaded data, allowing
             * the buffer to copy one value for both target and query. */
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
            /* Start consuming rows from the newly fetched vector. */
            if (is_vectorization_enabled) {
                bioseq_scan_state->vectorization.pending_row_number =
                    reinterpret_cast<vector_tuple_table_slot_storage*>(slot)->row_count;
                bioseq_scan_state->vectorization.row_index = 0;
            }
            return slot;
        }

        return nullptr;
    }

    /** @brief Leaves EvalPlanQual replacement rows to the scan's normal filter evaluation. */
    bool bioseq_column_scan_recheck(scan_state* node, TupleTableSlot* slot) {
        return true;
    }

    /**
     * @brief Releases scan resources while PostgreSQL still owns their tuple references.
     *
     * The C++ runtime survives until its query-memory callback runs. That callback
     * is stored inside the runtime, so deleting it here would leave a dangling
     * callback in the query context.
     */
    void bioseq_column_scan_end_custom_scan(CustomScanState* node) {
        TableScanDesc scan_desc = node->ss.ss_currentScanDesc;
        scan_state* bioseq_scan_state = scan_state::from_custom_scan_state(node);

        /* Emit one profiler summary per scan, then reset process-local counters. */
        if (bioseq::aligner::settings::profiling::enabled) {
            bioseq::instrumentation::summary::write_summary_to_log();
            bioseq::instrumentation::summary::reset_metrics();
        }

        /* Release buffered slots while the query's ResourceOwner still tracks
         * their tuple descriptors. Releasing them after that owner is gone can
         * leak references or report an ownership error. */
        if (bioseq_scan_state->runtime_state != nullptr) {
            bioseq_scan_runtime* runtime_ptr =
                static_cast<bioseq_scan_runtime*>(bioseq_scan_state->runtime_state);
            /* The later query-context callback destroys the C++ runtime. Clear
             * PostgreSQL slots here, while their ResourceOwner is still active. */
            /* Clear slot references before resetting memory that holds expanded
             * sequences. The result and scan slot can be the same object. */
            clear_recipe_scan_parent_tuple_references(node);

            /* Reset the private context too, while the source provider still lives.
             * Evaluated parameters may hold sequence values there. */
            ExprContext* expression_context = node->ss.ps.ps_ExprContext;
            if (bioseq_scan_state->runtime_expression_context != nullptr &&
                bioseq_scan_state->runtime_expression_context != expression_context) {
                ReScanExprContext(bioseq_scan_state->runtime_expression_context);
            }

            /* Slots no longer refer to the batch's sequence wrappers, so release them. */
            runtime_ptr->buffer.clear_slots();

            release_scan_runtime_lazy_resources(*runtime_ptr, /*is_commit=*/true);
            runtime_ptr->cleanup_done = true;
        }

        /* Release the bitmap of columns needed by the scan. */
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

        /* Close the table-AM scan. */
        if (scan_desc != nullptr) {
            table_endscan(scan_desc);
            node->ss.ss_currentScanDesc = nullptr;
        }

        /* Unregister snapshot */
        if (bioseq_scan_state->owns_snapshot_registration) {
            UnregisterSnapshot(bioseq_scan_state->snapshot);
        }
    }

    /** @brief Resets buffered results and binds current parameters before restarting the table scan. */
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

        /* A nested-loop rescan starts with an empty buffer and output cursor.
         * BeginCustomScan runs only once, so reset the previous iteration here. */
        runtime.buffer.clear_slots();
        runtime.source_exhausted = false;

        /* The nested-loop parent has installed this row's parameters before
         * requesting a rescan. Rebind mmsearch from its saved plan expression,
         * because the storage filters already contain evaluated parameter values. */
        if (runtime.param_rebind.active) {
            /* The saved call identifies the primary query regardless of filter order.
             * A NULL primary disables it for this rescan. Constant extra queries remain
             * registered for later rows with a non-NULL primary. */
            (void)resolve_mmsearch_param_query(
                runtime, node->ss.ps.ps_ExprContext, "ReScanCustomScan");
        }

        TableScanDesc scan_desc = node->ss.ss_currentScanDesc;

        if (scan_desc != nullptr) {
            /* Pass the saved qualifier storage through the table AM ScanKey ABI. */
            ScanKey scan_keys = reinterpret_cast<ScanKey>(bioseq_scan_state->qualifiers.get());
            table_rescan(node->ss.ss_currentScanDesc, scan_keys);
        }
    }

} // export custom-scan partition declarations

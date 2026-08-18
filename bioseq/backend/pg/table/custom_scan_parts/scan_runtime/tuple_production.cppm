/**
 * @file
 * @brief Owns residual-row evaluation and scalar or vector tuple production.
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

export module bioseq.postgres.table:custom_scan_scan_runtime_tuple_production;

import std;
import bioseq.common.logging;
import bioseq.postgres.storage.scanner;
import bioseq.postgres.support;
import bioseq.postgres.table.alignment_execution;
import bioseq.postgres.table.scan_state;
import bioseq.postgres.vectorization.execution;
import bioseq.postgres.vectorization.types;

import :access_common;
import :access_method;
import :custom_scan_join_runtime_tuple_production;
import :custom_scan_runtime_support;
import :custom_scan_scan_runtime_source_lifecycle;
import :custom_scan_scan_runtime_symbol_count_materialization;

using namespace bioseq::postgres::support;

export {
    /**
     * @brief Evaluates one row's residual qual and any expression-backed mmsearch operands.
     *
     * Evaluate the residual frontier and, when configured, both mmsearch operand
     * ExprStates while only their physical sources are promoted.  The scan slot is
     * restored on normal completion, C++ unwind, and PostgreSQL longjmp before a
     * caller can buffer it.
     * @param runtime Scan runtime with mmsearch operand ExprStates and pre-eval source flags.
     * @param slot Input tuple whose locator-backed sources may be promoted for evaluation.
     * @param qual Residual qual ExprState, or nullptr when no qual is installed.
     * @param expression_context Executor context supplying per-tuple memory and evaluation state.
     * @return Qual pass flag plus evaluated target and optional query datums for buffered submission.
     */
    [[nodiscard]] mmsearch_row_evaluation
    evaluate_mmsearch_input_row(bioseq_scan_runtime& runtime,
                                TupleTableSlot* slot,
                                ExprState* qual,
                                ExprContext* expression_context) {
        mmsearch_row_evaluation result{};
        if (!runtime.match_evaluated_submission && !runtime.mmsearch_pre_eval_sources_present) {
            result.qual_passed = qual == nullptr || ExecQual(qual, expression_context);
            return result;
        }
        const mmsearch_source_restore source_restore =
            capture_mmsearch_sources(runtime,
                                     slot,
                                     expression_context->ecxt_per_tuple_memory,
                                     mmsearch_source_capture_role::evaluated_operand);
        if (source_restore.count == 0u && !runtime.match_evaluated_submission) {
            result.qual_passed = qual == nullptr || ExecQual(qual, expression_context);
            return result;
        }
        MemoryContext saved_context = CurrentMemoryContext;
        ErrorData* exact_pg_error = nullptr;
        {
            std::exception_ptr cpp_error;
            ErrorData* volatile pg_error = nullptr;
            PG_TRY();
            {
                try {
                    (void)promote_mmsearch_sources(
                        runtime, slot, expression_context->ecxt_per_tuple_memory, source_restore);
                    result.qual_passed = qual == nullptr || ExecQual(qual, expression_context);
                    if (result.qual_passed && runtime.match_evaluated_submission) {
                        if (runtime.match_target_expr_state == nullptr) {
                            ereport(ERROR,
                                    (errcode(ERRCODE_DATA_CORRUPTED),
                                     errmsg("BioseqScan evaluated target ExprState is missing")));
                        }
                        result.target = ExecEvalExprSwitchContext(runtime.match_target_expr_state,
                                                                  expression_context,
                                                                  &result.target_is_null);
                        if (runtime.match_query_evaluated) {
                            if (runtime.match_query_expr_state == nullptr) {
                                ereport(
                                    ERROR,
                                    (errcode(ERRCODE_DATA_CORRUPTED),
                                     errmsg("BioseqScan evaluated query ExprState is missing")));
                            }
                            /* Preserve SQL argument order even when the target is NULL. */
                            result.query = ExecEvalExprSwitchContext(runtime.match_query_expr_state,
                                                                     expression_context,
                                                                     &result.query_is_null);
                        }
                    }
                    restore_mmsearch_sources(slot, source_restore);
                } catch (...) {
                    MemoryContextSwitchTo(saved_context);
                    restore_mmsearch_sources(slot, source_restore);
                    cpp_error = std::current_exception();
                }
            }
            PG_CATCH();
            {
                MemoryContextSwitchTo(saved_context);
                restore_mmsearch_sources(slot, source_restore);
                pg_error = CopyErrorData();
                FlushErrorState();
            }
            PG_END_TRY();
            if (pg_error != nullptr)
                exact_pg_error = pg_error;
            if (cpp_error != nullptr)
                std::rethrow_exception(cpp_error);
        }
        /* Re-enter PostgreSQL only after std::exception_ptr has been destroyed. */
        if (exact_pg_error != nullptr)
            ReThrowError(exact_pg_error);
        return result;
    }

    /**
     * @brief Builds the next projected vector batch for a VectorAgg parent.
     *
     * VectorAgg consumes one vector_column_storage pointer per projected attribute.
     * The ordinary scan path extracts scalar rows, so build a projected
     * vector batch explicitly when the planner attached VectorAgg.
     * @param bioseq_scan_state Custom scan state with vector slots and compiled vector qualifiers.
     * @param access_mtd Child access-method fetch callback.
     * @param recheck_mtd Recheck callback for lossy index predicates.
     * @return Virtual vector result slot holding one projected batch, or an empty slot at EOF.
     */
    static TupleTableSlot* next_vectorized_aggregate_batch(scan_state* bioseq_scan_state,
                                                           ExecScanAccessMtd access_mtd,
                                                           ExecScanRecheckMtd recheck_mtd) {
        ScanState* node = reinterpret_cast<ScanState*>(&bioseq_scan_state->custom_scan_state.ss);
        bioseq_scan_runtime& runtime = bioseq_runtime(bioseq_scan_state);
        ExprContext* econtext = node->ps.ps_ExprContext;
        ExprState* qual = node->ps.qual;
        TupleTableSlot* result_slot = bioseq_scan_state->vectorization.result_vector_slot;
        auto* result_vector = reinterpret_cast<vector_tuple_table_slot_storage*>(result_slot);
        TupleTableSlot* scalar_scan_slot = node->ss_ScanTupleSlot;
        const bool projects_attributes = result_slot->tts_tupleDescriptor->natts != 0;
        ProjectionInfo* projection =
            projects_attributes ? ExecBuildProjectionInfo(runtime.newtargetlist,
                                                          econtext,
                                                          node->ps.ps_ResultTupleSlot,
                                                          &node->ps,
                                                          scalar_scan_slot->tts_tupleDescriptor)
                                : nullptr;

        for (;;) {
            TupleTableSlot* slot = exec_scan_fetch(node, access_mtd, recheck_mtd);
            if (TupIsNull(slot)) {
                runtime.source_exhausted = true;
                return ExecClearTuple(result_slot);
            }
            if (!is_vector_tuple_table_slot(slot)) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("VectorAgg input is not a vector tuple slot")));
            }

            auto* scan_vector = reinterpret_cast<vector_tuple_table_slot_storage*>(slot);
            if (!bioseq_scan_state->vectorization.qualifiers.is_empty()) {
                if (bioseq_scan_state->vectorization.constructed_qualifiers.is_empty()) {
                    bioseq_scan_state->vectorization.constructed_qualifiers =
                        bioseq::postgres::support::postgres_list<Node>{
                            compile_vector_qualifier_list(
                                slot, bioseq_scan_state->vectorization.qualifiers.get())};
                }
                bool* result_qual = evaluate_vectorized_qualifiers(
                    slot,
                    bioseq_scan_state->vectorization.constructed_qualifiers.get(),
                    AND_EXPR,
                    econtext);
                memcpy(scan_vector->row_is_active.data(),
                       result_qual,
                       scan_vector->row_count * sizeof(bool));
            }

            std::uint32_t result_dimension = 0;
            for (std::uint32_t row_index = 0; row_index < scan_vector->row_count; ++row_index) {
                if (!scan_vector->row_is_active[row_index])
                    continue;

                ResetExprContext(econtext);
                ExecClearTuple(scalar_scan_slot);
                extract_tuple_from_vector_slot(
                    scalar_scan_slot,
                    scan_vector,
                    static_cast<std::int32_t>(row_index),
                    bioseq_scan_state->vectorization.attributes_needed.get());
                scalar_scan_slot->tts_tid =
                    row_number_to_tid(scan_vector->source_row_numbers[row_index]);
                econtext->ecxt_scantuple = scalar_scan_slot;

                if (qual != nullptr && !ExecQual(qual, econtext)) {
                    InstrCountFiltered1(node, 1);
                    continue;
                }

                if (projects_attributes) {
                    TupleTableSlot* projected = ExecProject(projection);
                    write_tuple_to_vector_slot(
                        projected, result_vector, static_cast<std::int32_t>(result_dimension));
                }
                ++result_dimension;
            }

            bioseq_scan_state->vectorization.pending_row_number = 0;
            bioseq_scan_state->vectorization.row_index = scan_vector->row_count;
            if (result_dimension == 0)
                continue;

            result_vector->row_count = result_dimension;
            memset(result_vector->row_is_active.data(), true, result_dimension);
            ExecStoreVirtualTuple(result_slot);
            return result_slot;
        }
    }

    /**
     * @brief Runs the base scan's scalar or vectorized write/drain state machine.
     * Routes VectorAgg parents through vector batch projection. Otherwise runs the scalar or
     * vectorized fetch path, buffers mmsearch-qualified rows, and drains projected match output.
     * @param bioseq_scan_state Writable scan, buffer, and vectorization state for the node.
     * @param access_mtd Child access-method fetch callback.
     * @param recheck_mtd Recheck callback for lossy index predicates.
     * @return The next projected row or vector batch, or an empty slot at end of scan.
     */
    static TupleTableSlot* inner_custom_exec_scan(scan_state* bioseq_scan_state,
                                                  ExecScanAccessMtd access_mtd,
                                                  ExecScanRecheckMtd recheck_mtd) {
        bioseq_log<logging_level::debug>("[bioseq] pg: Inner_CustomExecScan() entered");
        ExprContext* econtext;
        ExprState* qual;

        ScanState* node = reinterpret_cast<ScanState*>(&bioseq_scan_state->custom_scan_state.ss);
        bioseq_scan_runtime& runtime = bioseq_runtime(bioseq_scan_state);

        qual = node->ps.qual;
        econtext = node->ps.ps_ExprContext;

        if (bioseq_scan_state->vectorization.is_aggregate) {
            ExecClearTuple(bioseq_scan_state->vectorization.result_vector_slot);
            return next_vectorized_aggregate_batch(bioseq_scan_state, access_mtd, recheck_mtd);
        }

        /*
         * Two-stage state machine:
         *
         *   Writing stage: pull tuples from the access method, run qual,
         *                      buffer passing tuples via buffer.submit_scan.
         *                      When source is exhausted, run batch processor
         *                      and transition.
         *   Reading stage: drain buffered output entries, setting
         *                      placeholder const nodes from match_results and
         *                      projecting via ExecProject.
         *
         * buffer.has_finished_writing() flips from false → true at the
         * transition.
         */
        for (;;) {
            /* ---- Reading stage: drain buffered output ---- */
            if (runtime.buffer.has_finished_writing()) {
                bioseq_log<logging_level::debug>(
                    "[bioseq] pg: Inner_CustomExecScan() draining output cursor");
                /* The parent has consumed the previously returned result before it
                 * invokes this scan again.  Drop that tuple-visible EOH first, then
                 * release every detoasted locator and source-backed output allocated
                 * in the ordinary executor per-tuple context.  Buffered MinimalTuple
                 * slots and match_results have independent lifetimes. */
                ExecClearTuple(node->ps.ps_ResultTupleSlot);
                ResetExprContext(econtext);
                econtext->ecxt_scantuple = nullptr;
                econtext->ecxt_outertuple = nullptr;
                econtext->ecxt_innertuple = nullptr;
                auto next = runtime.buffer.read_next_output();
                if (!next) {
                    if (runtime.source_exhausted) {
                        bioseq::postgres::table::alignment_execution::
                            mark_alignment_pass_fully_drained(runtime.alignment_registration);
                        bioseq_log<logging_level::debug>(
                            "[bioseq] pg: Inner_CustomExecScan() final output cursor drained; "
                            "returning empty slot (EOF)");
                        return ExecClearTuple(node->ps.ps_ResultTupleSlot);
                    }
                    /* A capacity rollover completed. Drop only that physical
                     * batch and resume the still-live access-method scan. */
                    runtime.buffer.clear_slots();
                    continue;
                }
                bioseq::storage::note_mmsearch_output_context_reset_for_test();

                if (runtime.edit_predicate_active &&
                    (runtime.edit_exclusive_upper_bound <= 0 ||
                     next->match->edit_distance >=
                         static_cast<std::uint64_t>(runtime.edit_exclusive_upper_bound))) {
                    /* SneakySnake is only a lower-bound gate.  Every survivor is
                     * aligned, and this exact comparison is the authoritative SQL
                     * predicate (strict `<`, including the boundary case). */
                    InstrCountFiltered1(node, 1);
                    continue;
                }

                /*
                 * Update every placeholder Const from the match_result. The
                 * subsequent ExecProject reads the current constvalue of
                 * every Const node referenced in newtargetlist.
                 */
                populate_placeholder_consts(runtime.placeholder_consts, *next->match);

                /*
                 * symbol_count placeholders are independent of match_result;
                 * they are resolved from meta_sidecar by (relid, rowno) so
                 * we populate them alongside the per-match fields.
                 */
                populate_symbol_count_placeholders(runtime.placeholder_consts,
                                                   node->ss_currentRelation,
                                                   next->sequence_rowno,
                                                   bioseq_scan_state->snapshot,
                                                   runtime.aligner_state.read_sequence_index);

                /* The alignment path retains locator stubs in its buffered row. Promote
                 * only for this output projection, then restore the stable locator so a
                 * second match from the same row never points into a reset per-tuple
                 * context. */
                const mmsearch_source_restore source_restore =
                    capture_mmsearch_sources(runtime,
                                             next->scan_slot_copy,
                                             econtext->ecxt_per_tuple_memory,
                                             mmsearch_source_capture_role::output_projection);
                TupleTableSlot* projected = nullptr;
                std::exception_ptr projection_error;
                MemoryContext saved_context = CurrentMemoryContext;
                ErrorData* volatile projection_pg_error = nullptr;

                /* PostgreSQL ERROR skips C++ destructors. Keep the immutable restore
                 * snapshot outside the setjmp region and restore the locator slot on
                 * normal return, C++ unwind, and PG longjmp alike. */
                PG_TRY();
                {
                    try {
                        const std::size_t promoted_source_count =
                            promote_mmsearch_sources(runtime,
                                                     next->scan_slot_copy,
                                                     econtext->ecxt_per_tuple_memory,
                                                     source_restore);
                        bioseq::storage::note_mmsearch_output_locator_promotion_for_test(
                            promoted_source_count != 0u);
                        bioseq::storage::inject_mmsearch_output_after_promotion_failure_for_test();

                        /* Re-install the buffered scan slot as the current scan
                         * tuple so Var references resolve off the right row. */
                        econtext->ecxt_scantuple = next->scan_slot_copy;

                        ProjectionInfo* proj =
                            ExecBuildProjectionInfo(runtime.newtargetlist,
                                                    econtext,
                                                    node->ps.ps_ResultTupleSlot,
                                                    &(node->ps),
                                                    next->scan_slot_copy->tts_tupleDescriptor);
                        projected = ExecProject(proj);
                        restore_mmsearch_sources(next->scan_slot_copy, source_restore);
                    } catch (...) {
                        MemoryContextSwitchTo(saved_context);
                        restore_mmsearch_sources(next->scan_slot_copy, source_restore);
                        projection_error = std::current_exception();
                    }
                }
                PG_CATCH();
                {
                    MemoryContextSwitchTo(saved_context);
                    restore_mmsearch_sources(next->scan_slot_copy, source_restore);
                    bioseq::storage::note_mmsearch_output_pg_error_restore_for_test(
                        mmsearch_sources_restored_exactly(next->scan_slot_copy, source_restore));
                    projection_pg_error = CopyErrorData();
                    FlushErrorState();
                }
                PG_END_TRY();
                if (projection_pg_error != nullptr) {
                    ErrorData* exact_error = projection_pg_error;
                    throw bioseq::postgres::support::postgres_error{exact_error};
                }
                if (projection_error != nullptr) {
                    std::rethrow_exception(projection_error);
                }
                return projected;
            }

            /* ---- Writing stage ---- */
            TupleTableSlot* slot = nullptr;

            /*
             * Scalar fetch path: pull one scan tuple and run qual on it.
             */
            if (!bioseq_scan_state->vectorization.is_enabled) {
                bioseq_log<logging_level::debug>(
                    "[bioseq] pg: Inner_CustomExecScan() scalar fetch via ExecScanFetch");
                slot = exec_scan_fetch(node, access_mtd, recheck_mtd);

                if (TupIsNull(slot)) {
                    runtime.source_exhausted = true;
                    bioseq_log<logging_level::debug>("[bioseq] pg: Inner_CustomExecScan() source "
                                                     "exhausted; running batch processor");
                    if (runtime.buffer.run_batch_and_build_cursors()) {
                        bioseq::postgres::table::alignment_execution::merge_alignment_batch(
                            runtime.alignment_registration, runtime.buffer.execution_stats());
                    }
                    continue; /* Next iteration enters the reading stage. */
                }
            }
            /*
             * Vectorized fetch path: pull a vector slot, apply vectorized
             * qual mask, then extract a single scalar tuple per iteration
             * until the vector is exhausted.
             */
            else {
                /*
                 * First vector pull or after previous vector drained.
                 */
                if (bioseq_scan_state->vectorization.pending_row_number == 0) {
                    bioseq_log<logging_level::debug>("[bioseq] pg: Inner_CustomExecScan() "
                                                     "vectorized fetch (initial vector pull)");
                    slot = exec_scan_fetch(node, access_mtd, recheck_mtd);

                    if (TupIsNull(slot)) {
                        runtime.source_exhausted = true;
                        bioseq_log<logging_level::debug>(
                            "[bioseq] pg: Inner_CustomExecScan() vectorized source exhausted; "
                            "running batch processor");
                        if (runtime.buffer.run_batch_and_build_cursors()) {
                            bioseq::postgres::table::alignment_execution::merge_alignment_batch(
                                runtime.alignment_registration, runtime.buffer.execution_stats());
                        }
                        continue;
                    }

                    if (!bioseq_scan_state->vectorization.qualifiers.is_empty()) {
                        if (bioseq_scan_state->vectorization.constructed_qualifiers.is_empty()) {
                            bioseq_scan_state->vectorization.constructed_qualifiers =
                                bioseq::postgres::support::postgres_list<Node>{
                                    compile_vector_qualifier_list(
                                        slot, bioseq_scan_state->vectorization.qualifiers.get())};
                        }

                        vector_tuple_table_slot_storage* vector_slot =
                            reinterpret_cast<vector_tuple_table_slot_storage*>(slot);
                        bool* result_qual = evaluate_vectorized_qualifiers(
                            slot,
                            bioseq_scan_state->vectorization.constructed_qualifiers.get(),
                            AND_EXPR,
                            econtext);
                        memcpy(vector_slot->row_is_active.data(),
                               result_qual,
                               vector_column_capacity * sizeof(bool));
                    }
                }

                /*
                 * Extract one kept row from the vector slot into the scalar
                 * scan slot. Skip inactive rows.
                 */
                bioseq_log<logging_level::debug>(
                    "[bioseq] pg: Inner_CustomExecScan() vectorized extraction");
                vector_tuple_table_slot_storage* vector_slot =
                    reinterpret_cast<vector_tuple_table_slot_storage*>(
                        bioseq_scan_state->vectorization.scan_vector_slot);

                bool row_found = false;
                while (bioseq_scan_state->vectorization.pending_row_number != 0 && !row_found) {
                    if (vector_slot->row_is_active[bioseq_scan_state->vectorization.row_index]) {
                        slot = bioseq_scan_state->custom_scan_state.ss.ss_ScanTupleSlot;
                        ExecClearTuple(slot);
                        extract_tuple_from_vector_slot(
                            slot,
                            vector_slot,
                            bioseq_scan_state->vectorization.row_index,
                            bioseq_scan_state->vectorization.attributes_needed.get());

                        uint64_t row_number =
                            vector_slot
                                ->source_row_numbers[bioseq_scan_state->vectorization.row_index];
                        slot->tts_tid = row_number_to_tid(row_number);
                        row_found = true;
                    }
                    bioseq_scan_state->vectorization.pending_row_number--;
                    bioseq_scan_state->vectorization.row_index++;
                }

                /*
                 * Vector exhausted without finding a kept row: go back to
                 * pull another vector or finalize.
                 */
                if (!row_found)
                    continue;
            }

            /* Every fetched input row owns a fresh expression/root arena.  BSS3
             * promotion and the authoritative operand ExprStates retain only until
             * submit_scan_evaluated copies their small EOH wrappers into the batch
             * arena. */
            ResetExprContext(econtext);
            econtext->ecxt_scantuple = slot;

            /*
             * The scan runs with defer_seq_hydration=true, so `slot`
             * carries 40-byte locator stubs. When there is an mmsearch qual the
             * row is buffered (submit_scan captures the locator; the batch path
             * bulk-hydrates per bucket), so we leave the stub in place. Without an
             * mmsearch qual the row is projected immediately below, so hydrate the
             * SEQ columns now (force=true overrides the defer flag) — this covers
             * both the scalar and vectorized convergence and is the analogue of
             * the inline hydration bioseq_getnextslot performs on non-deferred
             * scan paths.
             */
            if (!runtime.buffer.mmsearch_active() && !runtime.locator_passthrough &&
                !runtime.lazy_source_projection) {
                hydrate_scan_slot(node->ss_currentScanDesc, slot, /*force=*/true);
            }
            /* A non-mmsearch scan projects immediately and may promote every BSS3
             * source.  Alignment scans instead retain stable locators in their
             * buffered row.  Evaluated operand leaves are promoted temporarily at
             * their ExprState boundary below; output-only sources are promoted once
             * per match_result under the PG_TRY restoration boundary above. */
            if (runtime.lazy_source_projection && !runtime.locator_passthrough &&
                !runtime.buffer.mmsearch_active()) {
                promote_slice_project_sources(bioseq_scan_state,
                                              slot,
                                              econtext->ecxt_per_tuple_memory,
                                              /*allow_locator_passthrough=*/false,
                                              slice_project_source_slot_layout::physical_relation,
                                              slot->tts_tupleDescriptor);
            }

            /*
             * Place the current tuple into the expr context and run the
             * non-vectorized qual (e.g. mmsearch()). Both scalar and
             * vectorized paths converge here.
             */
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: Inner_CustomExecScan() applying qual filter");
            mmsearch_row_evaluation mmsearch_evaluation{};
            bool qual_passed = false;
            if (runtime.buffer.mmsearch_active()) {
                mmsearch_evaluation =
                    evaluate_mmsearch_input_row(runtime, econtext->ecxt_scantuple, qual, econtext);
                qual_passed = mmsearch_evaluation.qual_passed;
            } else {
                qual_passed = qual == nullptr || ExecQual(qual, econtext);
            }
            if (qual_passed) {
                if (runtime.buffer.mmsearch_active()) {
                    bioseq_log<logging_level::debug>("[bioseq] pg: Inner_CustomExecScan() qual "
                                                     "passed; buffering scan tuple");
                    if (runtime.match_evaluated_submission) {
                        if (runtime.mmsearch_null_input)
                            continue;
                        runtime.buffer.submit_scan_evaluated(econtext->ecxt_scantuple,
                                                             mmsearch_evaluation.target,
                                                             mmsearch_evaluation.target_is_null,
                                                             runtime.match_query_evaluated,
                                                             mmsearch_evaluation.query,
                                                             mmsearch_evaluation.query_is_null);
                    } else {
                        if (runtime.mmsearch_null_input)
                            continue;
                        bool target_is_null = false;
                        (void)slot_getattr(econtext->ecxt_scantuple,
                                           runtime.match_sequence_attno,
                                           &target_is_null);
                        if (target_is_null)
                            continue;
                        runtime.buffer.submit_scan(econtext->ecxt_scantuple);
                    }
                    if (runtime.buffer.ready_to_dispatch()) {
                        if (runtime.buffer.run_batch_and_build_cursors()) {
                            bioseq::postgres::table::alignment_execution::merge_alignment_batch(
                                runtime.alignment_registration, runtime.buffer.execution_stats());
                        }
                    }
                } else {
                    if (runtime.mmsearch_null_input) {
                        /* A NULL global Const/Param leaves the buffer inactive.  An
                         * evaluated target still has SQL effects and must run once. */
                        if (runtime.match_evaluated_submission &&
                            runtime.match_target_expr_state != nullptr) {
                            bool ignored_null = false;
                            (void)ExecEvalExprSwitchContext(
                                runtime.match_target_expr_state, econtext, &ignored_null);
                        }
                        continue;
                    }
                    /*
                     * No WHERE mmsearch(...) qual installed -> there is nothing
                     * to batch. Project the scan tuple to the CustomScan target
                     * list so the output slot layout matches what an outer Result
                     * node expects. PlanCustomPath filters this target list to
                     * plain Vars plus bioseq-owned placeholders; ordinary
                     * expressions such as seq::text and bioseq.len(seq) remain in
                     * the outer Result and evaluate against this projected tuple.
                     */
                    bioseq_log<logging_level::debug>(
                        "[bioseq] pg: Inner_CustomExecScan() qual passed; no mmsearch qual");

                    /*
                     * symbol_count fast-path requires the row number; the
                     * access method set it on slot->tts_tid above. Convert
                     * once per projected row rather than stashing a
                     * (relid, rowno) in shared state.
                     */
                    {
                        int64_t row_number = -1;
                        if (ItemPointerIsValid(&slot->tts_tid)) {
                            row_number = static_cast<int64_t>(tid_to_row_number(slot->tts_tid));
                        }
                        populate_symbol_count_placeholders(
                            runtime.placeholder_consts,
                            node->ss_currentRelation,
                            row_number,
                            bioseq_scan_state->snapshot,
                            runtime.aligner_state.read_sequence_index);
                    }

                    ProjectionInfo* proj = ExecBuildProjectionInfo(runtime.newtargetlist,
                                                                   econtext,
                                                                   node->ps.ps_ResultTupleSlot,
                                                                   &(node->ps),
                                                                   slot->tts_tupleDescriptor);
                    TupleTableSlot* projected = ExecProject(proj);
                    if (runtime.locator_passthrough) {
                        if (!ItemPointerIsValid(&slot->tts_tid) ||
                            runtime.locator_rowno_resno <= 0 ||
                            runtime.locator_rowno_resno > projected->tts_tupleDescriptor->natts) {
                            ereport(ERROR,
                                    (errcode(ERRCODE_INTERNAL_ERROR),
                                     errmsg("Bioseq locator passthrough row-number projection is "
                                            "invalid")));
                        }
                        Form_pg_attribute rowno_attribute = TupleDescAttr(
                            projected->tts_tupleDescriptor, runtime.locator_rowno_resno - 1);
                        if (rowno_attribute->atttypid != INT8OID) {
                            ereport(ERROR,
                                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                                     errmsg("Bioseq locator passthrough row-number column is not "
                                            "bigint")));
                        }
                        slot_getallattrs(projected);
                        projected->tts_values[runtime.locator_rowno_resno - 1] = Int64GetDatum(
                            static_cast<std::int64_t>(tid_to_row_number(slot->tts_tid)));
                        projected->tts_isnull[runtime.locator_rowno_resno - 1] = false;
                    }
                    return projected;
                }
            } else {
                InstrCountFiltered1(node, 1);
            }
        }
    }

} // export custom-scan partition declarations

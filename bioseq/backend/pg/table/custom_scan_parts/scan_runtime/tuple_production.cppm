/**
 * @file
 * @brief Filters input rows and produces scalar, ventor or alignment results.
 */
module;
#include <bioseq/pg_compat.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <new>

// Include libc++ allocation declarations before header-based module dependencies.
// Clang 22 can otherwise crash in std::__libcpp_allocate when generating logging code

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

module bioseq.postgres.table;

import :custom_scan_scan_runtime_tuple_production_contracts;
import :custom_scan_scan_runtime_execution_callbacks_contracts;

import bioseq.common.logging;
import bioseq.postgres.column.table_access;
import bioseq.postgres.storage.scanner.source_test_contract;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.postgres.types.compact_sequence;
import bioseq.sequences.cigar_alphabet;
import bioseq.sequences.compact_sequence_typmod;
import bioseq.postgres.table.alignment_execution;
import bioseq.postgres.table.scan_state;
import bioseq.postgres.vectorization.execution;
import bioseq.postgres.vectorization.types;

import :access_common;
import :custom_scan_join_runtime_tuple_injection_contracts;
import :custom_scan_join_runtime_scan_fetch_contracts;
import :custom_scan_oid_expression_support_oid_catalog_core_contracts;
import :custom_scan_scan_runtime_state_contracts;
import :custom_scan_scan_runtime_source_lifecycle_contracts;
import :custom_scan_scan_runtime_symbol_count_materialization_contracts;

using namespace bioseq::postgres::support;

namespace {

    /**
     * @brief Builds a projected vector batch for a VectorAgg parent.
     *
     * Applies vector filters, then checks remaining scalar filters and projects each
     * surviving row  into the result vector. Skips batches with no survivors.
     * @return A vector result slot, or an empty slot when the input ends.
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

} // namespace

static TupleTableSlot* inner_custom_exec_scan(scan_state* bioseq_scan_state,
                                               ExecScanAccessMtd access_mtd,
                                               ExecScanRecheckMtd recheck_mtd);

/** @brief Prepares result placeholders on first use and produces the next scan result. */
TupleTableSlot* custom_exec_scan(scan_state* bioseq_scan_state,
                                 ExecScanAccessMtd access_mtd,
                                 ExecScanRecheckMtd recheck_mtd) {

    // Trace base-scan execution.
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

        /* Scores and MAPQ use PostgreSQL int4 values stored directly in Datum. */
        const auto int_placeholder = []() {
            return makeConst(INT4OID,
                             -1,
                             InvalidOid,
                             4,
                             static_cast<Datum>(static_cast<int32_t>(0)),
                             /*constisnull=*/true,
                             /*constbyval=*/true);
        };
        /* Sequence positions, lengths and edit distance use bigint so values
         * above INT32_MAX keep their full range. */
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
        /* CIGAR seq: byref varlena with full/default CIGAR metadata. */
        const auto cigar_placeholder = []() {
            const compact_sequence_stdc_type_params params{
                .alphabet_code = pgseq_cigar_alphabet_code,
                .storage_unit_byte_width = sizeof(bioseq::cigar_run),
                .encoding = bioseq::sequence_encoding::cigar_rle,
                .space = bioseq::cigar_space::compatibility,
            };
            return makeConst(get_cached_stub_oids().bioseq_seq_type_oid,
                             params.encode(),
                             InvalidOid,
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
        runtime.placeholder_consts.cigar_node = cigar_placeholder();

        /* Clear buffered rows while keeping the mmsearch configuration
         * prepared by BeginCustomScan. */
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
            /* A raw target with a NULL query produces no matches. Evaluated
             * operands still need their SQL side effects, so those scans must read
             * and evaluate rows that  pass the remaining filters. */
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
 * @brief Checks a row's filters, then evaluates its configured mmsearch operands.
 *
 * Makes only the required sequence sources readable during evaluation. Restores
 * the original slot values before returning or propagating either a PostgreSQL
 * or C++ error, so the caller can safely buffer the row.
 * @return The filter result and evaluated tabget and query values, with NULL flags.
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
                            ereport(ERROR,
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
 * @brief Produces the next row or vector batch from the base scan.
 *
 * Ordinary rows are filtered and projected immediately. With mmsearch, collects
 * a batch, runs alignment and returns one projected row per accepted, match.
 * VectorAgg uses its own batch projection path
 * @return A result slot, or an empty slot  when all input and buffered matches are consumed.
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

        /* Alternate between collecting input and returning alignment results.
         * Run a batch when it fills or the source ends. While draining it, update
         * helper placeholders for each match before projecting its row. */
        for (;;) {
            /* Return buffered matches. */
            if (runtime.buffer.has_finished_writing()) {
                bioseq_log<logging_level::debug>(
                    "[bioseq] pg: Inner_CustomExecScan() draining output cursor");
                /* The parent has consumed the previous result. Clear its slot before
                 * resetting per-row memory. Buffered input rows and match results remain
                 * alive for the rest of this batch. */
                ExecClearTuple(node->ps.ps_ResultTupleSlot);
                ResetExprContext(econtext);
                econtext->ecxt_scantuple = nullptr;
                econtext->ecxt_outertuple = nullptr;
                econtext->ecxt_innertuple = nullptr;
                auto next = runtime.buffer.read_next_output();
                if (!next) {
                    if (runtime.source_exhausted) {
                        bioseq::postgres::table::alignment_execution::
                            mark_alignment_pass_fully_drained(
                                runtime.alignment_registration);
                        bioseq_log<logging_level::debug>(
                            "[bioseq] pg: Inner_CustomExecScan() final output cursor drained; "
                            "returning empty slot (EOF)");
                        return ExecClearTuple(node->ps.ps_ResultTupleSlot);
                    }
                    /* This batch is drained. Clear it and  resume reading the table. */
                    runtime.buffer.clear_slots();
                    continue;
                }
                bioseq::storage::note_mmsearch_output_context_reset_for_test();

                if (runtime.edit_predicate_active &&
                    (runtime.edit_exclusive_upper_bound <= 0 ||
                     next->match->edit_distance >=
                         static_cast<std::uint64_t>(runtime.edit_exclusive_upper_bound))) {
                    /* The prefilter can only reject impossible matches. Apply the exact
                     * edit-distance bound here, including rejection at the strict < boundary. */
                    InstrCountFiltered1(node, 1);
                    continue;
                }

                /* Supply this match's values to the placeholders used by ExecProject. */
                populate_placeholder_consts(runtime.placeholder_consts, *next->match);

                /* symbol_count belongs to the input row. Look it up by row number
                 * separtely from the alignment result. */
                populate_symbol_count_placeholders(runtime.placeholder_consts,
                                                   node->ss_currentRelation,
                                                   next->sequence_rowno,
                                                   bioseq_scan_state->snapshot,
                                                   runtime.aligner_state.read_sequence_index);

                /* Make sources readable for this projection, then put their original
                 * locators back. Another match from the same buffered row must survive
                 * the next reset of per-row memory. */
                const mmsearch_source_restore source_restore =
                    capture_mmsearch_sources(runtime,
                                             next->scan_slot_copy,
                                             econtext->ecxt_per_tuple_memory,
                                             mmsearch_source_capture_role::output_projection);
                TupleTableSlot* projected = nullptr;
                std::exception_ptr projection_error;
                MemoryContext saved_context = CurrentMemoryContext;
                ErrorData* volatile projection_pg_error = nullptr;
#if PG_VERSION_NUM >= 190000
                // PG19 specializes projections for the input slot type. Compile  against
                // the buffer's MinimalTuple slot and restore the scan metadata on every exit
                const auto saved_scan_ops = node->ps.scanops;
                const auto saved_scan_desc = node->ps.scandesc;
                const bool saved_scan_ops_set = node->ps.scanopsset;
                const bool saved_scan_ops_fixed = node->ps.scanopsfixed;
                const auto restore_scan_projection_metadata = [&] {
                    node->ps.scanops = saved_scan_ops;
                    node->ps.scandesc = saved_scan_desc;
                    node->ps.scanopsset = saved_scan_ops_set;
                    node->ps.scanopsfixed = saved_scan_ops_fixed;
                };
#endif

                /* PostgreSQL ERROR skips C++ destructors. Save original slot values
                 * before PG_TRY, then restore them on success and both error paths. */
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

                        /* Resolve projection Vars against the buffered input row. */
                        econtext->ecxt_scantuple = next->scan_slot_copy;

#if PG_VERSION_NUM >= 190000
                        node->ps.scanops = next->scan_slot_copy->tts_ops;
                        node->ps.scandesc = next->scan_slot_copy->tts_tupleDescriptor;
                        node->ps.scanopsset = true;
                        node->ps.scanopsfixed = true;
#endif
                        ProjectionInfo* proj =
                            ExecBuildProjectionInfo(runtime.newtargetlist,
                                                    econtext,
                                                    node->ps.ps_ResultTupleSlot,
                                                    &(node->ps),
                                                    next->scan_slot_copy->tts_tupleDescriptor);
#if PG_VERSION_NUM >= 190000
                        restore_scan_projection_metadata();
#endif
                        projected = ExecProject(proj);
                        restore_mmsearch_sources(next->scan_slot_copy, source_restore);
                    } catch (...) {
#if PG_VERSION_NUM >= 190000
                        restore_scan_projection_metadata();
#endif
                        MemoryContextSwitchTo(saved_context);
                        restore_mmsearch_sources(next->scan_slot_copy, source_restore);
                        projection_error = std::current_exception();
                    }
                }
                PG_CATCH();
                {
#if PG_VERSION_NUM >= 190000
                    restore_scan_projection_metadata();
#endif
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

            /* Collect input rows. */
            TupleTableSlot* slot = nullptr;

            /* Fetch one scalar row. Filters run after both fetch paths meet below. */
            if (!bioseq_scan_state->vectorization.is_enabled) {
                bioseq_log<logging_level::debug>(
                    "[bioseq] pg: Inner_CustomExecScan() scalar fetch via ExecScanFetch");
                slot = exec_scan_fetch(node, access_mtd, recheck_mtd);

                if (TupIsNull(slot)) {
                    runtime.source_exhausted = true;
                    bioseq_log<logging_level::debug>("[bioseq] pg: Inner_CustomExecScan() source "
                                                     "exhausted; running batch processor");
                    if (runtime.buffer.run_batch_and_build_cursors()) {
                        bioseq::postgres::table::alignment_execution::
                            merge_alignment_batch(
                                runtime.alignment_registration,
                                runtime.buffer.execution_stats());
                    }
                    continue; /* Next iteration enters the reading stage. */
                }
            }
            /* Apply vector filters to a batch, then extract its surviving rows
             * one at a time for the remaining scan work. */
            else {
                /* Fetch a, vector when no rows remain in the current one. */
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
                            bioseq::postgres::table::alignment_execution::
                                merge_alignment_batch(
                                    runtime.alignment_registration,
                                    runtime.buffer.execution_stats());
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

                /* No surviving  row remains in this vector. Fetch another batch. */
                if (!row_found)
                    continue;
            }

            /* Give each input row fresh expression memory. Evaluated submission
             * copies the sequence wrappers it needs into the batch before this memory
             * is reset for the nept row. */
            ResetExprContext(econtext);
            econtext->ecxt_scantuple = slot;

            /* Ordinary scans need loaded sequence values for immediate projection.
             * Alignment and lazy-source scans keep locators until their own consumers
             * request the sequence data. */
            if (!runtime.buffer.mmsearch_active() && !runtime.locator_passthrough &&
                !runtime.lazy_source_projection) {
                hydrate_scan_slot(node->ss_currentScanDesc, slot, /*force=*/true);
            }
            /* A lazy source without mmsearch can prepare its  bound sequences now.
             * Alignment scans promote them temporarily during operand evaluation or
             * once per output match, preserving the buffered locators between uses. */
            if (runtime.lazy_source_projection && !runtime.locator_passthrough &&
                !runtime.buffer.mmsearch_active()) {
                promote_slice_project_sources(bioseq_scan_state,
                                              slot,
                                              econtext->ecxt_per_tuple_memory,
                                              /*allow_locator_passthrough=*/false,
                                              slice_project_source_slot_layout::physical_relation,
                                              slot->tts_tupleDescriptor);
            }

            /* Apply remaining scalar filters to rows from either fetch path.
             * Owned mmsearch calls run through the alignment buffer separately */
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
                            bioseq::postgres::table::alignment_execution::
                                merge_alignment_batch(
                                    runtime.alignment_registration,
                                    runtime.buffer.execution_stats());
                        }
                    }
                } else {
                    if (runtime.mmsearch_null_input) {
                        /* A NULL constant or parameter disables alignment, but an evaluated
                         * target still runs once for each row that passes the filters. */
                        if (runtime.match_evaluated_submission &&
                            runtime.match_target_expr_state != nullptr) {
                            bool ignored_null = false;
                            (void)ExecEvalExprSwitchContext(
                                runtime.match_target_expr_state, econtext, &ignored_null);
                        }
                        continue;
                    }
                    /* Without alignment  work, project the row immediately into the scan
                     * output layout expected by its parent. */
                    bioseq_log<logging_level::debug>(
                        "[bioseq] pg: Inner_CustomExecScan() qual passed; no mmsearch qual");

                    /* Use the input row's TID to locate its symbol counts. */
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
                        Form_pg_attribute rowno_attribute = bioseq::postgres::support::tuple_descriptor_attribute(
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

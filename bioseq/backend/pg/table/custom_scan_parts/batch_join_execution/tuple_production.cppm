/**
 * @file
 * @brief Combine query rows, target rows, and alignment results into join output.
 */
module;
// Include Highway here so imported target-specific types share one
// translation-unit definition
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <span>
#include <string>
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

export module bioseq.postgres.table:custom_scan_batch_join_execution_tuple_production;

import bioseq.postgres.scheduler.dispatch_buffer.api;
import bioseq.postgres.table.scan_state;
import bioseq.postgres.support;
import bioseq.sequences.cigar;
import bioseq.postgres.types.compact_sequence;

import :custom_scan_execution_method_contracts;
import :custom_scan_batch_join_execution_runtime_state_contracts;
import :custom_scan_scan_runtime_state_contracts;
import :custom_scan_oid_expression_support_oid_catalog_core_contracts;
import :custom_scan_batch_join_execution_resource_lifecycle_contracts;
import :custom_scan_batch_join_execution_tuple_production_contracts;

using namespace bioseq::postgres::support;

export {
    /**
     * @brief Fill a result column such as score, coordinates, or CIGAR from one match.
     * @param output_context Memory context for variable-length results.
     */
    void batch_join_set_placeholder_value(TupleTableSlot* slot,
                                          const batch_join_placeholder_site& site,
                                          const bioseq::simd::match_result& match,
                                          const bioseq_stub_oids& oids,
                                          MemoryContext output_context) {
        if (slot == nullptr || site.resno <= 0 || site.resno > slot->tts_tupleDescriptor->natts) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin placeholder resno is invalid")));
        }
        Datum value = static_cast<Datum>(0);
        bool is_null = false;
        // Converts an unsigned match coordinate to PostgreSQL bigint with range checking.
        auto bigint = [&value](const std::uint64_t input) {
            if (input >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                ereport(ERROR,
                        (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                         errmsg("BioseqBatchJoin match coordinate exceeds bigint")));
            }
            value = Int64GetDatum(static_cast<std::int64_t>(input));
        };
        if (site.funcid == oids.score_oid || site.funcid == oids.dp_score_oid) {
            value = Int32GetDatum(match.dp_score);
        } else if (site.funcid == oids.query_len_oid) {
            bigint(match.query_len);
        } else if (site.funcid == oids.query_start_oid) {
            bigint(match.query_start);
        } else if (site.funcid == oids.query_end_oid) {
            bigint(match.query_end);
        } else if (site.funcid == oids.target_len_oid) {
            bigint(match.target_len);
        } else if (site.funcid == oids.target_start_oid) {
            bigint(match.target_start);
        } else if (site.funcid == oids.target_end_oid) {
            bigint(match.target_end);
        } else if (site.funcid == oids.num_matches_oid) {
            bigint(match.num_matches);
        } else if (site.funcid == oids.block_length_oid) {
            bigint(match.block_length);
        } else if (site.funcid == oids.edit_oid) {
            bigint(match.edit_distance);
        } else if (site.funcid == oids.mapq_oid) {
            value = Int32GetDatum(match.mapq);
        } else if (site.funcid == oids.chain_score_oid) {
            value = Int32GetDatum(match.chain_score);
        } else if (site.funcid == oids.dp_max_score_oid) {
            value = Int32GetDatum(match.dp_max_score);
        } else if (site.funcid == oids.seed_count_oid) {
            value = Int32GetDatum(match.seed_count);
        } else if (site.funcid == oids.secondary_chain_score_oid) {
            value = Int32GetDatum(match.secondary_chain_score);
        } else if (site.funcid == oids.is_reverse_oid) {
            value = BoolGetDatum(match.is_reverse);
        } else if (site.funcid == oids.is_primary_oid) {
            value = BoolGetDatum(match.is_primary);
        } else if (site.funcid == oids.has_cigar_oid) {
            value = BoolGetDatum(match.has_cigar);
        } else if (site.funcid == oids.divergence_oid) {
            value = Float4GetDatum(match.divergence);
        } else if (site.funcid == oids.cigar_oid) {
            is_null = !match.has_cigar;
            if (match.has_cigar) {
                MemoryContext previous = MemoryContextSwitchTo(output_context);
                value = bioseq::postgres::support::pg_datum::from_pointer(make_pg_cigar_from_runs(
                    std::span<const bioseq::cigar_run>{match.cigar.data(), match.cigar.size()})).raw();
                MemoryContextSwitchTo(previous);
            }
        } else {
            is_null = true;
        }
        slot->tts_values[site.resno - 1] = value;
        slot->tts_isnull[site.resno - 1] = is_null;
    }

    /** Original target value to  restore after producing a match row. */
    struct batch_join_target_source_datum {
        AttrNumber resno = InvalidAttrNumber;
        Datum datum = Datum{0};
    };

    /** Saved target values and their column positions. The values remain borrowed. */
    struct batch_join_target_source_restore {
        batch_join_target_source_datum* sources = nullptr;
        std::size_t count = 0u;
    };

    /**
     * @brief Save target values before temporarily converting their locators.
     * @param output_context Context that owns the array of saved values.
     */
    [[nodiscard]] batch_join_target_source_restore capture_batch_join_target_sources(
        bioseq_scan_runtime& target_runtime, TupleTableSlot* slot, MemoryContext output_context) {
        batch_join_target_source_restore restore{};
        if (!target_runtime.lazy_source_projection || slot == nullptr) {
            return restore;
        }
        if (output_context == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("BioseqBatchJoin target output context is unavailable")));
        }
        slot_getallattrs(slot);
        const std::size_t required_bindings =
            static_cast<std::size_t>(slot->tts_tupleDescriptor->natts + 1);
        if (target_runtime.lazy_source_projected_bindings.size() != required_bindings) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqBatchJoin target source binding layout changed")));
        }
        for (int index = 0; index < slot->tts_tupleDescriptor->natts; ++index) {
            const AttrNumber resno = static_cast<AttrNumber>(index + 1);
            if (target_runtime.lazy_source_projected_bindings[resno].physical_attno ==
                    InvalidAttrNumber ||
                slot->tts_isnull[index]) {
                continue;
            }
            ++restore.count;
        }
        if (restore.count == 0u)
            return restore;
        restore.sources = static_cast<batch_join_target_source_datum*>(MemoryContextAlloc(
            output_context, restore.count * sizeof(batch_join_target_source_datum)));
        std::size_t destination = 0u;
        for (int index = 0; index < slot->tts_tupleDescriptor->natts; ++index) {
            const AttrNumber resno = static_cast<AttrNumber>(index + 1);
            if (target_runtime.lazy_source_projected_bindings[resno].physical_attno ==
                    InvalidAttrNumber ||
                slot->tts_isnull[index]) {
                continue;
            }
            restore.sources[destination++] = batch_join_target_source_datum{
                .resno = resno,
                .datum = slot->tts_values[index],
            };
        }
        Assert(destination == restore.count);
        return restore;
    }

    /** Put the original borrowed target values back after producing an output row. */
    void
    restore_batch_join_target_sources(TupleTableSlot* slot,
                                      const batch_join_target_source_restore& restore) noexcept {
        if (slot == nullptr)
            return;
        for (std::size_t index = 0u; index < restore.count; ++index) {
            const AttrNumber resno = restore.sources[index].resno;
            if (resno > 0 && resno <= slot->tts_tupleDescriptor->natts) {
                slot->tts_values[resno - 1] = restore.sources[index].datum;
            }
        }
    }

    /** Give stored target values a normal varlena header before reading sequence fields. */
    void normalize_batch_join_target_sources(TupleTableSlot* slot,
                                             const batch_join_target_source_restore& restore,
                                             MemoryContext output_context) {
        for (std::size_t index = 0u; index < restore.count; ++index) {
            const batch_join_target_source_datum& source = restore.sources[index];
            if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(source.datum))) {
                continue;
            }
            /* MinimalTuple storage can use a short varlena header. The sequenc
             * locator decoder expects the normal four-byte header, so detoast into
             * this output row's memory context before reading the  sequence header. */
            MemoryContext previous = MemoryContextSwitchTo(output_context);
            bioseq::pg_types::seq* flat =
                reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(source.datum));
            MemoryContextSwitchTo(previous);
            if (static_cast<std::size_t>(bioseq::postgres::support::varlena_4byte_size(flat)) < bioseq::pg_types::seq::header_bytes()) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqBatchJoin target source is truncated"),
                         errdetail("attribute=%d bytes=%zu",
                                   static_cast<int>(source.resno),
                                   static_cast<std::size_t>(bioseq::postgres::support::varlena_4byte_size(flat)))));
            }
            slot->tts_values[source.resno - 1] = PointerGetDatum(flat);
        }
    }

    /**
     * @brief Combine a target match with its query row and evaluate the output projection.
     * @return The projected row, or null if the output predicate rejects the match.
     */
    TupleTableSlot*
    batch_join_project_output(CustomScanState* node,
                              batch_join_runtime& runtime,
                              const bioseq::scheduler::dispatch_next_output& output) {
        if (output.scan_slot_copy == nullptr || output.match == nullptr ||
            output.query_ordinal >= runtime.query_tuples.size()) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqBatchJoin output references invalid inputs")));
        }
        ExprContext* expression_context = node->ss.ps.ps_ExprContext;
        if (expression_context == nullptr || expression_context->ecxt_per_tuple_memory == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin expression context is missing")));
        }
        /* We bypass ExecScan, so own the per-output reset it would normally do. */
        ResetExprContext(expression_context);
        TupleTableSlot* query_slot = runtime.query_tuples[output.query_ordinal].slot;
        TupleTableSlot* combined = node->ss.ss_ScanTupleSlot;
        ExecClearTuple(combined);
        slot_getallattrs(output.scan_slot_copy);
        slot_getallattrs(query_slot);
        if (runtime.target_column_count > output.scan_slot_copy->tts_tupleDescriptor->natts ||
            runtime.query_column_count > query_slot->tts_tupleDescriptor->natts ||
            runtime.target_column_count + runtime.query_column_count >
                combined->tts_tupleDescriptor->natts) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin combined tuple layout is invalid")));
        }
        /* A parent projection or join may need sequence values from this target.
         * Convert locators in the output copy now, keeping the values in this
         * row's context. Restore the buffered tuple afterward so later matches
         * can, reuse its original locators. */
        bioseq_scan_runtime* target_runtime = nullptr;
        if (runtime.target_child != nullptr && IsA(runtime.target_child, CustomScanState)) {
            auto* target_custom = reinterpret_cast<CustomScanState*>(runtime.target_child);
            if (target_custom->methods == bioseq_column_scan_execute_methods()) {
                auto* target_state = scan_state::from_custom_scan_state(target_custom);
                target_runtime = &bioseq_runtime(target_state);
                if (!target_runtime->lazy_source_projection) {
                    ereport(ERROR,
                            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                             errmsg("BioseqBatchJoin target source route is unavailable")));
                }
            }
        }
        if (target_runtime == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("BioseqBatchJoin target source provider is unavailable")));
        }
        const batch_join_target_source_restore target_source_restore =
            capture_batch_join_target_sources(
                *target_runtime, output.scan_slot_copy, expression_context->ecxt_per_tuple_memory);
        MemoryContext saved_context = CurrentMemoryContext;
        ErrorData* volatile promotion_pg_error = nullptr;
        std::exception_ptr promotion_cpp_error;
        PG_TRY();
        {
            try {
                auto* target_custom = reinterpret_cast<CustomScanState*>(runtime.target_child);
                auto* target_state = scan_state::from_custom_scan_state(target_custom);
                normalize_batch_join_target_sources(output.scan_slot_copy,
                                                    target_source_restore,
                                                    expression_context->ecxt_per_tuple_memory);
                promote_slice_project_sources(
                    target_state,
                    output.scan_slot_copy,
                    expression_context->ecxt_per_tuple_memory,
                    /*allow_locator_passthrough=*/true,
                    slice_project_source_slot_layout::projected_child,
                    ExecGetResultType(reinterpret_cast<PlanState*>(target_custom)));
                slot_getallattrs(output.scan_slot_copy);
                for (int i = 0; i < runtime.target_column_count; ++i) {
                    combined->tts_values[i] = output.scan_slot_copy->tts_values[i];
                    combined->tts_isnull[i] = output.scan_slot_copy->tts_isnull[i];
                }
                restore_batch_join_target_sources(output.scan_slot_copy, target_source_restore);
            } catch (...) {
                MemoryContextSwitchTo(saved_context);
                restore_batch_join_target_sources(output.scan_slot_copy, target_source_restore);
                promotion_cpp_error = std::current_exception();
            }
        }
        PG_CATCH();
        {
            MemoryContextSwitchTo(saved_context);
            restore_batch_join_target_sources(output.scan_slot_copy, target_source_restore);
            promotion_pg_error = CopyErrorData();
            FlushErrorState();
        }
        PG_END_TRY();
        if (promotion_pg_error != nullptr) {
            ErrorData* exact_error = promotion_pg_error;
            ReThrowError(exact_error);
        }
        if (promotion_cpp_error != nullptr) {
            std::rethrow_exception(promotion_cpp_error);
        }
        for (int i = 0; i < runtime.query_column_count; ++i) {
            const int destination = runtime.target_column_count + i;
            combined->tts_values[destination] = query_slot->tts_values[i];
            combined->tts_isnull[destination] = query_slot->tts_isnull[i];
        }
        const bioseq_stub_oids& oids = get_cached_stub_oids();
        for (const batch_join_placeholder_site& site : runtime.placeholder_sites) {
            batch_join_set_placeholder_value(
                combined, site, *output.match, oids, expression_context->ecxt_per_tuple_memory);
        }
        ExecStoreVirtualTuple(combined);
        expression_context->ecxt_scantuple = combined;
        if (node->ss.ps.qual != nullptr && !ExecQual(node->ss.ps.qual, expression_context)) {
            InstrCountFiltered1(&node->ss, 1);
            /* The next loop step may release  this target's dispatch buffer Clear
             * borrowed sequence references before that storage is reset. */
            clear_recipe_scan_parent_tuple_references(node);
            return nullptr;
        }
        return node->ss.ps.ps_ProjInfo != nullptr ? ExecProject(node->ss.ps.ps_ProjInfo) : combined;
    }

    /**
     * @brief Allocate execution state for a stored join plan.
     * @return State in the current PostgreSQL memory context.
     */
    Node* bioseq_batch_join_create_custom_scan_state(CustomScan* cscan) {
        /* Prepared plans can be reused after the executor context is gone. Keep
         * the plan unchanged herk. Planning must already have replaced helper
         * calls with INDEX_VAR references to result columns. */
        if (cscan == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin executor plan is missing")));
        }
        auto* state = reinterpret_cast<batch_join_state*>(
            newNode(sizeof(batch_join_state), T_CustomScanState));
        state->custom_scan_state.methods = bioseq_batch_join_execute_methods();
        state->runtime = nullptr;
        return reinterpret_cast<Node*>(&state->custom_scan_state);
    }

} // export custom-scan partition declarations

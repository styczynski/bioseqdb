/**
 * @file
 * @brief Executes BatchJoin materialization, dispatch, projection, cleanup, and callback tables.
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

export module bioseq.postgres.table:custom_scan_batch_join_execution;

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

import :custom_scan_join_runtime;

using namespace bioseq::postgres::support;

export {

    [[nodiscard]] batch_join_runtime* try_get_batch_join_runtime(CustomScanState* node) noexcept {
        auto* state = reinterpret_cast<batch_join_state*>(node);
        return state == nullptr ? nullptr : static_cast<batch_join_runtime*>(state->runtime);
    }

    /* Recipe-aware custom scans expose virtual rows which borrow retained EOH
     * wrappers. Clear every parent-visible slot and expression-context pointer
     * before the owning dispatch arena is reset. */
    void clear_recipe_scan_parent_tuple_references(CustomScanState* node) {
        if (node == nullptr)
            return;
        std::array<TupleTableSlot*, 2> slots{
            node->ss.ps.ps_ResultTupleSlot,
            node->ss.ss_ScanTupleSlot,
        };
        for (std::size_t index = 0u; index < slots.size(); ++index) {
            TupleTableSlot* slot = slots[index];
            if (slot == nullptr)
                continue;
            bool duplicate = false;
            for (std::size_t previous = 0u; previous < index; ++previous) {
                duplicate = duplicate || slots[previous] == slot;
            }
            if (!duplicate)
                ExecClearTuple(slot);
        }
        if (ExprContext* context = node->ss.ps.ps_ExprContext; context != nullptr) {
            context->ecxt_scantuple = nullptr;
            context->ecxt_outertuple = nullptr;
            context->ecxt_innertuple = nullptr;
            ResetExprContext(context);
        }
    }

    void batch_join_clear_parent_tuple_references(CustomScanState* node,
                                                  batch_join_runtime& runtime) {
        clear_recipe_scan_parent_tuple_references(node);
        runtime.current_target_slot = nullptr;
    }

    void batch_join_release_executor_resources_checked(CustomScanState* node,
                                                       batch_join_runtime& runtime,
                                                       const bool delete_query_context) {
        if (runtime.cleanup_done) {
            return;
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [node, &runtime] { batch_join_clear_parent_tuple_references(node, runtime); });
        if (runtime.target_active) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [&runtime] { runtime.buffer.finish_join_target(); });
            runtime.target_active = false;
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&runtime] { runtime.buffer.reset_join_runtime_for_rescan(); });
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&runtime] { runtime.buffer.release_join_slice_async_owner_after_drain(); });
        runtime.buffer.clear_sequence_source_provider_binding();
        for (batch_join_query_tuple& query : runtime.query_tuples) {
            if (query.slot != nullptr) {
                TupleTableSlot* finished = query.slot;
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [finished] { ExecDropSingleTupleTableSlot(finished); });
                query.slot = nullptr;
            }
        }
        runtime.query_tuples.clear();
        runtime.all_query_refs.clear();
        if (runtime.query_staging_slot != nullptr) {
            TupleTableSlot* staging = runtime.query_staging_slot;
            bioseq::postgres::support::invoke_pg_as_cpp(
                [staging] { ExecDropSingleTupleTableSlot(staging); });
            runtime.query_staging_slot = nullptr;
        }
        if (runtime.query_tuple_context != nullptr) {
            MemoryContext context = runtime.query_tuple_context;
            bioseq::postgres::support::invoke_pg_as_cpp([context, delete_query_context] {
                if (delete_query_context) {
                    MemoryContextDelete(context);
                } else {
                    MemoryContextReset(context);
                }
            });
            runtime.query_tuple_context = nullptr;
        }
        /* Release admission only after the owning tuple context was reset/deleted. */
        runtime.query_permits.clear();
        runtime.query_reserved_admission_bytes = 0;
        runtime.query_actual_materialized_bytes = 0;
        runtime.queries_materialized = false;
        if (runtime.query_child_initialized && !runtime.query_child_ended) {
            PlanState* finished = runtime.query_child;
            bioseq::postgres::support::invoke_pg_as_cpp([finished] { ExecEndNode(finished); });
            runtime.query_child_ended = true;
        }
        if (runtime.target_child_initialized && !runtime.target_child_ended) {
            PlanState* finished = runtime.target_child;
            bioseq::postgres::support::invoke_pg_as_cpp([finished] { ExecEndNode(finished); });
            runtime.target_child_ended = true;
        }
        if (node != nullptr) {
            node->custom_ps = NIL;
        }
        runtime.cleanup_done = true;
    }

    void batch_join_abandon_executor_resources_after_error(CustomScanState* node,
                                                           batch_join_runtime& runtime) noexcept {
        if (runtime.cleanup_done) {
            return;
        }
        runtime.buffer.abandon_join_runtime_after_pg_error_noexcept();
        runtime.buffer.clear_sequence_source_provider_binding();
        for (batch_join_query_tuple& query : runtime.query_tuples) {
            query.slot = nullptr;
            query.tuple = nullptr;
        }
        runtime.query_tuples.clear();
        runtime.all_query_refs.clear();
        runtime.placeholder_sites.clear();
        runtime.query_tuple_context = nullptr;
        runtime.query_staging_slot = nullptr;
        runtime.query_permits.clear();
        runtime.query_reserved_admission_bytes = 0;
        runtime.query_actual_materialized_bytes = 0;
        runtime.queries_materialized = false;
        runtime.target_active = false;
        runtime.current_target_slot = nullptr;
        runtime.query_child = nullptr;
        runtime.target_child = nullptr;
        runtime.query_child_ended = true;
        runtime.target_child_ended = true;
        runtime.query_child_initialized = false;
        runtime.target_child_initialized = false;
        if (node != nullptr) {
            node->custom_ps = NIL;
        }
        runtime.cleanup_done = true;
    }

    void batch_join_runtime_cleanup(void* argument) {
        auto* state = static_cast<batch_join_state*>(argument);
        if (state == nullptr || state->runtime == nullptr) {
            return;
        }
        auto* runtime = static_cast<batch_join_runtime*>(state->runtime);
        batch_join_abandon_executor_resources_after_error(&state->custom_scan_state, *runtime);
        state->runtime = nullptr;
        std::destroy_at(runtime);
    }

    void batch_join_clear_query_tuples(batch_join_runtime& runtime, const bool drop_slots) {
        if (drop_slots) {
            for (batch_join_query_tuple& query : runtime.query_tuples) {
                if (query.slot != nullptr) {
                    ExecDropSingleTupleTableSlot(query.slot);
                    query.slot = nullptr;
                }
            }
        }
        runtime.query_tuples.clear();
        runtime.all_query_refs.clear();
        if (runtime.query_staging_slot != nullptr) {
            ExecDropSingleTupleTableSlot(runtime.query_staging_slot);
            runtime.query_staging_slot = nullptr;
        }
        if (runtime.query_tuple_context != nullptr) {
            MemoryContextReset(runtime.query_tuple_context);
        }
        /* The permit covers allocations owned by query_tuple_context (plus the
         * MinimalTuple slots above).  Release only after those owners are gone. */
        runtime.query_permits.clear();
        runtime.query_reserved_admission_bytes = 0;
        runtime.query_actual_materialized_bytes = 0;
        runtime.queries_materialized = false;
    }

    /* A child PlanState may legally return a pass-through scan/child slot rather
     * than its ps_ResultTupleSlot.  Validate the slot that ExecProcNode actually
     * returned before reading or copying it; the post-setrefs Plan targetlist is
     * the planning ABI, while this is the independent executor ABI. */
    void batch_join_validate_returned_child_slot(TupleTableSlot* slot,
                                                 const int expected_columns,
                                                 const AttrNumber sequence_resno,
                                                 const AttrNumber rowno_resno,
                                                 const std::uint8_t alphabet_code,
                                                 const std::uint8_t storage_unit_byte_width,
                                                 const char* role) {
        TupleDesc descriptor = slot == nullptr ? nullptr : slot->tts_tupleDescriptor;
        const bool rowno_required = rowno_resno != InvalidAttrNumber;
        if (descriptor == nullptr || expected_columns <= 0 ||
            descriptor->natts != expected_columns || sequence_resno <= 0 ||
            sequence_resno > expected_columns ||
            (rowno_required && (rowno_resno <= 0 || rowno_resno > expected_columns))) {
            ereport(
                ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("BioseqBatchJoin returned %s slot layout is invalid", role),
                 errdetail("expected_columns=%d actual_columns=%d sequence_resno=%d rowno_resno=%d",
                           expected_columns,
                           descriptor == nullptr ? -1 : descriptor->natts,
                           sequence_resno,
                           rowno_resno)));
        }
        const Oid sequence_type = get_cached_stub_oids().bioseq_seq_type_oid;
        const int32 expected_typmod =
            compact_sequence_stdc_type_params{alphabet_code, storage_unit_byte_width}.encode();
        Form_pg_attribute sequence_attribute = TupleDescAttr(descriptor, sequence_resno - 1);
        Form_pg_attribute rowno_attribute =
            rowno_required ? TupleDescAttr(descriptor, rowno_resno - 1) : nullptr;
        if (sequence_attribute->atttypid != sequence_type ||
            sequence_attribute->atttypmod != expected_typmod ||
            sequence_attribute->attcollation != get_typcollation(sequence_type) ||
            (rowno_required &&
             (rowno_attribute->atttypid != INT8OID || rowno_attribute->atttypmod != -1 ||
              OidIsValid(rowno_attribute->attcollation)))) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("BioseqBatchJoin returned %s slot types are invalid", role),
                     errdetail("sequence_type=%u sequence_typmod=%d sequence_collation=%u "
                               "rowno_type=%u rowno_typmod=%d rowno_collation=%u",
                               sequence_attribute->atttypid,
                               sequence_attribute->atttypmod,
                               sequence_attribute->attcollation,
                               rowno_attribute == nullptr ? InvalidOid : rowno_attribute->atttypid,
                               rowno_attribute == nullptr ? -1 : rowno_attribute->atttypmod,
                               rowno_attribute == nullptr ? InvalidOid
                                                          : rowno_attribute->attcollation)));
        }
    }

    /* Convert validation failures raised while automatic C++ row metadata is live
     * into normal C++ unwinding. A raw PostgreSQL longjmp here would skip the two
     * retained-attribute vector destructors. */
    [[noreturn]] void batch_join_raise_materialization_as_cpp(const int sqlstate,
                                                              const char* message,
                                                              const std::string detail = {}) {
        const char* detail_cstr = detail.c_str();
        bioseq::postgres::support::invoke_pg_as_cpp([sqlstate, message, detail_cstr] {
            if (detail_cstr[0] == '\0') {
                ereport(ERROR, (errcode(sqlstate), errmsg("%s", message)));
            }
            ereport(ERROR,
                    (errcode(sqlstate), errmsg("%s", message), errdetail("%s", detail_cstr)));
        });
        pg_unreachable();
    }

    void batch_join_materialize_queries(batch_join_runtime& runtime) {
        if (runtime.queries_materialized) {
            return;
        }
        if (runtime.query_child == nullptr || runtime.query_tuple_context == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin query child is not initialized")));
        }
        if (!std::isfinite(runtime.query_planner_rows) || runtime.query_planner_rows < 0.0 ||
            runtime.query_planner_width < 0) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqBatchJoin query planner estimate is invalid")));
        }
        /* Reserve the planner's rows*width envelope before asking the child for
         * its first tuple.  Actual whole-MinimalTuple upper bounds below extend
         * this reservation before detoast/copy whenever the estimate is low. */
        const double bounded_rows =
            std::min(runtime.query_planner_rows,
                     static_cast<double>((std::numeric_limits<std::uint64_t>::max)()));
        const std::uint64_t planner_rows = static_cast<std::uint64_t>(std::ceil(bounded_rows));
        const std::uint64_t planner_tuple_bytes = bioseq::common::saturating_add(
            static_cast<std::uint64_t>(runtime.query_planner_width),
            static_cast<std::uint64_t>(MAXALIGN(MINIMAL_TUPLE_DATA_OFFSET) +
                                       sizeof(batch_join_query_tuple) +
                                       sizeof(bioseq::scheduler::dispatch_join_query_ref) + 256u));
        const std::uint64_t scratch_metadata_bytes = bioseq::common::saturating_mul(
            bioseq::common::saturating_add(
                static_cast<std::uint64_t>(sizeof(TupleTableSlot)),
                bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(runtime.query_column_count),
                    static_cast<std::uint64_t>(
                        sizeof(Datum) + sizeof(bool) +
                        sizeof(batch_join_query_tuple::retained_expanded_attribute)))),
            bioseq::parallelism::k_vector_preallocation_headroom_factor);
        /* One reusable virtual staging slot and one per-row descriptor vector are
         * allocated after the planner permit but before the row-specific extension.
         * Admit their worst query-column-count shape once, even when the planner
         * estimated zero rows/width. */
        runtime.query_planner_admission_bytes = bioseq::common::saturating_add(
            bioseq::common::saturating_mul(planner_rows, planner_tuple_bytes),
            scratch_metadata_bytes);
        {
            auto planner_permit =
                bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                    runtime.query_planner_admission_bytes);
            if (!planner_permit.has_value()) {
                batch_join_raise_materialization_as_cpp(
                    ERRCODE_OUT_OF_MEMORY,
                    "BioseqBatchJoin planned query relation cannot be admitted",
                    fmt::format("planned_rows={:.0f} planned_width={} required_bytes={}",
                                runtime.query_planner_rows,
                                runtime.query_planner_width,
                                runtime.query_planner_admission_bytes));
            }
            runtime.query_reserved_admission_bytes = runtime.query_planner_admission_bytes;
            runtime.query_permits.push_back(std::move(*planner_permit));
        }
        slice_async_owner_guard* query_async_owner = nullptr;
        for (;;) {
            TupleTableSlot* source = ExecProcNode(runtime.query_child);
            if (TupIsNull(source)) {
                break;
            }
            batch_join_validate_returned_child_slot(source,
                                                    runtime.query_column_count,
                                                    runtime.query_slot_seq_resno,
                                                    InvalidAttrNumber,
                                                    runtime.query_alphabet_code,
                                                    runtime.query_storage_unit_byte_width,
                                                    "query");
            slot_getallattrs(source);
            bool query_is_null = source->tts_isnull[runtime.query_slot_seq_resno - 1];
            if (query_is_null) {
                /* Strict INNER predicate: a NULL query cannot produce a match. */
                continue;
            }
            const Datum original = source->tts_values[runtime.query_slot_seq_resno - 1];
            const bool expanded_query = VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(original));
            const Oid sequence_type = get_cached_stub_oids().bioseq_seq_type_oid;
            pg_sequence_descriptor recipe_descriptor{};
            std::vector<batch_join_query_tuple::retained_expanded_attribute>
                expanded_source_attributes{};
            expanded_source_attributes.reserve(
                static_cast<std::size_t>(source->tts_tupleDescriptor->natts));
            for (int index = 0; index < source->tts_tupleDescriptor->natts; ++index) {
                if (source->tts_isnull[index] ||
                    TupleDescAttr(source->tts_tupleDescriptor, index)->atttypid != sequence_type) {
                    continue;
                }
                const Datum attribute = source->tts_values[index];
                if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(attribute))) {
                    continue;
                }
                const pg_sequence_descriptor descriptor =
                    bioseq::postgres::support::invoke_pg_as_cpp(
                        [attribute] { return classify_and_describe_sequence(attribute); });
                const bool expanded_kind =
                    descriptor.kind == pg_sequence_datum_kind::expanded_read_only ||
                    descriptor.kind == pg_sequence_datum_kind::expanded_read_write;
                if (!expanded_kind) {
                    batch_join_raise_materialization_as_cpp(
                        ERRCODE_DATA_CORRUPTED,
                        "BioseqBatchJoin expanded output descriptor is invalid");
                }
                const AttrNumber attno = static_cast<AttrNumber>(index + 1);
                expanded_source_attributes.push_back({
                    .attno = attno,
                    .datum = attribute,
                    .descriptor = descriptor,
                });
                if (attno == runtime.query_slot_seq_resno) {
                    recipe_descriptor = descriptor;
                }
            }
            if (expanded_query) {
                const bool valid_query_descriptor =
                    recipe_descriptor.alphabet_bound &&
                    recipe_descriptor.alphabet_code == runtime.query_alphabet_code &&
                    recipe_descriptor.storage_unit_byte_width ==
                        runtime.query_storage_unit_byte_width;
                if (!valid_query_descriptor) {
                    batch_join_raise_materialization_as_cpp(
                        ERRCODE_DATATYPE_MISMATCH,
                        "BioseqBatchJoin expanded query typmod changed after planning");
                }
                auto* attachment = bioseq::postgres::support::invoke_pg_as_cpp(
                    [original] { return pg_sequence_trace_attachment_for_datum(original); });
                auto* candidate = borrow_slice_async_owner_noexcept(attachment);
                if (candidate == nullptr) {
                    batch_join_raise_materialization_as_cpp(
                        ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                        "BioseqBatchJoin expanded query has no live async owner");
                }
                if (query_async_owner != nullptr && query_async_owner != candidate) {
                    batch_join_raise_materialization_as_cpp(
                        ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                        "BioseqBatchJoin expanded queries have distinct async owners");
                }
                query_async_owner = candidate;
            }
            const bool row_has_expanded_sequence = !expanded_source_attributes.empty();
            if (row_has_expanded_sequence) {
                if (runtime.query_staging_slot == nullptr) {
                    MemoryContext old = MemoryContextSwitchTo(runtime.query_tuple_context);
                    try {
                        runtime.query_staging_slot =
                            bioseq::postgres::support::invoke_pg_as_cpp([source] {
                                return MakeSingleTupleTableSlot(source->tts_tupleDescriptor,
                                                                &TTSOpsVirtual);
                            });
                        MemoryContextSwitchTo(old);
                    } catch (...) {
                        MemoryContextSwitchTo(old);
                        throw;
                    }
                } else if (runtime.query_staging_slot->tts_tupleDescriptor !=
                               source->tts_tupleDescriptor &&
                           !bioseq::postgres::support::invoke_pg_as_cpp(
                               [staging_descriptor =
                                    runtime.query_staging_slot->tts_tupleDescriptor,
                                source_descriptor = source->tts_tupleDescriptor] {
                                   return equalTupleDescs(staging_descriptor, source_descriptor);
                               })) {
                    batch_join_raise_materialization_as_cpp(
                        ERRCODE_DATA_CORRUPTED, "BioseqBatchJoin query staging descriptor changed");
                }
                TupleTableSlot* staging_slot = runtime.query_staging_slot;
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [staging_slot] { ExecClearTuple(staging_slot); });
                const int natts = source->tts_tupleDescriptor->natts;
                std::memcpy(runtime.query_staging_slot->tts_values,
                            source->tts_values,
                            static_cast<std::size_t>(natts) * sizeof(Datum));
                std::memcpy(runtime.query_staging_slot->tts_isnull,
                            source->tts_isnull,
                            static_cast<std::size_t>(natts) * sizeof(bool));
                for (const auto& attribute : expanded_source_attributes) {
                    runtime.query_staging_slot->tts_values[attribute.attno - 1] = Datum{0};
                    runtime.query_staging_slot->tts_isnull[attribute.attno - 1] = true;
                }
                runtime.query_staging_slot->tts_nvalid = natts;
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [staging_slot] { ExecStoreVirtualTuple(staging_slot); });
            }
            const std::uint64_t retained_recipe_bytes =
                row_has_expanded_sequence
                    ? bioseq::common::saturating_mul(
                          static_cast<std::uint64_t>(expanded_source_attributes.size()),
                          k_batch_join_retained_recipe_admission_bytes)
                    : 0u;
            const std::uint64_t flat_query_bytes =
                expanded_query
                    ? 0u
                    : static_cast<std::uint64_t>(bioseq::postgres::support::invoke_pg_as_cpp(
                          [original] { return toast_raw_datum_size(original); }));
            const std::uint64_t retained_expanded_bytes =
                bioseq::common::saturating_add(retained_recipe_bytes, flat_query_bytes);
            TupleTableSlot* tuple_shape_source =
                row_has_expanded_sequence ? runtime.query_staging_slot : source;
            const Size source_tuple_data =
                bioseq::postgres::support::invoke_pg_as_cpp([tuple_shape_source] {
                    return heap_compute_data_size(tuple_shape_source->tts_tupleDescriptor,
                                                  tuple_shape_source->tts_values,
                                                  tuple_shape_source->tts_isnull);
                });
            std::uint64_t tuple_upper_bound = bioseq::common::saturating_add(
                static_cast<std::uint64_t>(MAXALIGN(MINIMAL_TUPLE_DATA_OFFSET)),
                static_cast<std::uint64_t>(source_tuple_data));
            /* source_tuple_data includes the original (possibly external TOAST)
             * sequence datum. Adding the raw size rather than subtracting that datum
             * is deliberately conservative and covers the flat replacement without
             * inspecting or allocating any pass-by-reference value. */
            tuple_upper_bound =
                bioseq::common::saturating_add(tuple_upper_bound, retained_expanded_bytes);
            tuple_upper_bound = bioseq::common::saturating_add(
                tuple_upper_bound,
                bioseq::common::saturating_mul(
                    bioseq::common::saturating_mul(
                        static_cast<std::uint64_t>(expanded_source_attributes.size()),
                        static_cast<std::uint64_t>(
                            sizeof(batch_join_query_tuple::retained_expanded_attribute))),
                    bioseq::parallelism::k_vector_preallocation_headroom_factor));
            tuple_upper_bound = bioseq::common::saturating_add(
                tuple_upper_bound,
                static_cast<std::uint64_t>(
                    sizeof(batch_join_query_tuple) +
                    sizeof(bioseq::scheduler::dispatch_join_query_ref) + sizeof(TupleTableSlot) +
                    static_cast<std::size_t>(source->tts_tupleDescriptor->natts) *
                        (sizeof(Datum) + sizeof(bool))));
            const std::uint64_t required_after_row = bioseq::common::saturating_add(
                runtime.query_actual_materialized_bytes, tuple_upper_bound);
            if (required_after_row == (std::numeric_limits<std::uint64_t>::max)()) {
                batch_join_raise_materialization_as_cpp(
                    ERRCODE_PROGRAM_LIMIT_EXCEEDED,
                    "BioseqBatchJoin query admission size overflow");
            }
            if (required_after_row > runtime.query_reserved_admission_bytes) {
                const std::uint64_t additional_bytes =
                    required_after_row - runtime.query_reserved_admission_bytes;
                auto additional_permit =
                    bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                        additional_bytes);
                if (!additional_permit.has_value()) {
                    batch_join_raise_materialization_as_cpp(
                        ERRCODE_OUT_OF_MEMORY,
                        "BioseqBatchJoin query child cannot be admitted",
                        fmt::format("query_ordinal={} required_additional_bytes={} "
                                    "whole_tuple_upper_bound={}",
                                    runtime.query_tuples.size(),
                                    additional_bytes,
                                    tuple_upper_bound));
                }
                runtime.query_permits.push_back(std::move(*additional_permit));
                runtime.query_reserved_admission_bytes = required_after_row;
            }
            /* Every following operation may allocate; the complete tuple upper
             * bound is owned before the first retain/detoast/copy. */
            Datum retained_recipe = Datum{0};
            std::vector<batch_join_query_tuple::retained_expanded_attribute>
                retained_expanded_attributes{};
            retained_expanded_attributes.reserve(expanded_source_attributes.size());
            std::uint64_t newly_retained_wrapper_bytes = 0u;
            bioseq::pg_types::seq* flat = nullptr;
            TupleTableSlot* serialization_source =
                row_has_expanded_sequence ? runtime.query_staging_slot : source;
            if (row_has_expanded_sequence) {
                MemoryContext old = MemoryContextSwitchTo(runtime.query_tuple_context);
                try {
                    for (const auto& attribute : expanded_source_attributes) {
                        const acquired_sequence acquired =
                            bioseq::postgres::support::invoke_pg_as_cpp(
                                [attribute_datum = attribute.datum] {
                                    const Datum read_only =
                                        MakeExpandedObjectReadOnly(attribute_datum, false, -1);
                                    return acquire_writable_pg_sequence(read_only);
                                });
                        newly_retained_wrapper_bytes = bioseq::common::saturating_add(
                            newly_retained_wrapper_bytes,
                            static_cast<std::uint64_t>(
                                MemoryContextMemAllocated(acquired.value->object_context,
                                                          /*recurse=*/true)));
                        const Datum retained = pg_sequence_ro_datum(acquired.value);
                        retained_expanded_attributes.push_back({
                            .attno = attribute.attno,
                            .datum = retained,
                            .descriptor = attribute.descriptor,
                        });
                        if (attribute.attno == runtime.query_slot_seq_resno) {
                            retained_recipe = retained;
                        }
                    }
                } catch (...) {
                    MemoryContextSwitchTo(old);
                    throw;
                }
                MemoryContextSwitchTo(old);
                if (newly_retained_wrapper_bytes > retained_recipe_bytes) {
                    batch_join_raise_materialization_as_cpp(
                        ERRCODE_INTERNAL_ERROR,
                        "BioseqBatchJoin retained recipe admission was under-estimated");
                }
            }
            if (!expanded_query) {
                flat = bioseq::postgres::support::invoke_pg_as_cpp([original] {
                    return reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(original));
                });
                if (bioseq::storage::is_locator_stub(flat)) {
                    if (reinterpret_cast<void*>(flat) !=
                        reinterpret_cast<void*>(DatumGetPointer(original))) {
                        pfree(flat);
                    }
                    batch_join_raise_materialization_as_cpp(
                        ERRCODE_FEATURE_NOT_SUPPORTED,
                        "BioseqBatchJoin query child returned an unresolved locator");
                }
                if (flat->alphabet_code != runtime.query_alphabet_code ||
                    flat->storage_unit_byte_width != runtime.query_storage_unit_byte_width) {
                    if (reinterpret_cast<void*>(flat) !=
                        reinterpret_cast<void*>(DatumGetPointer(original))) {
                        pfree(flat);
                    }
                    batch_join_raise_materialization_as_cpp(
                        ERRCODE_DATATYPE_MISMATCH,
                        "BioseqBatchJoin query typmod changed after planning");
                }
                TupleTableSlot* flat_destination =
                    row_has_expanded_sequence ? runtime.query_staging_slot : source;
                flat_destination->tts_values[runtime.query_slot_seq_resno - 1] =
                    PointerGetDatum(flat);
                flat_destination->tts_isnull[runtime.query_slot_seq_resno - 1] = false;
            }
            /* The MinimalTuple and its owning slot are one retained query-row
             * object.  Allocate both in the dedicated query context: execution may
             * enter here with a transient expression context current, and retaining
             * only the tuple there leaves the slot itself dangling before EndScan. */
            MemoryContext old = MemoryContextSwitchTo(runtime.query_tuple_context);
            MinimalTuple copy = nullptr;
            try {
                copy = bioseq::postgres::support::invoke_pg_as_cpp([serialization_source] {
                    return ExecCopySlotMinimalTuple(serialization_source);
                });
                MemoryContextSwitchTo(old);
            } catch (...) {
                MemoryContextSwitchTo(old);
                throw;
            }
            if (flat != nullptr && reinterpret_cast<void*>(flat) !=
                                       reinterpret_cast<void*>(DatumGetPointer(original))) {
                pfree(flat);
                if (!row_has_expanded_sequence) {
                    source->tts_values[runtime.query_slot_seq_resno - 1] = original;
                }
            }
            if (copy == nullptr) {
                batch_join_raise_materialization_as_cpp(
                    ERRCODE_OUT_OF_MEMORY, "BioseqBatchJoin failed to copy a query tuple");
            }
            const std::uint64_t ordinal = runtime.query_tuples.size();
            if (ordinal > static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())) {
                batch_join_raise_materialization_as_cpp(
                    ERRCODE_PROGRAM_LIMIT_EXCEEDED,
                    "BioseqBatchJoin query ordinal exceeds int32 dispatch ABI");
            }
            old = MemoryContextSwitchTo(runtime.query_tuple_context);
            TupleTableSlot* stable_slot = nullptr;
            try {
                stable_slot = bioseq::postgres::support::invoke_pg_as_cpp([source, copy] {
                    TupleTableSlot* result =
                        MakeSingleTupleTableSlot(source->tts_tupleDescriptor, &TTSOpsMinimalTuple);
                    ExecStoreMinimalTuple(copy, result, /*shouldFree=*/false);
                    slot_getallattrs(result);
                    return result;
                });
                for (const auto& attribute : retained_expanded_attributes) {
                    stable_slot->tts_values[attribute.attno - 1] = attribute.datum;
                    stable_slot->tts_isnull[attribute.attno - 1] = false;
                }
                MemoryContextSwitchTo(old);
            } catch (...) {
                MemoryContextSwitchTo(old);
                throw;
            }
            const std::uint64_t retained_vector_bytes = bioseq::common::saturating_mul(
                static_cast<std::uint64_t>(retained_expanded_attributes.capacity()),
                static_cast<std::uint64_t>(
                    sizeof(batch_join_query_tuple::retained_expanded_attribute)));
            const std::uint64_t actual_tuple_bytes = bioseq::common::saturating_add(
                static_cast<std::uint64_t>(copy->t_len),
                bioseq::common::saturating_add(
                    newly_retained_wrapper_bytes,
                    bioseq::common::saturating_add(
                        retained_vector_bytes,
                        static_cast<std::uint64_t>(
                            sizeof(batch_join_query_tuple) +
                            sizeof(bioseq::scheduler::dispatch_join_query_ref) +
                            sizeof(TupleTableSlot) +
                            static_cast<std::size_t>(source->tts_tupleDescriptor->natts) *
                                (sizeof(Datum) + sizeof(bool))))));
            runtime.query_actual_materialized_bytes = bioseq::common::saturating_add(
                runtime.query_actual_materialized_bytes, actual_tuple_bytes);
            if (runtime.query_actual_materialized_bytes > runtime.query_reserved_admission_bytes) {
                batch_join_raise_materialization_as_cpp(
                    ERRCODE_INTERNAL_ERROR,
                    "BioseqBatchJoin whole-tuple admission under-estimated its allocation");
            }
            runtime.query_tuples.push_back(batch_join_query_tuple{
                .tuple = copy,
                .slot = stable_slot,
                .ordinal = ordinal,
                .recipe_datum = retained_recipe,
                .recipe_descriptor = recipe_descriptor,
                .expanded_sequence_attributes = std::move(retained_expanded_attributes),
            });
        }
        if (query_async_owner != nullptr) {
            runtime.buffer.bind_slice_async_owner(query_async_owner);
        }
        runtime.all_query_refs.reserve(runtime.query_tuples.size());
        for (batch_join_query_tuple& query : runtime.query_tuples) {
            runtime.all_query_refs.push_back(bioseq::scheduler::dispatch_join_query_ref{
                .ordinal = query.ordinal,
                .query_slot = query.slot,
            });
        }
        runtime.queries_materialized = true;
    }

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
        auto bigint = [&value](const std::uint64_t input) {
            if (input > static_cast<std::uint64_t>(INT64_MAX)) {
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
            const std::string cigar = bioseq::simd::format_cigar(match.cigar);
            MemoryContext previous = MemoryContextSwitchTo(output_context);
            value = CStringGetTextDatum(cigar.c_str());
            MemoryContextSwitchTo(previous);
        } else {
            is_null = true;
        }
        slot->tts_values[site.resno - 1] = value;
        slot->tts_isnull[site.resno - 1] = is_null;
    }

    struct batch_join_target_source_datum {
        AttrNumber resno = InvalidAttrNumber;
        Datum datum = Datum{0};
    };

    struct batch_join_target_source_restore {
        batch_join_target_source_datum* sources = nullptr;
        std::size_t count = 0u;
    };

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

    void normalize_batch_join_target_sources(TupleTableSlot* slot,
                                             const batch_join_target_source_restore& restore,
                                             MemoryContext output_context) {
        for (std::size_t index = 0u; index < restore.count; ++index) {
            const batch_join_target_source_datum& source = restore.sources[index];
            if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(source.datum))) {
                continue;
            }
            /* The dispatch buffer owns a MinimalTuple copy, which may short-pack an
             * extended sequence Datum.  BSS3's locator decoder requires the ordinary
             * four-byte varlena header, so normalize in the output lifetime before it
             * examines bioseq::pg_types::seq fields. */
            MemoryContext previous = MemoryContextSwitchTo(output_context);
            bioseq::pg_types::seq* flat =
                reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(source.datum));
            MemoryContextSwitchTo(previous);
            if (static_cast<std::size_t>(VARSIZE(flat)) < bioseq::pg_types::seq::header_bytes()) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqBatchJoin target source is truncated"),
                         errdetail("attribute=%d bytes=%zu",
                                   static_cast<int>(source.resno),
                                   static_cast<std::size_t>(VARSIZE(flat)))));
            }
            slot->tts_values[source.resno - 1] = PointerGetDatum(flat);
        }
    }

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
        /* A locator target remains deferred while BatchJoin aligns it.  Once the
         * match has been produced, an enclosing SliceProject may consume that same
         * target through a source-backed recipe, and an upper BatchJoin may bind a
         * sibling sequence as PARAM_EXEC. Promote the output copy only now, into
         * this output's per-tuple context, copy the promoted Datums, then restore
         * the retained locator tuple before another match can reuse it. */
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
            /* A rejected output can be followed by finish_join_target() inside this
             * same executor call. Drop borrowed query/target recipe Datums before that
             * transition resets the dispatch arena. */
            clear_recipe_scan_parent_tuple_references(node);
            return nullptr;
        }
        return node->ss.ps.ps_ProjInfo != nullptr ? ExecProject(node->ss.ps.ps_ProjInfo) : combined;
    }

    Node* bioseq_batch_join_create_custom_scan_state(CustomScan* cscan) {
        /* A PlannedStmt can outlive every executor context (prepared statements,
         * generic plans, EXPLAIN followed by EXECUTE).  Treat the plan as immutable;
         * planner setrefs must already have mapped each registered whole-helper
         * identity to its INDEX_VAR descriptor. */
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

    [[nodiscard]] bool bioseq_batch_join_is_post_join_recipe(CustomScanState const* node) noexcept {
        if (node == nullptr || node->ss.ps.plan == nullptr || !IsA(node->ss.ps.plan, CustomScan))
            return false;
        auto const* cscan = reinterpret_cast<CustomScan const*>(node->ss.ps.plan);
        if (list_length(cscan->custom_plans) != 1 ||
            list_length(cscan->custom_private) != k_bioseq_per_row_join_private_fields)
            return false;
        Node* version = reinterpret_cast<Node*>(linitial(cscan->custom_private));
        return version != nullptr && IsA(version, Integer) &&
               intVal(version) == k_bioseq_per_row_join_plan_version;
    }

    void bioseq_batch_join_begin_custom_scan(CustomScanState* node, EState* estate, int eflags) {
        if (bioseq_batch_join_is_post_join_recipe(node)) {
            bioseq_per_row_join_begin_custom_scan(node, estate, eflags);
            return;
        }
        auto* cscan = reinterpret_cast<CustomScan*>(node->ss.ps.plan);
        if (IsParallelWorker() || cscan == nullptr || cscan->scan.scanrelid != 0 ||
            list_length(cscan->custom_plans) != 2 || cscan->custom_exprs != NIL ||
            node->custom_ps != NIL ||
            list_length(cscan->custom_private) != k_bioseq_batch_join_private_fields) {
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("BioseqBatchJoin executor shape is invalid or parallel")));
        }
        /* custom_scan_tlist deliberately retains original helper/PHV identities
         * as descriptor-only columns, but PostgreSQL setrefs must have removed
         * every helper from the executable plan targetlist.  If that contract ever
         * changes, fail before child initialization instead of silently executing a
         * rendezvous stub (which would fabricate a CPU-looking success). */
        for (TargetEntry* output : postgres_list<TargetEntry>(cscan->scan.plan.targetlist)) {
            if (output != nullptr && output->expr != nullptr &&
                batch_join_expression_has_match_helper(reinterpret_cast<Node*>(output->expr),
                                                       get_cached_stub_oids())) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqBatchJoin executable projection retained a match helper"),
                         errdetail("setrefs did not map every helper identity to an executor-owned "
                                   "scan slot; no child was initialized")));
            }
        }
        auto* state = reinterpret_cast<batch_join_state*>(node);
        state->runtime = nullptr;
        batch_join_runtime* volatile runtime = nullptr;
        bool volatile cleanup_registered = false;
        MemoryContext saved_context = CurrentMemoryContext;
        ErrorData* volatile original_error = nullptr;
        ErrorData* volatile converted_pg_error = nullptr;
        std::exception_ptr cpp_error;
        PG_TRY();
        {
            try {
                runtime = std::construct_at(static_cast<batch_join_runtime*>(
                    MemoryContextAlloc(estate->es_query_cxt, sizeof(batch_join_runtime))));
                state->runtime = runtime;
                runtime->cleanup_callback.func = batch_join_runtime_cleanup;
                runtime->cleanup_callback.arg = state;
                MemoryContextRegisterResetCallback(estate->es_query_cxt,
                                                   &runtime->cleanup_callback);
                cleanup_registered = true;
                auto private_integer = [cscan](const int index) -> int {
                    Node* encoded = reinterpret_cast<Node*>(list_nth(cscan->custom_private, index));
                    if (encoded == nullptr || !IsA(encoded, Integer)) {
                        ereport(ERROR,
                                (errcode(ERRCODE_INTERNAL_ERROR),
                                 errmsg("BioseqBatchJoin private integer ABI is invalid"),
                                 errdetail("field=%d", index)));
                    }
                    return intVal(encoded);
                };
                const int version = private_integer(k_batch_join_version);
                const int token = private_integer(k_batch_join_token);
                const int stable_match_id = validate_finalized_match_owner_descriptor(
                    cscan, k_batch_join_match_owner_descriptor);
                runtime->target_relation_oid =
                    static_cast<Oid>(private_integer(k_batch_join_target_relation_oid));
                runtime->target_storage_seq_attno =
                    static_cast<AttrNumber>(private_integer(k_batch_join_target_storage_seq_attno));
                runtime->target_slot_seq_resno =
                    static_cast<AttrNumber>(private_integer(k_batch_join_target_slot_seq_resno));
                runtime->target_slot_rowno_resno =
                    static_cast<AttrNumber>(private_integer(k_batch_join_target_slot_rowno_resno));
                runtime->query_slot_seq_resno =
                    static_cast<AttrNumber>(private_integer(k_batch_join_query_slot_seq_resno));
                runtime->target_column_count = private_integer(k_batch_join_target_column_count);
                runtime->query_column_count = private_integer(k_batch_join_query_column_count);
                const int raw_alphabet_code = private_integer(k_batch_join_alphabet_code);
                const int raw_storage_width = private_integer(k_batch_join_storage_unit_byte_width);
                const int raw_requirement = private_integer(k_batch_join_index_read_requirement);
                runtime->honest_startup_cost = batch_join_cost_from_node(
                    reinterpret_cast<Node*>(
                        list_nth(cscan->custom_private, k_batch_join_honest_startup_cost)),
                    "honest_startup_cost");
                runtime->honest_total_cost = batch_join_cost_from_node(
                    reinterpret_cast<Node*>(
                        list_nth(cscan->custom_private, k_batch_join_honest_total_cost)),
                    "honest_total_cost");
                runtime->incumbent_cost_floor = batch_join_cost_from_node(
                    reinterpret_cast<Node*>(
                        list_nth(cscan->custom_private, k_batch_join_incumbent_cost_floor)),
                    "incumbent_cost_floor");
                Node* sites_node = reinterpret_cast<Node*>(
                    list_nth(cscan->custom_private, k_batch_join_placeholder_sites));
                /* PostgreSQL's canonical empty List is NIL, i.e. a null pointer.  Keep
                 * shape validity separate from the decoded pointer so an empty helper
                 * set is valid while every non-List node remains fail-closed. */
                const bool sites_shape_valid = sites_node == nullptr || IsA(sites_node, List);
                List* sites = sites_node == nullptr ? NIL : reinterpret_cast<List*>(sites_node);
                FuncExpr* mmsearch = try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(
                    list_nth(cscan->custom_private, k_batch_join_mmsearch_expr)));
                if (version != k_bioseq_batch_join_plan_version || token <= 0 ||
                    !OidIsValid(runtime->target_relation_oid) ||
                    runtime->target_storage_seq_attno <= 0 || runtime->target_slot_seq_resno <= 0 ||
                    runtime->target_slot_rowno_resno <= 0 || runtime->query_slot_seq_resno <= 0 ||
                    runtime->target_column_count <= 0 || runtime->query_column_count <= 0 ||
                    raw_alphabet_code < 0 || raw_alphabet_code > UINT8_MAX ||
                    (raw_storage_width != 1 && raw_storage_width != 2 && raw_storage_width != 4 &&
                     raw_storage_width != 8) ||
                    !bioseq::indexing::index_read_requirement_ordinal_valid(raw_requirement) ||
                    !sites_shape_valid || mmsearch == nullptr) {
                    ereport(ERROR,
                            (errcode(ERRCODE_INTERNAL_ERROR),
                             errmsg("BioseqBatchJoin executor private ABI is invalid")));
                }
                runtime->alphabet_code = static_cast<std::uint8_t>(raw_alphabet_code);
                runtime->storage_unit_byte_width = static_cast<std::uint8_t>(raw_storage_width);
                const int32 expected_typmod =
                    compact_sequence_stdc_type_params{runtime->alphabet_code,
                                                      runtime->storage_unit_byte_width}
                        .encode();
                if (!batch_join_typmod_is_supported(expected_typmod)) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATATYPE_MISMATCH),
                             errmsg("BioseqBatchJoin encoded typmod is unsupported")));
                }
                runtime->index_read_requirement =
                    static_cast<bioseq::indexing::index_read_requirement>(raw_requirement);
                runtime->explain_only = (eflags & EXEC_FLAG_EXPLAIN_ONLY) != 0;

                /* Re-decode the catalog composite before any child initialization.  This
                 * catches catalog drift and validates that the ordinal marker was not altered
                 * between planning and execution. */
                bioseq::simd::minimap2_parameters prevalidated_params = build_params_from_gucs();
                bioseq::indexing::index_read_requirement decoded_requirement =
                    bioseq::indexing::index_read_requirement::allow;
                (void)apply_aligner_options_override(
                    mmsearch, prevalidated_params, decoded_requirement, get_cached_stub_oids());
                finalize_simd_chaining_policy(prevalidated_params);
                if (decoded_requirement != runtime->index_read_requirement) {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg(
                             "BioseqBatchJoin index requirement marker disagrees with options")));
                }
                /* A selected Vulkan BatchJoin is allowed to initialize its children and
                 * reach tuple_dispatch_buffer's production persisted-target preparation path.  That
                 * path opens the PostgreSQL-owned minimizer-sequence generation, validates READY,
                 * revision/ABI/source/device identity, and retains its three resident GPU
                 * buffers.  Missing or incompatible data still fails there before typed
                 * dispatch; the typed processor independently requires that exact GPU
                 * prepared target and has no CPU/SIMD/recompute fallback.  The ordinary
                 * column-scan path remains rejected separately because it has no such
                 * prepared-target owner. */

                TupleDesc scan_desc = node->ss.ss_ScanTupleSlot == nullptr
                                          ? nullptr
                                          : node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
                const std::int64_t child_column_count =
                    static_cast<std::int64_t>(runtime->target_column_count) +
                    static_cast<std::int64_t>(runtime->query_column_count);
                if (scan_desc == nullptr || child_column_count <= 0 ||
                    child_column_count > scan_desc->natts) {
                    ereport(ERROR,
                            (errcode(ERRCODE_INTERNAL_ERROR),
                             errmsg("BioseqBatchJoin scan tuple/private layout is invalid"),
                             errdetail("target_columns=%d query_columns=%d scan_columns=%d "
                                       "placeholder_sites=%d",
                                       runtime->target_column_count,
                                       runtime->query_column_count,
                                       scan_desc == nullptr ? -1 : scan_desc->natts,
                                       list_length(sites))));
                }
                Bitmapset* placeholder_resnos = nullptr;
                int appended_placeholder_count = 0;
                for (List* encoded : postgres_list<List>(sites)) {
                    Node* encoded_node = reinterpret_cast<Node*>(encoded);
                    if (encoded_node == nullptr || !IsA(encoded_node, List) ||
                        list_length(encoded) != k_bioseq_match_helper_site_fields) {
                        ereport(ERROR,
                                (errcode(ERRCODE_INTERNAL_ERROR),
                                 errmsg("BioseqBatchJoin placeholder ABI is invalid")));
                    }
                    for (const int index : {k_match_site_resno,
                                            k_match_site_funcid,
                                            k_match_site_typmod,
                                            k_match_site_collation,
                                            k_match_site_binding_kind,
                                            k_match_site_phid,
                                            k_match_site_stable_id}) {
                        Node* value = reinterpret_cast<Node*>(list_nth(encoded, index));
                        if (value == nullptr || !IsA(value, Integer)) {
                            ereport(ERROR,
                                    (errcode(ERRCODE_INTERNAL_ERROR),
                                     errmsg("BioseqBatchJoin placeholder scalar ABI is invalid")));
                        }
                    }
                    const int raw_resno = intVal(list_nth(encoded, k_match_site_resno));
                    const bool duplicate_resno =
                        raw_resno > 0 && bms_is_member(raw_resno, placeholder_resnos);
                    const bool existing_target_resno =
                        raw_resno > 0 && raw_resno <= runtime->target_column_count;
                    const bool appended_resno =
                        raw_resno > child_column_count && raw_resno <= scan_desc->natts;
                    if ((!existing_target_resno && !appended_resno) ||
                        raw_resno > scan_desc->natts || duplicate_resno) {
                        ereport(
                            ERROR,
                            (errcode(ERRCODE_INTERNAL_ERROR),
                             errmsg("BioseqBatchJoin placeholder resno ABI is invalid"),
                             errdetail("resno=%d child_columns=%lld scan_columns=%d duplicate=%s",
                                       raw_resno,
                                       static_cast<long long>(child_column_count),
                                       scan_desc->natts,
                                       duplicate_resno ? "true" : "false")));
                    }
                    if (appended_resno)
                        ++appended_placeholder_count;
                    placeholder_resnos = bms_add_member(placeholder_resnos, raw_resno);
                    const batch_join_placeholder_site site{
                        .resno = static_cast<AttrNumber>(raw_resno),
                        .funcid = static_cast<Oid>(intVal(list_nth(encoded, k_match_site_funcid))),
                        .typmod = intVal(list_nth(encoded, k_match_site_typmod)),
                        .collation =
                            static_cast<Oid>(intVal(list_nth(encoded, k_match_site_collation))),
                        .binding_kind = static_cast<match_helper_binding_kind>(
                            intVal(list_nth(encoded, k_match_site_binding_kind))),
                        .stable_match_id = intVal(list_nth(encoded, k_match_site_stable_id)),
                    };
                    Form_pg_attribute attribute =
                        scan_desc == nullptr || site.resno <= 0 || site.resno > scan_desc->natts
                            ? nullptr
                            : TupleDescAttr(scan_desc, site.resno - 1);
                    if (attribute == nullptr ||
                        !is_bioseq_projection_funcid(site.funcid, get_cached_stub_oids()) ||
                        attribute->atttypid != get_func_rettype(site.funcid) ||
                        attribute->atttypmod != site.typmod ||
                        attribute->attcollation != site.collation ||
                        (site.binding_kind != match_helper_binding_kind::exact_recipe &&
                         site.binding_kind != match_helper_binding_kind::unique_lineage) ||
                        site.stable_match_id != stable_match_id) {
                        ereport(ERROR,
                                (errcode(ERRCODE_DATATYPE_MISMATCH),
                                 errmsg("BioseqBatchJoin placeholder type/resno ABI is invalid")));
                    }
                    runtime->placeholder_sites.push_back(site);
                }
                if (appended_placeholder_count != scan_desc->natts - child_column_count) {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqBatchJoin appended placeholder layout is invalid"),
                         errdetail(
                             "child_columns=%lld scan_columns=%d appended_sites=%d total_sites=%d",
                             static_cast<long long>(child_column_count),
                             scan_desc->natts,
                             appended_placeholder_count,
                             list_length(sites))));
                }
                bms_free(placeholder_resnos);

                Plan* target_plan = reinterpret_cast<Plan*>(linitial(cscan->custom_plans));
                Plan* query_plan = reinterpret_cast<Plan*>(lsecond(cscan->custom_plans));
                if (target_plan == nullptr || query_plan == nullptr) {
                    ereport(ERROR,
                            (errcode(ERRCODE_INTERNAL_ERROR),
                             errmsg("BioseqBatchJoin child plan ABI is invalid")));
                }
                runtime->query_planner_rows = std::max(0.0, query_plan->plan_rows);
                runtime->query_planner_width = std::max(0, query_plan->plan_width);
                runtime->target_child = ExecInitNode(target_plan, estate, eflags);
                runtime->target_child_initialized = true;
                node->custom_ps = lappend(node->custom_ps, runtime->target_child);
                runtime->query_child = ExecInitNode(query_plan, estate, eflags);
                runtime->query_child_initialized = true;
                node->custom_ps = lappend(node->custom_ps, runtime->query_child);
                auto* target_custom = reinterpret_cast<CustomScan*>(target_plan);
                if (!IsA(target_plan, CustomScan) ||
                    target_custom->methods != bioseq_column_scan_scan_methods()) {
                    ereport(ERROR,
                            (errcode(ERRCODE_INTERNAL_ERROR),
                             errmsg("BioseqBatchJoin target provider is invalid")));
                }
                CustomScanState* target_custom_state = try_cast_pg_node<CustomScanState>(
                    reinterpret_cast<Node*>(runtime->target_child));
                if (target_custom_state == nullptr ||
                    target_custom_state->methods != bioseq_column_scan_execute_methods() ||
                    target_custom_state->ss.ps.plan != target_plan ||
                    target_custom_state->ss.ss_currentRelation == nullptr ||
                    RelationGetRelid(target_custom_state->ss.ss_currentRelation) !=
                        runtime->target_relation_oid) {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqBatchJoin initialized target provider state is invalid")));
                }
                scan_state* target_bioseq_state =
                    scan_state::from_custom_scan_state(target_custom_state);
                bioseq_scan_runtime& target_scan_runtime = bioseq_runtime(target_bioseq_state);
                Snapshot target_snapshot = target_bioseq_state->snapshot;
                if (target_snapshot == nullptr || !IsMVCCSnapshot(target_snapshot) ||
                    target_snapshot->regd_count == 0) {
                    ereport(ERROR,
                            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                             errmsg("BioseqBatchJoin target provider snapshot is not registered "
                                    "MVCC state")));
                }
                RangeTblEntry* target_rte = exec_rt_fetch(target_custom->scan.scanrelid, estate);
                if (target_rte == nullptr || target_rte->rtekind != RTE_RELATION ||
                    target_rte->relid != runtime->target_relation_oid) {
                    ereport(ERROR,
                            (errcode(ERRCODE_INTERNAL_ERROR),
                             errmsg("BioseqBatchJoin target provider relation is invalid")));
                }
                List* locator_tag = nullptr;
                for (Node* raw_tag : postgres_list<Node>(target_custom->custom_private)) {
                    if (raw_tag == nullptr || !IsA(raw_tag, List))
                        continue;
                    List* candidate = reinterpret_cast<List*>(raw_tag);
                    if (list_length(candidate) != 4 ||
                        !IsA(reinterpret_cast<Node*>(linitial(candidate)), Integer) ||
                        intVal(linitial(candidate)) != k_bioseq_locator_passthrough_magic)
                        continue;
                    if (locator_tag != nullptr) {
                        ereport(ERROR,
                                (errcode(ERRCODE_INTERNAL_ERROR),
                                 errmsg("BioseqBatchJoin target has duplicate locator ownership")));
                    }
                    locator_tag = candidate;
                }
                if (locator_tag == nullptr || list_length(locator_tag) != 4 ||
                    intVal(list_nth(locator_tag, 0)) != k_bioseq_locator_passthrough_magic ||
                    intVal(list_nth(locator_tag, 1)) != k_bioseq_batch_join_plan_version ||
                    intVal(list_nth(locator_tag, 2)) != token ||
                    intVal(list_nth(locator_tag, 3)) != runtime->target_slot_rowno_resno) {
                    ereport(ERROR,
                            (errcode(ERRCODE_INTERNAL_ERROR),
                             errmsg("BioseqBatchJoin target locator token mismatch")));
                }
                /* Validate against the post-setrefs Plan targetlists.  They are stable
                 * planner ABI; ps_ResultTupleSlot is not, because projection-free and
                 * pass-through PlanStates may return a scan/child slot instead.  The
                 * actual slots are validated independently at each ExecProcNode edge. */
                if (runtime->target_column_count != list_length(target_plan->targetlist) ||
                    runtime->query_column_count != list_length(query_plan->targetlist) ||
                    runtime->target_slot_seq_resno <= 0 ||
                    runtime->target_slot_seq_resno > runtime->target_column_count ||
                    runtime->target_slot_rowno_resno <= 0 ||
                    runtime->target_slot_rowno_resno > runtime->target_column_count ||
                    runtime->query_slot_seq_resno <= 0 ||
                    runtime->query_slot_seq_resno > runtime->query_column_count) {
                    ereport(ERROR,
                            (errcode(ERRCODE_INTERNAL_ERROR),
                             errmsg("BioseqBatchJoin post-setrefs child targetlists are invalid"),
                             errdetail("target_private_columns=%d target_plan_columns=%d "
                                       "query_private_columns=%d query_plan_columns=%d "
                                       "target_sequence_resno=%d target_rowno_resno=%d "
                                       "query_sequence_resno=%d",
                                       runtime->target_column_count,
                                       list_length(target_plan->targetlist),
                                       runtime->query_column_count,
                                       list_length(query_plan->targetlist),
                                       runtime->target_slot_seq_resno,
                                       runtime->target_slot_rowno_resno,
                                       runtime->query_slot_seq_resno)));
                }
                TargetEntry* target_seq_entry = reinterpret_cast<TargetEntry*>(
                    list_nth(target_plan->targetlist, runtime->target_slot_seq_resno - 1));
                TargetEntry* target_rowno_entry = reinterpret_cast<TargetEntry*>(
                    list_nth(target_plan->targetlist, runtime->target_slot_rowno_resno - 1));
                TargetEntry* query_seq_entry = reinterpret_cast<TargetEntry*>(
                    list_nth(query_plan->targetlist, runtime->query_slot_seq_resno - 1));
                const int32 expected_sequence_typmod =
                    compact_sequence_stdc_type_params{runtime->alphabet_code,
                                                      runtime->storage_unit_byte_width}
                        .encode();
                const int32 query_sequence_typmod =
                    query_seq_entry == nullptr
                        ? -1
                        : exprTypmod(reinterpret_cast<Node*>(query_seq_entry->expr));
                const bool query_typmod_supported =
                    batch_join_typmod_is_supported(query_sequence_typmod);
                const auto query_sequence_type_params =
                    query_typmod_supported
                        ? compact_sequence_stdc_type_params::decode(query_sequence_typmod)
                        : compact_sequence_stdc_type_params{};
                const Oid sequence_type = get_cached_stub_oids().bioseq_seq_type_oid;
                if (target_seq_entry == nullptr || target_rowno_entry == nullptr ||
                    query_seq_entry == nullptr ||
                    target_seq_entry->resno != runtime->target_slot_seq_resno ||
                    target_rowno_entry->resno != runtime->target_slot_rowno_resno ||
                    query_seq_entry->resno != runtime->query_slot_seq_resno ||
                    !target_rowno_entry->resjunk ||
                    exprType(reinterpret_cast<Node*>(target_seq_entry->expr)) !=
                        get_cached_stub_oids().bioseq_seq_type_oid ||
                    exprType(reinterpret_cast<Node*>(query_seq_entry->expr)) !=
                        get_cached_stub_oids().bioseq_seq_type_oid ||
                    exprTypmod(reinterpret_cast<Node*>(target_seq_entry->expr)) !=
                        expected_sequence_typmod ||
                    !query_typmod_supported ||
                    query_sequence_type_params.alphabet_code != runtime->alphabet_code ||
                    exprCollation(reinterpret_cast<Node*>(target_seq_entry->expr)) !=
                        exprCollation(reinterpret_cast<Node*>(query_seq_entry->expr)) ||
                    exprCollation(reinterpret_cast<Node*>(target_seq_entry->expr)) !=
                        get_typcollation(sequence_type) ||
                    exprType(reinterpret_cast<Node*>(target_rowno_entry->expr)) != INT8OID ||
                    exprTypmod(reinterpret_cast<Node*>(target_rowno_entry->expr)) != -1 ||
                    OidIsValid(exprCollation(reinterpret_cast<Node*>(target_rowno_entry->expr)))) {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_DATATYPE_MISMATCH),
                         errmsg("BioseqBatchJoin post-setrefs child targetlist types are invalid"),
                         errdetail(
                             "target_sequence_type=%u target_sequence_typmod=%d "
                             "target_sequence_collation=%u query_sequence_type=%u "
                             "query_sequence_typmod=%d query_sequence_collation=%u rowno_type=%u "
                             "rowno_typmod=%d rowno_collation=%u rowno_resjunk=%s",
                             target_seq_entry == nullptr
                                 ? InvalidOid
                                 : exprType(reinterpret_cast<Node*>(target_seq_entry->expr)),
                             target_seq_entry == nullptr
                                 ? -1
                                 : exprTypmod(reinterpret_cast<Node*>(target_seq_entry->expr)),
                             target_seq_entry == nullptr
                                 ? InvalidOid
                                 : exprCollation(reinterpret_cast<Node*>(target_seq_entry->expr)),
                             query_seq_entry == nullptr
                                 ? InvalidOid
                                 : exprType(reinterpret_cast<Node*>(query_seq_entry->expr)),
                             query_seq_entry == nullptr
                                 ? -1
                                 : exprTypmod(reinterpret_cast<Node*>(query_seq_entry->expr)),
                             query_seq_entry == nullptr
                                 ? InvalidOid
                                 : exprCollation(reinterpret_cast<Node*>(query_seq_entry->expr)),
                             target_rowno_entry == nullptr
                                 ? InvalidOid
                                 : exprType(reinterpret_cast<Node*>(target_rowno_entry->expr)),
                             target_rowno_entry == nullptr
                                 ? -1
                                 : exprTypmod(reinterpret_cast<Node*>(target_rowno_entry->expr)),
                             target_rowno_entry == nullptr
                                 ? InvalidOid
                                 : exprCollation(reinterpret_cast<Node*>(target_rowno_entry->expr)),
                             target_rowno_entry != nullptr && target_rowno_entry->resjunk
                                 ? "true"
                                 : "false")));
                }
                runtime->query_alphabet_code = query_sequence_type_params.alphabet_code;
                runtime->query_storage_unit_byte_width =
                    query_sequence_type_params.storage_unit_byte_width;

                bioseq::simd::minimap2_parameters params = build_params_from_gucs();
                bioseq::indexing::index_read_requirement requirement =
                    bioseq::indexing::index_read_requirement::allow;
                (void)apply_aligner_options_override(
                    mmsearch, params, requirement, get_cached_stub_oids());
                finalize_simd_chaining_policy(params);
                params.alphabet_code = runtime->alphabet_code;
                if (requirement != runtime->index_read_requirement) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("BioseqBatchJoin requirement changed after prevalidation")));
                }
                /* Plan setrefs has deliberately replaced every match-result helper
                 * with an INDEX_VAR into the combined scan tuple.  Derive expensive
                 * stage requirements from the validated private placeholder kinds,
                 * not by re-walking an executor targetlist in which helpers no longer
                 * exist. */
                params.do_cigar = false;
                params.compute_divergence = false;
                params.compute_mapq = false;
                params.compute_edit_distance = false;
                const bioseq_stub_oids& batch_oids = get_cached_stub_oids();
                for (const batch_join_placeholder_site& site : runtime->placeholder_sites) {
                    params.do_cigar = params.do_cigar || site.funcid == batch_oids.cigar_oid ||
                                      site.funcid == batch_oids.has_cigar_oid ||
                                      site.funcid == batch_oids.edit_oid;
                    params.compute_edit_distance =
                        params.compute_edit_distance || site.funcid == batch_oids.edit_oid;
                    params.compute_divergence =
                        params.compute_divergence || site.funcid == batch_oids.divergence_oid;
                    params.compute_mapq = params.compute_mapq || site.funcid == batch_oids.mapq_oid;
                }

                /* Reuse the target provider's executor-owned Relation. Its lock lives
                 * through ExecEndNode; this BatchJoin neither reopens nor closes it. */
                Relation target_relation = target_custom_state->ss.ss_currentRelation;
                if (target_relation == nullptr) {
                    ereport(ERROR,
                            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                             errmsg("BioseqBatchJoin target executor relation is unavailable")));
                }
                if (target_relation->rd_tableam != column_table_access_method()) {
                    ereport(ERROR,
                            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                             errmsg("BioseqBatchJoin target relation changed table AM")));
                }
                TupleDesc relation_desc = RelationGetDescr(target_relation);
                Form_pg_attribute storage_sequence_attribute =
                    runtime->target_storage_seq_attno <= 0 ||
                            runtime->target_storage_seq_attno > relation_desc->natts
                        ? nullptr
                        : TupleDescAttr(relation_desc, runtime->target_storage_seq_attno - 1);
                if (storage_sequence_attribute == nullptr ||
                    storage_sequence_attribute->attisdropped ||
                    storage_sequence_attribute->atttypid !=
                        get_cached_stub_oids().bioseq_seq_type_oid ||
                    storage_sequence_attribute->atttypmod != expected_sequence_typmod ||
                    storage_sequence_attribute->attcollation != get_typcollation(sequence_type)) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATATYPE_MISMATCH),
                             errmsg("BioseqBatchJoin storage-column ABI changed after planning")));
                }
                char* target_column_name = get_attname(runtime->target_relation_oid,
                                                       runtime->target_storage_seq_attno,
                                                       /*missing_ok=*/false);
                /* The target child may have replaced the executor snapshot while
                 * flushing its columnar write state. Borrow that exact registered
                 * snapshot. batch_join_release_executor_resources_checked drains the
                 * buffer (and its fragment readers) before ExecEndNode(target_child)
                 * releases the child's registration. */
                runtime->aligner_params = params;
                runtime->aligner_params_captured = true;
                runtime->pipeline_desc = describe_pipeline_for_pg_alphabet(
                    params, runtime->alphabet_code, query_sketch_plan_route_for(requirement));
                runtime->buffer.configure_params(params,
                                                 requirement,
                                                 bioseq::settings::index::sequence_read::enabled,
                                                 runtime->target_relation_oid,
                                                 target_relation->rd_locator.dbOid,
                                                 target_relation->rd_locator.spcOid,
                                                 target_relation->rd_locator.relNumber,
                                                 target_relation,
                                                 target_snapshot,
                                                 estate->es_query_cxt);
                runtime->buffer.bind_sequence_source_provider(
                    target_scan_runtime.lazy_source_provider);
                runtime->buffer.configure_mmsearch_join(runtime->target_relation_oid,
                                                        runtime->target_storage_seq_attno,
                                                        runtime->target_slot_seq_resno,
                                                        runtime->target_slot_rowno_resno,
                                                        runtime->query_slot_seq_resno,
                                                        std::string{target_column_name},
                                                        runtime->alphabet_code,
                                                        runtime->storage_unit_byte_width,
                                                        runtime->query_alphabet_code,
                                                        runtime->query_storage_unit_byte_width);
                pfree(target_column_name);

                runtime->query_tuple_context = AllocSetContextCreate(
                    estate->es_query_cxt, "BioseqBatchJoin query tuples", ALLOCSET_DEFAULT_SIZES);
                const int plan_node_id = cscan->scan.plan.plan_node_id;
                if (plan_node_id < 0) {
                    ereport(ERROR,
                            (errcode(ERRCODE_INTERNAL_ERROR),
                             errmsg("BioseqBatchJoin plan node id is invalid")));
                }
                runtime->alignment_registration =
                    bioseq::postgres::table::alignment_execution::bind_alignment_node(
                        estate, static_cast<std::uint32_t>(plan_node_id));
                runtime->buffer.bind_persisted_gpu_operation_stats_sink(
                    *bioseq::postgres::table::alignment_execution::
                        borrow_persisted_gpu_operation_accumulator(
                            runtime->alignment_registration));
            } catch (bioseq::postgres::support::postgres_error& error) {
                converted_pg_error = error.release_error_data();
            } catch (...) {
                cpp_error = std::current_exception();
            }
        }
        PG_CATCH();
        {
            MemoryContextSwitchTo(saved_context);
            original_error = CopyErrorData();
            FlushErrorState();
        }
        PG_END_TRY();
        batch_join_runtime* stable_runtime = runtime;
        const bool stable_cleanup_registered = cleanup_registered;
        auto abandon = [node, state, stable_runtime, stable_cleanup_registered]() noexcept {
            if (stable_runtime == nullptr)
                return;
            batch_join_abandon_executor_resources_after_error(node, *stable_runtime);
            if (!stable_cleanup_registered) {
                state->runtime = nullptr;
                std::destroy_at(stable_runtime);
            }
        };
        if (original_error != nullptr) {
            abandon();
            ErrorData* exact_error = original_error;
            ReThrowError(exact_error);
        }
        if (converted_pg_error != nullptr) {
            abandon();
            ErrorData* exact_error = converted_pg_error;
            ReThrowError(exact_error);
        }
        if (cpp_error != nullptr) {
            abandon();
            std::array<char, 512> detail{"unknown C++ exception"};
            try {
                std::rethrow_exception(cpp_error);
            } catch (const std::exception& error) {
                strlcpy(detail.data(),
                        error.what() == nullptr ? "std::exception without detail" : error.what(),
                        detail.size());
            } catch (...) {
            }
            cpp_error = nullptr;
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin Begin raised a C++ exception"),
                     errdetail_internal("%s", detail.data())));
        }
    }

    TupleTableSlot* bioseq_batch_join_exec_custom_scan_impl(CustomScanState* node) {
        batch_join_runtime* runtime = try_get_batch_join_runtime(node);
        if (runtime == nullptr) {
            ereport(
                ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR), errmsg("BioseqBatchJoin runtime is missing")));
        }
        batch_join_clear_parent_tuple_references(node, *runtime);
        bioseq::postgres::table::alignment_execution::ensure_alignment_node_registered(
            runtime->alignment_registration);
        batch_join_materialize_queries(*runtime);

        for (;;) {
            auto output = runtime->buffer.read_next_output();
            if (output.has_value()) {
                TupleTableSlot* projected = batch_join_project_output(node, *runtime, *output);
                if (projected != nullptr)
                    return projected;
                continue;
            }
            if (runtime->target_active &&
                runtime->next_query_batch < runtime->all_query_refs.size()) {
                const std::size_t batch_end =
                    std::min(runtime->all_query_refs.size(),
                             runtime->next_query_batch + k_bioseq_runtime_buffer_reserve);
                const std::span<const bioseq::scheduler::dispatch_join_query_ref> batch{
                    runtime->all_query_refs.data() + runtime->next_query_batch,
                    batch_end - runtime->next_query_batch};
                runtime->buffer.set_join_query_batch(batch);
                if (runtime->buffer.run_join_query_batch_and_build_cursors()) {
                    const auto delta = runtime->buffer.execution_stats();
                    runtime->aggregate_stats.merge(delta);
                    bioseq::postgres::table::alignment_execution::merge_alignment_batch(
                        runtime->alignment_registration, delta);
                }
                runtime->next_query_batch = batch_end;
                ++runtime->query_batch_count;
                continue;
            }
            if (runtime->target_active) {
                runtime->buffer.finish_join_target();
                runtime->target_active = false;
                runtime->current_target_slot = nullptr;
                runtime->next_query_batch = 0;
            }
            if (runtime->target_eof) {
                bioseq::postgres::table::alignment_execution::mark_alignment_pass_fully_drained(
                    runtime->alignment_registration);
                runtime->is_current_pass_fully_drained = true;
                return ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
            }
            TupleTableSlot* target = ExecProcNode(runtime->target_child);
            if (TupIsNull(target)) {
                runtime->target_eof = true;
                continue;
            }
            batch_join_validate_returned_child_slot(target,
                                                    runtime->target_column_count,
                                                    runtime->target_slot_seq_resno,
                                                    runtime->target_slot_rowno_resno,
                                                    runtime->alphabet_code,
                                                    runtime->storage_unit_byte_width,
                                                    "target");
            slot_getallattrs(target);
            if (target->tts_isnull[runtime->target_slot_seq_resno - 1]) {
                /* Strict INNER predicate: do not hand a NULL target to dispatch. */
                continue;
            }
            runtime->current_target_slot = target;
            runtime->buffer.begin_join_target(target);
            runtime->buffer.prepare_join_target_queries(
                std::span<const bioseq::scheduler::dispatch_join_query_ref>{
                    runtime->all_query_refs});
            runtime->target_active = true;
            ++runtime->distinct_targets;
        }
    }

    TupleTableSlot* bioseq_batch_join_exec_custom_scan(CustomScanState* node) {
        if (bioseq_batch_join_is_post_join_recipe(node)) {
            return bioseq_per_row_join_exec_custom_scan(node);
        }
        TupleTableSlot* result = nullptr;
        MemoryContext saved_context = CurrentMemoryContext;
        ErrorData* volatile original_error = nullptr;
        ErrorData* volatile converted_pg_error = nullptr;
        std::exception_ptr cpp_error;
        PG_TRY();
        {
            try {
                result = bioseq_batch_join_exec_custom_scan_impl(node);
            } catch (bioseq::postgres::support::postgres_error& error) {
                converted_pg_error = error.release_error_data();
            } catch (...) {
                cpp_error = std::current_exception();
            }
        }
        PG_CATCH();
        {
            MemoryContextSwitchTo(saved_context);
            original_error = CopyErrorData();
            FlushErrorState();
        }
        PG_END_TRY();
        if (original_error != nullptr) {
            if (batch_join_runtime* runtime = try_get_batch_join_runtime(node);
                runtime != nullptr) {
                batch_join_abandon_executor_resources_after_error(node, *runtime);
            }
            ErrorData* exact_error = original_error;
            ReThrowError(exact_error);
        }
        if (converted_pg_error != nullptr) {
            if (batch_join_runtime* runtime = try_get_batch_join_runtime(node);
                runtime != nullptr) {
                batch_join_abandon_executor_resources_after_error(node, *runtime);
            }
            ErrorData* exact_error = converted_pg_error;
            ReThrowError(exact_error);
        }
        if (cpp_error != nullptr) {
            if (batch_join_runtime* runtime = try_get_batch_join_runtime(node);
                runtime != nullptr) {
                batch_join_abandon_executor_resources_after_error(node, *runtime);
            }
            std::array<char, 512> detail{"unknown C++ exception"};
            try {
                std::rethrow_exception(cpp_error);
            } catch (const std::exception& error) {
                strlcpy(detail.data(),
                        error.what() == nullptr ? "std::exception without detail" : error.what(),
                        detail.size());
            } catch (...) {
            }
            cpp_error = nullptr;
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin Exec raised a C++ exception"),
                     errdetail_internal("%s", detail.data())));
        }
        return result;
    }

    void bioseq_batch_join_end_custom_scan(CustomScanState* node) {
        if (bioseq_batch_join_is_post_join_recipe(node)) {
            bioseq_per_row_join_end_custom_scan(node);
            return;
        }
        batch_join_runtime* runtime = try_get_batch_join_runtime(node);
        if (runtime == nullptr || runtime->cleanup_done) {
            return;
        }
        ErrorData* exact_error = nullptr;
        bool cpp_error = false;
        bool unknown_cpp_error = false;
        std::array<char, 512> detail{"std::exception without detail"};
        try {
            batch_join_release_executor_resources_checked(
                node, *runtime, /*delete_query_context=*/false);
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
        if (exact_error != nullptr || cpp_error || unknown_cpp_error) {
            batch_join_abandon_executor_resources_after_error(node, *runtime);
        }
        if (exact_error != nullptr) {
            ReThrowError(exact_error);
        }
        if (cpp_error) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin checked End raised a C++ exception"),
                     errdetail_internal("%s", detail.data())));
        }
        if (unknown_cpp_error) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin checked End raised an unknown C++ exception")));
        }
    }

    void bioseq_batch_join_rescan_custom_scan(CustomScanState* node) {
        if (bioseq_batch_join_is_post_join_recipe(node)) {
            bioseq_per_row_join_rescan_custom_scan(node);
            return;
        }
        batch_join_runtime* runtime = try_get_batch_join_runtime(node);
        if (runtime == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin rescan runtime is missing")));
        }
        MemoryContext saved_context = CurrentMemoryContext;
        ErrorData* volatile original_error = nullptr;
        ErrorData* volatile converted_pg_error = nullptr;
        std::exception_ptr cpp_error;
        PG_TRY();
        {
            try {
                batch_join_clear_parent_tuple_references(node, *runtime);
                bioseq::postgres::table::alignment_execution::record_alignment_rescan(
                    runtime->alignment_registration);
                if (runtime->target_active) {
                    runtime->buffer.finish_join_target();
                }
                runtime->buffer.reset_join_runtime_for_rescan();
                runtime->buffer.release_join_slice_async_owner_after_drain();
                batch_join_clear_query_tuples(*runtime, /*drop_slots=*/true);
                runtime->target_active = false;
                runtime->target_eof = false;
                runtime->is_current_pass_fully_drained = false;
                runtime->current_target_slot = nullptr;
                runtime->next_query_batch = 0;
                runtime->active_query_batch_end = 0;
                ExecReScan(runtime->target_child);
                ExecReScan(runtime->query_child);
            } catch (bioseq::postgres::support::postgres_error& error) {
                converted_pg_error = error.release_error_data();
            } catch (...) {
                cpp_error = std::current_exception();
            }
        }
        PG_CATCH();
        {
            MemoryContextSwitchTo(saved_context);
            original_error = CopyErrorData();
            FlushErrorState();
        }
        PG_END_TRY();
        if (original_error != nullptr) {
            batch_join_abandon_executor_resources_after_error(node, *runtime);
            ErrorData* exact_error = original_error;
            ReThrowError(exact_error);
        }
        if (converted_pg_error != nullptr) {
            batch_join_abandon_executor_resources_after_error(node, *runtime);
            ErrorData* exact_error = converted_pg_error;
            ReThrowError(exact_error);
        }
        if (cpp_error != nullptr) {
            batch_join_abandon_executor_resources_after_error(node, *runtime);
            std::array<char, 512> detail{"unknown C++ exception"};
            try {
                std::rethrow_exception(cpp_error);
            } catch (const std::exception& error) {
                strlcpy(detail.data(),
                        error.what() == nullptr ? "std::exception without detail" : error.what(),
                        detail.size());
            } catch (...) {
            }
            cpp_error = nullptr;
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin ReScan raised a C++ exception"),
                     errdetail_internal("%s", detail.data())));
        }
    }

    void bioseq_batch_join_explain_custom_scan(CustomScanState* node,
                                               List* ancestors,
                                               ExplainState* es) {
        if (bioseq_batch_join_is_post_join_recipe(node)) {
            bioseq_per_row_join_explain_custom_scan(node, ancestors, es);
            return;
        }
        (void)ancestors;
        batch_join_runtime* runtime = try_get_batch_join_runtime(node);
        ExplainPropertyInteger("Bioseq Query Batch Capacity",
                               nullptr,
                               static_cast<std::int64_t>(k_bioseq_runtime_buffer_reserve),
                               es);
        ExplainPropertyText("Bioseq Residual Mmsearch Qual", "absent", es);
        ExplainPropertyInteger("Bioseq Batch ABI Version",
                               nullptr,
                               static_cast<std::int64_t>(k_bioseq_batch_join_plan_version),
                               es);
        if (runtime == nullptr) {
            return;
        }
        ExplainPropertyFloat(
            "Bioseq Honest Startup Cost", nullptr, runtime->honest_startup_cost, 2, es);
        ExplainPropertyFloat(
            "Bioseq Honest Total Cost", nullptr, runtime->honest_total_cost, 2, es);
        ExplainPropertyFloat(
            "Bioseq Incumbent Cost Floor", nullptr, runtime->incumbent_cost_floor, 2, es);
        ExplainPropertyInteger("Bioseq Match Placeholder Sites",
                               nullptr,
                               static_cast<std::int64_t>(runtime->placeholder_sites.size()),
                               es);
        ExplainPropertyText(
            "Bioseq Index Read Requirement",
            bioseq::indexing::index_read_requirement_name(runtime->index_read_requirement).data(),
            es);
        ExplainPropertyBool("Bioseq Requirement Executable", true, es);
        ExplainPropertyText(
            "Bioseq CPU Prepared Target",
            runtime->index_read_requirement ==
                    bioseq::indexing::index_read_requirement::require_absent
                ? "disabled: ExactSimd target-sketch backend unavailable"
            : runtime->index_read_requirement ==
                    bioseq::indexing::index_read_requirement::require_compatible_persisted
                ? "grouped-SoA direct reader retained across query waves"
                : "persisted-only query-hash union; one target index per parameter group",
            es);
        ExplainPropertyFloat(
            "Bioseq Query Planner Rows", nullptr, runtime->query_planner_rows, 0, es);
        ExplainPropertyInteger("Bioseq Query Planner Width",
                               "bytes",
                               static_cast<std::int64_t>(runtime->query_planner_width),
                               es);
        ExplainPropertyInteger(
            "Bioseq Query Planner Admission",
            "bytes",
            static_cast<std::int64_t>(std::min<std::uint64_t>(
                runtime->query_planner_admission_bytes, static_cast<std::uint64_t>(INT64_MAX))),
            es);
        ExplainPropertyInteger(
            "Bioseq Query Actual Materialization",
            "bytes",
            static_cast<std::int64_t>(std::min<std::uint64_t>(
                runtime->query_actual_materialized_bytes, static_cast<std::uint64_t>(INT64_MAX))),
            es);
        ExplainPropertyInteger("Bioseq Distinct Targets",
                               nullptr,
                               static_cast<std::int64_t>(runtime->distinct_targets),
                               es);
        ExplainPropertyInteger("Bioseq Query Batches",
                               nullptr,
                               static_cast<std::int64_t>(runtime->query_batch_count),
                               es);
        explain_pipeline_execution_stats(
            runtime->aggregate_stats,
            es,
            runtime->aligner_params_captured ? &runtime->aligner_params : nullptr,
            runtime->pipeline_desc.has_value() ? &*runtime->pipeline_desc : nullptr,
            query_sketch_plan_route_for(runtime->index_read_requirement),
            /*highway_dispatch_already_rendered=*/false);
    }

    /** Returns the stable PostgreSQL scan callbacks for base Bioseq scans. */
    [[nodiscard]] const CustomScanMethods* bioseq_column_scan_scan_methods() noexcept {
        static const CustomScanMethods methods{
            .CustomName = "BioseqScan",
            .CreateCustomScanState = bioseq_column_scan_create_custom_scan_state,
        };
        return &methods;
    }

    /** Returns the stable PostgreSQL scan callbacks for batched joins. */
    [[nodiscard]] const CustomScanMethods* bioseq_batch_join_scan_methods() noexcept {
        static const CustomScanMethods methods{
            .CustomName = "BioseqBatchJoin",
            .CreateCustomScanState = bioseq_batch_join_create_custom_scan_state,
        };
        return &methods;
    }

    /** Returns the stable PostgreSQL executor callbacks for batched joins. */
    [[nodiscard]] const CustomExecMethods* bioseq_batch_join_execute_methods() noexcept {
        static const CustomExecMethods methods{
            .CustomName = "BioseqBatchJoin",
            .BeginCustomScan = bioseq_batch_join_begin_custom_scan,
            .ExecCustomScan = bioseq_batch_join_exec_custom_scan,
            .EndCustomScan = bioseq_batch_join_end_custom_scan,
            .ReScanCustomScan = bioseq_batch_join_rescan_custom_scan,
            .ExplainCustomScan = bioseq_batch_join_explain_custom_scan,
        };
        return &methods;
    }

} // export custom-scan partition declarations

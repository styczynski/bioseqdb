/**
 * @file
 * @brief Buffer query rows for a sequence join within the available memory budget.
 */
module;
#include <new>
#include <fmt/format.h>
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

export module bioseq.postgres.table:custom_scan_batch_join_execution_query_materialization;

import std;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import bioseq.postgres.scheduler.dispatch_buffer.api;
import bioseq.postgres.storage.manager;
import bioseq.postgres.support;
import bioseq.postgres.types.compact_sequence;

import :custom_scan_batch_join_execution_runtime_state_contracts;
import :custom_scan_oid_expression_support_oid_catalog_core_contracts;
import :custom_scan_batch_join_execution_resource_lifecycle_contracts;
import :custom_scan_batch_join_execution_query_materialization_contracts;

using namespace bioseq::postgres::support;

export {
    /**
     * @brief Read the query child once and save its non-null query rows.
     *
     * Reserve memory before copying each row. Keep expanded sequence wrappers
     * and their sources alive without flattening them, so every target can reuse
     * the same queries. The reservations last until the query context is cleared.
     * @throws PostgreSQL ERROR for invalid row types or  insufficient memory.
     */
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
        /* Reserve the planner's estimated row storage before reading the child.
         * Increase the reservation before copying a row if the eitimate is low */
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
        /* Reserve scratch space for the staging slot and attribute descriptions
         * even, when the planner expects no rows. */
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
                    bioseq::postgres::support::tuple_descriptor_attribute(source->tts_tupleDescriptor, index)->atttypid != sequence_type) {
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
            /* Add space for the full sequence even if the tuple estimate already
             * includes a TOAST pointer. This safe upper bound avoids fetching the
             * value before memory is reserved. */
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
            /* Reserve  the full row size before saving sequence wrappers or copying values. */
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
            /* Allocate both the saved tuple and its slot in the query context.
             * The caller may be using a temporary expression context that is reset
             * before the join has finished with these rows. */
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

} // export custom-scan partition declarations

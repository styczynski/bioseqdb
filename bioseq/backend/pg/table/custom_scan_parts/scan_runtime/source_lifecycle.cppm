/**
 * @file
 * @brief Owns SliceProject and mmsearch source promotion and restoration.
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

export module bioseq.postgres.table:custom_scan_scan_runtime_source_lifecycle;

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

using namespace bioseq::postgres::support;

export {
    void promote_slice_project_sources(scan_state* state,
                                       TupleTableSlot* slot,
                                       MemoryContext result_context,
                                       const bool allow_locator_passthrough,
                                       const slice_project_source_slot_layout layout,
                                       TupleDesc expected_descriptor) {
        bioseq_scan_runtime& runtime = bioseq_runtime(state);
        if (!runtime.lazy_source_projection ||
            (runtime.locator_passthrough && !allow_locator_passthrough))
            return;
        if (runtime.lazy_source_provider == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("BioseqSliceProject source provider is not initialized")));
        }
        if (slot == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("BioseqSliceProject source slot is unavailable")));
        }
        slot_getallattrs(slot);
        const Oid seq_oid = get_cached_stub_oids().bioseq_seq_type_oid;
        if (result_context == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("BioseqSliceProject result context is unavailable")));
        }
        TupleDesc actual_descriptor = slot->tts_tupleDescriptor;
        if (actual_descriptor == nullptr || expected_descriptor == nullptr ||
            (actual_descriptor != expected_descriptor &&
             !equalTupleDescs(actual_descriptor, expected_descriptor))) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("BioseqSliceProject source slot layout changed"),
                     errdetail("layout=%s",
                               layout == slice_project_source_slot_layout::physical_relation
                                   ? "physical relation"
                                   : "projected child")));
        }
        const std::size_t required_entries = static_cast<std::size_t>(actual_descriptor->natts + 1);
        if ((layout == slice_project_source_slot_layout::physical_relation &&
             (runtime.lazy_source_physical_producer_ids.size() != required_entries ||
              runtime.lazy_source_physical_bound.size() != required_entries)) ||
            (layout == slice_project_source_slot_layout::projected_child &&
             runtime.lazy_source_projected_bindings.size() != required_entries)) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject source binding layout changed")));
        }
        for (int index = 0; index < slot->tts_tupleDescriptor->natts; ++index) {
            const AttrNumber resno = static_cast<AttrNumber>(index + 1);
            std::uint32_t producer_id = 0u;
            AttrNumber column_attno = InvalidAttrNumber;
            bool bound = false;
            if (layout == slice_project_source_slot_layout::physical_relation) {
                bound = runtime.lazy_source_physical_bound[resno] != 0u;
                producer_id = runtime.lazy_source_physical_producer_ids[resno];
                column_attno = resno;
            } else {
                auto const& binding = runtime.lazy_source_projected_bindings[resno];
                bound = binding.physical_attno != InvalidAttrNumber;
                producer_id = binding.producer_id;
                column_attno = binding.physical_attno;
            }
            if (!bound)
                continue;
            if (TupleDescAttr(actual_descriptor, index)->atttypid != seq_oid) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATATYPE_MISMATCH),
                         errmsg("BioseqSliceProject bound source is not a sequence"),
                         errdetail("layout_attribute=%d physical_attribute=%d",
                                   static_cast<int>(resno),
                                   static_cast<int>(column_attno))));
            }
            if (slot->tts_isnull[index])
                continue;
            Datum source = slot->tts_values[index];
            if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(source))) {
                if (producer_id != 0u && runtime.lazy_trace_attachment != nullptr &&
                    runtime.lazy_trace_attachment->runtime != nullptr &&
                    runtime.lazy_trace_attachment->runtime->collection_enabled &&
                    !attach_slice_trace_provenance(
                        source, runtime.lazy_trace_attachment, producer_id)) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("BioseqSliceProject expanded source lost provenance")));
                }
                continue;
            }
            auto* value = reinterpret_cast<const bioseq::pg_types::seq*>(DatumGetPointer(source));
            if (!bioseq::storage::is_locator_stub(value)) {
                /* A small inline sequence has no EOH metadata field on which to keep
                 * the planned producer identity.  Under ANALYZE only, adopt that flat
                 * value once into the row context and attach observation metadata before
                 * the lazy expression runs.  The backing copy is a real source-adoption
                 * allocation (and remains visible to the allocator probe); it is not a
                 * SliceProject slot-publication materialization. */
                if (producer_id == 0u || runtime.lazy_trace_attachment == nullptr ||
                    runtime.lazy_trace_attachment->runtime == nullptr ||
                    !runtime.lazy_trace_attachment->runtime->collection_enabled) {
                    continue;
                }
                MemoryContext previous = MemoryContextSwitchTo(result_context);
                try {
                    source = bioseq::postgres::support::invoke_pg_as_cpp(
                        [source, trace = runtime.lazy_trace_attachment, producer_id] {
                            auto adopted = acquire_writable_pg_sequence(source, trace, producer_id);
                            return MakeExpandedObjectReadOnly(
                                pg_sequence_rw_datum(adopted.value), false, -1);
                        });
                    MemoryContextSwitchTo(previous);
                } catch (...) {
                    MemoryContextSwitchTo(previous);
                    throw;
                }
                slot->tts_values[index] = source;
                continue;
            }
            if (column_attno <= 0) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqSliceProject locator lost its physical column"),
                         errdetail("result_attribute=%d", static_cast<int>(resno))));
            }
            slot->tts_values[index] =
                bioseq::storage::promote_pg_sequence_locator(runtime.lazy_source_provider,
                                                             column_attno,
                                                             source,
                                                             result_context,
                                                             runtime.lazy_trace_attachment,
                                                             producer_id);
        }
    }

    enum class mmsearch_source_capture_role : std::uint8_t {
        evaluated_operand,
        output_projection,
    };

    struct mmsearch_source_restore_entry {
        AttrNumber attno = InvalidAttrNumber;
        Datum datum{};
    };

    /* Four physical sources cover the ordinary target/query/output case without
     * a per-match_result allocation.  Wider projections spill into the executor
     * per-tuple MemoryContext, which PostgreSQL releases even when ERROR skips C++
     * destructors.  The object is populated before PG_TRY and never mutated in
     * its setjmp region, so either storage form is safe to read from PG_CATCH. */
    struct mmsearch_source_restore {
        static constexpr std::size_t inline_capacity = 4u;
        std::array<mmsearch_source_restore_entry, inline_capacity> inline_entries{};
        mmsearch_source_restore_entry* overflow_entries = nullptr;
        std::size_t count = 0u;

        [[nodiscard]] mmsearch_source_restore_entry& entry(const std::size_t index) noexcept {
            return overflow_entries == nullptr ? inline_entries[index] : overflow_entries[index];
        }

        [[nodiscard]] const mmsearch_source_restore_entry&
        entry(const std::size_t index) const noexcept {
            return overflow_entries == nullptr ? inline_entries[index] : overflow_entries[index];
        }
    };

    void mark_mmsearch_physical_sequence_source(std::vector<std::uint8_t>& roles,
                                                TupleDesc relation_descriptor,
                                                const AttrNumber attno,
                                                const Oid sequence_type_oid) {
        if (relation_descriptor == nullptr)
            return;
        const int natts = relation_descriptor->natts;
        if (attno == 0) {
            for (int index = 0; index < natts; ++index) {
                Form_pg_attribute attribute = TupleDescAttr(relation_descriptor, index);
                if (attribute != nullptr && !attribute->attisdropped &&
                    attribute->atttypid == sequence_type_oid) {
                    roles[static_cast<std::size_t>(index + 1)] = 1u;
                }
            }
            return;
        }
        if (attno <= 0 || attno > natts)
            return;
        Form_pg_attribute attribute = TupleDescAttr(relation_descriptor, attno - 1);
        if (attribute != nullptr && !attribute->attisdropped &&
            attribute->atttypid == sequence_type_oid) {
            roles[static_cast<std::size_t>(attno)] = 1u;
        }
    }

    /* Mark physical sequence Vars from one finalized executor expression tree.
     * This mirrors collect_needed_column_attributes' Var/whole-row rules, but records only
     * sequence columns and keeps the lifetime role separate from read demand. */
    void mark_mmsearch_sequence_sources_from_expression(std::vector<std::uint8_t>& roles,
                                                        TupleDesc relation_descriptor,
                                                        Node* expression,
                                                        const Oid sequence_type_oid) {
        if (expression == nullptr)
            return;
        constexpr int pull_flags =
            PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
        List* variables = pull_var_clause(expression, pull_flags);
        for (Var* variable : postgres_list<Var>(variables)) {
            if (variable != nullptr)
                mark_mmsearch_physical_sequence_source(
                    roles, relation_descriptor, variable->varattno, sequence_type_oid);
        }
        list_free(variables);
    }

    /* Derive timing from the final executor trees, not the broader BSS3 provider
     * tag.  newtargetlist is the exact per-match_result projection after helper
     * injection; plan.qual is the executable residual frontier.  Operand lineage
     * is added because evaluated submission executes both operand ExprStates even
     * when only one side is syntactically transformed. */
    void derive_mmsearch_source_lifetime_roles(bioseq_scan_runtime& runtime,
                                               Plan* plan,
                                               TupleDesc relation_descriptor) {
        runtime.mmsearch_pre_eval_sources_present = false;
        if (relation_descriptor == nullptr)
            return;
        const std::size_t role_count = static_cast<std::size_t>(relation_descriptor->natts + 1);
        runtime.mmsearch_pre_eval_source_bound.assign(role_count, 0u);
        runtime.mmsearch_output_source_bound.assign(role_count, 0u);
        const Oid sequence_type_oid = get_cached_stub_oids().bioseq_seq_type_oid;
        mark_mmsearch_sequence_sources_from_expression(
            runtime.mmsearch_output_source_bound,
            relation_descriptor,
            reinterpret_cast<Node*>(runtime.newtargetlist),
            sequence_type_oid);
        if (plan != nullptr)
            mark_mmsearch_sequence_sources_from_expression(runtime.mmsearch_pre_eval_source_bound,
                                                           relation_descriptor,
                                                           reinterpret_cast<Node*>(plan->qual),
                                                           sequence_type_oid);
        if (runtime.match_evaluated_submission) {
            for (Var* source : postgres_list<Var>(runtime.match_target_lineage)) {
                if (source != nullptr)
                    mark_mmsearch_physical_sequence_source(runtime.mmsearch_pre_eval_source_bound,
                                                           relation_descriptor,
                                                           source->varattno,
                                                           sequence_type_oid);
            }
            if (runtime.match_query_evaluated) {
                for (Var* source : postgres_list<Var>(runtime.match_query_lineage)) {
                    if (source != nullptr)
                        mark_mmsearch_physical_sequence_source(
                            runtime.mmsearch_pre_eval_source_bound,
                            relation_descriptor,
                            source->varattno,
                            sequence_type_oid);
                }
            }
        }
        runtime.mmsearch_pre_eval_sources_present =
            std::any_of(runtime.mmsearch_pre_eval_source_bound.begin(),
                        runtime.mmsearch_pre_eval_source_bound.end(),
                        [](const std::uint8_t bound) { return bound != 0u; });
    }

    [[nodiscard]] bool
    mmsearch_source_matches_capture_role(const bioseq_scan_runtime& runtime,
                                         const AttrNumber attno,
                                         const mmsearch_source_capture_role role) noexcept {
        const auto& roles = role == mmsearch_source_capture_role::output_projection
                                ? runtime.mmsearch_output_source_bound
                                : runtime.mmsearch_pre_eval_source_bound;
        const std::size_t index = static_cast<std::size_t>(attno);
        return index < roles.size() && roles[index] != 0u;
    }

    /* Capture each physical attribute at most once.  Iterating the slot's attno
     * namespace forms the exact union required by the role: evaluated operand
     * leaves before ExprState execution, or finalized output-projection sources
     * before per-match_result projection. */
    [[nodiscard]] mmsearch_source_restore
    capture_mmsearch_sources(bioseq_scan_runtime& runtime,
                             TupleTableSlot* slot,
                             MemoryContext restore_context,
                             const mmsearch_source_capture_role role) {
        mmsearch_source_restore restore{};
        if (runtime.lazy_source_provider == nullptr || slot == nullptr ||
            slot->tts_tupleDescriptor == nullptr || restore_context == nullptr)
            return restore;
        slot_getallattrs(slot);
        const int natts = slot->tts_tupleDescriptor->natts;
        std::size_t count = 0u;
        for (int index = 0; index < natts; ++index) {
            const AttrNumber attno = static_cast<AttrNumber>(index + 1);
            if (!slot->tts_isnull[index] &&
                mmsearch_source_matches_capture_role(runtime, attno, role)) {
                ++count;
            }
        }
        if (count == 0u)
            return restore;
        restore.count = count;
        if (count > mmsearch_source_restore::inline_capacity) {
            restore.overflow_entries = static_cast<mmsearch_source_restore_entry*>(
                MemoryContextAlloc(restore_context, count * sizeof(mmsearch_source_restore_entry)));
        }
        std::size_t captured = 0u;
        for (int index = 0; index < natts; ++index) {
            const AttrNumber attno = static_cast<AttrNumber>(index + 1);
            if (slot->tts_isnull[index] ||
                !mmsearch_source_matches_capture_role(runtime, attno, role)) {
                continue;
            }
            restore.entry(captured) = mmsearch_source_restore_entry{
                .attno = attno,
                .datum = slot->tts_values[index],
            };
            ++captured;
        }
        return restore;
    }

    [[nodiscard]] std::size_t promote_mmsearch_sources(bioseq_scan_runtime& runtime,
                                                       TupleTableSlot* slot,
                                                       MemoryContext output_context,
                                                       const mmsearch_source_restore& restore) {
        if (runtime.lazy_source_provider == nullptr || slot == nullptr ||
            output_context == nullptr) {
            return 0u;
        }
        std::size_t promoted_count = 0u;
        for (std::size_t index = 0u; index < restore.count; ++index) {
            const AttrNumber attno = restore.entry(index).attno;
            if (attno <= 0 || attno > slot->tts_tupleDescriptor->natts)
                continue;
            const Datum source = restore.entry(index).datum;
            if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(source)))
                continue;

            /* ExecCopySlotMinimalTuple may short-pack this STORAGE=extended value.
             * Promotion and the locator ABI require the ordinary four-byte varlena
             * header, so detoast in the output lifetime before inspecting it. */
            MemoryContext previous = MemoryContextSwitchTo(output_context);
            bioseq::pg_types::seq* flat = nullptr;
            try {
                flat = reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(source));
                MemoryContextSwitchTo(previous);
            } catch (...) {
                MemoryContextSwitchTo(previous);
                throw;
            }
            if (static_cast<std::size_t>(VARSIZE(flat)) < bioseq::pg_types::seq::header_bytes()) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("buffered bioseq output source is truncated"),
                         errdetail("attribute=%d bytes=%zu",
                                   static_cast<int>(attno),
                                   static_cast<std::size_t>(VARSIZE(flat)))));
            }
            if (!bioseq::storage::is_locator_stub(flat))
                continue;
            std::uint32_t producer_id = 0u;
            slice_trace_attachment* trace_attachment = nullptr;
            if (runtime.lazy_source_projection) {
                const std::size_t binding_index = static_cast<std::size_t>(attno);
                if (binding_index < runtime.lazy_source_physical_bound.size() &&
                    binding_index < runtime.lazy_source_physical_producer_ids.size() &&
                    runtime.lazy_source_physical_bound[binding_index] != 0u) {
                    trace_attachment = runtime.lazy_trace_attachment;
                    producer_id = runtime.lazy_source_physical_producer_ids[binding_index];
                }
            }
            slot->tts_values[attno - 1] =
                bioseq::storage::promote_pg_sequence_locator(runtime.lazy_source_provider,
                                                             attno,
                                                             PointerGetDatum(flat),
                                                             output_context,
                                                             trace_attachment,
                                                             producer_id);
            ++promoted_count;
        }
        return promoted_count;
    }

    void restore_mmsearch_sources(TupleTableSlot* slot,
                                  const mmsearch_source_restore& restore) noexcept {
        if (slot == nullptr)
            return;
        for (std::size_t index = 0u; index < restore.count; ++index) {
            const AttrNumber attno = restore.entry(index).attno;
            if (attno > 0 && attno <= slot->tts_tupleDescriptor->natts) {
                slot->tts_values[attno - 1] = restore.entry(index).datum;
            }
        }
    }

    [[nodiscard]] bool
    mmsearch_sources_restored_exactly(TupleTableSlot* slot,
                                      const mmsearch_source_restore& restore) noexcept {
        if (slot == nullptr || slot->tts_tupleDescriptor == nullptr) {
            return restore.count == 0u;
        }
        for (std::size_t index = 0u; index < restore.count; ++index) {
            const AttrNumber attno = restore.entry(index).attno;
            if (attno <= 0 || attno > slot->tts_tupleDescriptor->natts ||
                slot->tts_values[attno - 1] != restore.entry(index).datum) {
                return false;
            }
        }
        return true;
    }

    struct mmsearch_row_evaluation {
        bool qual_passed = false;
        Datum target{};
        bool target_is_null = true;
        Datum query{};
        bool query_is_null = true;
    };

} // export custom-scan partition declarations

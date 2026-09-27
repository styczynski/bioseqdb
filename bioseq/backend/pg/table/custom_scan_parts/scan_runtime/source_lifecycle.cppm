/**
 * @file
 * @brief Makes stored sequence locators usable by expressions and restores buffered rows.
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

export module bioseq.postgres.table:custom_scan_scan_runtime_source_lifecycle;

export import :custom_scan_scan_runtime_source_lifecycle_contracts;

import std;
import bioseq.postgres.support;
import bioseq.postgres.storage.manager;
import bioseq.postgres.storage.scanner;
import bioseq.postgres.table.scan_state;
import bioseq.postgres.types.compact_sequence;
import bioseq.storage.lazy_sequence;

import :custom_scan_oid_expression_support;

using namespace bioseq::postgres::support;

export {
    /**
     * @brief Replaces bound sequence locators with lazy values in @p result_context.
     *
     * Checks the slot against @p expected_descriptor and uses @p layout to find
     * the physical source columns. A locator-only scan is skipped unless
     * @p allow_locator_passthrough is true. When tracing is enabled, the values
     * also carry their planned producer IDs.
     */
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
            if (bioseq::postgres::support::tuple_descriptor_attribute(actual_descriptor, index)->atttypid != seq_oid) {
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
                /* Inline sequences need an expanded wrapper to carry their producer ID
                 * during ANALYZE. Adopt the value in the row context before evaluating the
                 * expression. This allocation is counted as adopting a source value. */
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

    /** @brief Marks one physical sequence attribute, or every sequence attribute for whole-row use. */
    void mark_mmsearch_physical_sequence_source(std::vector<std::uint8_t>& roles,
                                                TupleDesc relation_descriptor,
                                                const AttrNumber attno,
                                                const Oid sequence_type_oid) {
        if (relation_descriptor == nullptr)
            return;
        const int natts = relation_descriptor->natts;
        if (attno == 0) {
            for (int index = 0; index < natts; ++index) {
                Form_pg_attribute attribute = bioseq::postgres::support::tuple_descriptor_attribute(relation_descriptor, index);
                if (attribute != nullptr && !attribute->attisdropped &&
                    attribute->atttypid == sequence_type_oid) {
                    roles[static_cast<std::size_t>(index + 1)] = 1u;
                }
            }
            return;
        }
        if (attno <= 0 || attno > natts)
            return;
        Form_pg_attribute attribute = bioseq::postgres::support::tuple_descriptor_attribute(relation_descriptor, attno - 1);
        if (attribute != nullptr && !attribute->attisdropped &&
            attribute->atttypid == sequence_type_oid) {
            roles[static_cast<std::size_t>(attno)] = 1u;
        }
    }

    /**
     * @brief Marks the physical sequence columns used by an executor expression.
     *
     * Expands whole-row references to all sequence columns. These marks record
     * when the values must remain usable, separately from the scan's read-column bitmap.
     */
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

    /**
     * @brief Finds which sequence sources are needed before alignment and during output.
     *
     * Uses the rewritten output list for projection sources and plan.qual for
     * pre-alignment filters. Evaluated operands add their source columns to the
     * pre-alignment set, so their expressions can read sequences safely.
     */
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

    /** @brief Checks whether a source column is needed at the selected evaluation step. */
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

    /**
     * @brief Saves the original values of columns needed for evaluation or projection.
     *
     * Records each non-NULL column once, before temporary locator replacement.
     * Returns an empty record when the source provider, slot or memory context
     * is unavailable. @p restore_context owns storage beyond the inline capacity.
     */
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

    /**
     * @brief Makes captured locators readable in the supplied evaluation context.
     *
     * Replaces only locator values and leaves expanded or ordinary sequences as they are.
     * @return The number of slot values replaced.
     */
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

            /* Copying a minimal tuple can shorten a sequence's varlena header.
             * Restore the four-byte header in this output context before reading
             * the locator fields. */
            MemoryContext previous = MemoryContextSwitchTo(output_context);
            bioseq::pg_types::seq* flat = nullptr;
            try {
                flat = reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(source));
                MemoryContextSwitchTo(previous);
            } catch (...) {
                MemoryContextSwitchTo(previous);
                throw;
            }
            if (static_cast<std::size_t>(bioseq::postgres::support::varlena_4byte_size(flat)) < bioseq::pg_types::seq::header_bytes()) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("buffered bioseq output source is truncated"),
                         errdetail("attribute=%d bytes=%zu",
                                   static_cast<int>(attno),
                                   static_cast<std::size_t>(bioseq::postgres::support::varlena_4byte_size(flat)))));
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

    /** @brief Puts saved source values back into the slot after evaluation or an error. */
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

    /** @brief Checks that each saved datum is back in its original slot column. */
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

} // export custom-scan partition declarations

/** @brief Closes the source provider and drops tracing references after slots release their values. */
void release_scan_runtime_lazy_resources(bioseq_scan_runtime& runtime,
                                         const bool is_commit) {
    if (runtime.lazy_source_provider != nullptr) {
        bioseq::storage::close_pg_sequence_source_provider(runtime.lazy_source_provider,
                                                            is_commit);
        runtime.lazy_source_provider = nullptr;
    }
    runtime.buffer.clear_sequence_source_provider_binding();
    if (runtime.lazy_trace_attachment != nullptr) {
        release_slice_trace_attachment(runtime.lazy_trace_attachment);
        runtime.lazy_trace_attachment = nullptr;
    }
}

/**
 * @file
 * @brief Release batch-join tuples, sequence readers, and reserved memory.
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

export module bioseq.postgres.table:custom_scan_batch_join_execution_resource_lifecycle;

import std;
import bioseq.postgres.support;
import bioseq.sequences.compact_sequence_typmod;

import :custom_scan_batch_join_execution_runtime_state_contracts;
import :custom_scan_oid_expression_support_oid_catalog_core_contracts;
import :custom_scan_batch_join_execution_resource_lifecycle_contracts;

using namespace bioseq::postgres::support;

export {

    /** Return the  join runtime, or null when the node has none. */
    [[nodiscard]] batch_join_runtime* try_get_batch_join_runtime(CustomScanState* node) noexcept {
        auto* state = reinterpret_cast<batch_join_state*>(node);
        return state == nullptr ? nullptr : static_cast<batch_join_runtime*>(state->runtime);
    }

    /**
     * @brief Clear output references before releasing borrowed sequence values.
     *
     * Virtual slots can point into saved expanded sequences. Clear the resut
     * and scan slots, then reset expression references before the dispatch buffer
     * frees that storage.
     */
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

    /** Clear output references and forget the current target slot. */
    void batch_join_clear_parent_tuple_references(CustomScanState* node,
                                                  batch_join_runtime& runtime) {
        clear_recipe_scan_parent_tuple_references(node);
        runtime.current_target_slot = nullptr;
    }

    /**
     * @brief Drain join work before releasing tuples, memory reservations, and children.
     *
     * Translate PostgreSQL errors into C++ exceptions during cleanup Repeated
     * calls do nothing once cleanup is complete.
     * @param delete_query_context Delete the query  context if true, otherwise reset it.
     */
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
        /* Keep the memory reservation until the tuple storage has been freed. */
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

    /** Forget PostgreSQL resources after an error and release the remaining C++ state. */
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

    /** @brief Destroys the C++ runtime from its query-memory-context reset callback. */
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

    /**
     * @brief Clear saved query rows and reset their memory context.
     * @param drop_slots Explicitly drop live row slots before resetting the context.
     */
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
        /* Release the reservation after freeing tuple storage and its slots. */
        runtime.query_permits.clear();
        runtime.query_reserved_admission_bytes = 0;
        runtime.query_actual_materialized_bytes = 0;
        runtime.queries_materialized = false;
    }

    /**
     * @brief Check the layout and sequence  type of the slot returned by a child.
     *
     * ExecProcNode can return a scan slot or another child's slot, directly. Check
     * that slot before reading it. Its layout, sequence alphabet, and optional
     * bigint row number must match the saved plan.
     * @throws PostgreSQL ERROR for a layout or type misnatch.
     */
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
        Form_pg_attribute sequence_attribute = bioseq::postgres::support::tuple_descriptor_attribute(descriptor, sequence_resno - 1);
        Form_pg_attribute rowno_attribute =
            rowno_required ? bioseq::postgres::support::tuple_descriptor_attribute(descriptor, rowno_resno - 1) : nullptr;
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

    /**
     * @brief Raise a materialization error while allowing C++ cleanup to run.
     *
     * A direct PostgreSQL error would jump past local vector destructors. The
     * translated exception releases those objects before returning to PostgreSQL.
     */
    [[noreturn]] void batch_join_raise_materialization_as_cpp(const int sqlstate,
                                                              const char* message,
                                                              const std::string detail) {
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

} // export custom-scan partition declarations

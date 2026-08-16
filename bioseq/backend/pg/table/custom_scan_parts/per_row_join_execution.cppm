/**
 * @file
 * @brief Executes transformed per-row mmsearch joins and owns their child lifecycle.
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

export module bioseq.postgres.table:custom_scan_per_row_join_execution;

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
import :custom_scan_batch_join_execution;

using namespace bioseq::postgres::support;

export {

    /** @brief Returns the per-row C++ runtime stored behind a custom scan state. */
    [[nodiscard]] per_row_join_runtime*
    try_get_per_row_join_runtime(CustomScanState* node) noexcept {
        auto* state = reinterpret_cast<per_row_join_state*>(node);
        return state == nullptr ? nullptr : static_cast<per_row_join_runtime*>(state->runtime);
    }

    /** @brief Marks per-row resources abandoned after PostgreSQL error unwinding. */
    void per_row_join_abandon_after_error(CustomScanState* node,
                                          per_row_join_runtime& runtime) noexcept {
        if (runtime.cleanup_done)
            return;
        runtime.buffer.abandon_join_runtime_after_pg_error_noexcept();
        runtime.child = nullptr;
        runtime.target_relation = nullptr;
        runtime.child_initialized = false;
        runtime.child_ended = true;
        if (node != nullptr)
            node->custom_ps = NIL;
        runtime.cleanup_done = true;
    }

    /** @brief Destroys the per-row runtime from its query-memory-context callback. */
    void per_row_join_runtime_cleanup(void* argument) {
        auto* state = static_cast<per_row_join_state*>(argument);
        if (state == nullptr || state->runtime == nullptr)
            return;
        auto* runtime = static_cast<per_row_join_runtime*>(state->runtime);
        per_row_join_abandon_after_error(&state->custom_scan_state, *runtime);
        state->runtime = nullptr;
        std::destroy_at(runtime);
    }

    /** @brief Drains asynchronous ownership and releases child and relation resources in order. */
    void per_row_join_release_checked(CustomScanState* node, per_row_join_runtime& runtime) {
        if (runtime.cleanup_done)
            return;
        clear_recipe_scan_parent_tuple_references(node);
        runtime.buffer.clear_slots();
        runtime.buffer.release_per_row_slice_async_owner_after_drain();
        if (runtime.child_initialized && !runtime.child_ended && runtime.child != nullptr) {
            ExecEndNode(runtime.child);
            runtime.child_ended = true;
        }
        if (runtime.target_relation != nullptr) {
            table_close(runtime.target_relation, NoLock);
            runtime.target_relation = nullptr;
        }
        runtime.child = nullptr;
        if (node != nullptr)
            node->custom_ps = NIL;
        runtime.cleanup_done = true;
    }

    /**
     * @brief Validates the serialized per-row recipe and constructs its query-owned runtime.
     * @throws PostgreSQL ERROR when the plan shape, binding recipe, or child ABI is invalid.
     */
    void
    bioseq_per_row_join_begin_custom_scan_impl(CustomScanState* node, EState* estate, int eflags) {
        auto* cscan = reinterpret_cast<CustomScan*>(node->ss.ps.plan);
        if (IsParallelWorker() || cscan == nullptr || cscan->scan.scanrelid != 0 ||
            cscan->custom_exprs != NIL || list_length(cscan->custom_plans) != 1 ||
            list_length(cscan->custom_private) != k_bioseq_per_row_join_private_fields ||
            node->custom_ps != NIL) {
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("BioseqPerRowMmsearch executor shape is invalid or parallel")));
        }
        for (TargetEntry* output : postgres_list<TargetEntry>(cscan->scan.plan.targetlist)) {
            if (output != nullptr && output->expr != nullptr &&
                batch_join_expression_has_match_helper(reinterpret_cast<Node*>(output->expr),
                                                       get_cached_stub_oids())) {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqPerRowMmsearch executable projection retained a match helper")));
            }
        }
        auto* state = reinterpret_cast<per_row_join_state*>(node);
        auto* runtime = std::construct_at(static_cast<per_row_join_runtime*>(
            MemoryContextAlloc(estate->es_query_cxt, sizeof(per_row_join_runtime))));
        state->runtime = runtime;
        runtime->cleanup_callback.func = per_row_join_runtime_cleanup;
        runtime->cleanup_callback.arg = state;
        MemoryContextRegisterResetCallback(estate->es_query_cxt, &runtime->cleanup_callback);
        runtime->cleanup_callback_registered = true;
        auto private_integer = [cscan](const int index) -> int {
            Node* value = reinterpret_cast<Node*>(list_nth(cscan->custom_private, index));
            if (value == nullptr || !IsA(value, Integer)) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqPerRowMmsearch executor integer ABI is invalid"),
                         errdetail("field=%d", index)));
            }
            return intVal(value);
        };
        const int version = private_integer(k_per_row_version);
        const int stable_match_id =
            validate_finalized_match_owner_descriptor(cscan, k_per_row_match_owner_descriptor);
        const Oid target_relation_oid =
            static_cast<Oid>(private_integer(k_per_row_target_relation_oid));
        const AttrNumber target_storage_attno =
            static_cast<AttrNumber>(private_integer(k_per_row_target_storage_attno));
        runtime->target_slot_resno =
            static_cast<AttrNumber>(private_integer(k_per_row_target_slot_resno));
        runtime->query_slot_resno =
            static_cast<AttrNumber>(private_integer(k_per_row_query_slot_resno));
        runtime->child_column_count = private_integer(k_per_row_child_column_count);
        const int raw_alphabet = private_integer(k_per_row_alphabet_code);
        const int raw_width = private_integer(k_per_row_storage_unit_byte_width);
        const int raw_query_alphabet = private_integer(k_per_row_query_alphabet_code);
        const int raw_query_width = private_integer(k_per_row_query_storage_unit_byte_width);
        const int target_typmod_erased = private_integer(k_per_row_target_typmod_erased);
        const int query_typmod_erased = private_integer(k_per_row_query_typmod_erased);
        const int raw_requirement = private_integer(k_per_row_index_read_requirement);
        runtime->honest_startup_cost = batch_join_cost_from_node(
            reinterpret_cast<Node*>(list_nth(cscan->custom_private, k_per_row_honest_startup_cost)),
            "honest_startup_cost");
        runtime->honest_total_cost = batch_join_cost_from_node(
            reinterpret_cast<Node*>(list_nth(cscan->custom_private, k_per_row_honest_total_cost)),
            "honest_total_cost");
        runtime->incumbent_cost_floor =
            batch_join_cost_from_node(reinterpret_cast<Node*>(list_nth(
                                          cscan->custom_private, k_per_row_incumbent_cost_floor)),
                                      "incumbent_cost_floor");
        Node* sites_node =
            reinterpret_cast<Node*>(list_nth(cscan->custom_private, k_per_row_placeholder_sites));
        const bool sites_shape_valid = sites_node == nullptr || IsA(sites_node, List);
        List* sites = sites_node == nullptr ? NIL : reinterpret_cast<List*>(sites_node);
        FuncExpr* mmsearch = try_cast_pg_node<FuncExpr>(
            reinterpret_cast<Node*>(list_nth(cscan->custom_private, k_per_row_mmsearch_expr)));
        if (version != k_bioseq_per_row_join_plan_version || !OidIsValid(target_relation_oid) ||
            target_storage_attno <= 0 || runtime->target_slot_resno <= 0 ||
            runtime->query_slot_resno <= 0 || runtime->child_column_count <= 0 ||
            raw_alphabet < 0 ||
            raw_alphabet > static_cast<int>(std::numeric_limits<std::uint8_t>::max()) ||
            (raw_width != 1 && raw_width != 2 && raw_width != 4 && raw_width != 8) ||
            raw_query_alphabet < 0 ||
            raw_query_alphabet >
                static_cast<int>(std::numeric_limits<std::uint8_t>::max()) ||
            (raw_query_width != 1 && raw_query_width != 2 && raw_query_width != 4 &&
             raw_query_width != 8) ||
            (target_typmod_erased != 0 && target_typmod_erased != 1) ||
            (query_typmod_erased != 0 && query_typmod_erased != 1) ||
            raw_requirement !=
                static_cast<int>(bioseq::indexing::index_read_requirement::require_absent) ||
            !sites_shape_valid || mmsearch == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqPerRowMmsearch executor private ABI is invalid")));
        }
        runtime->alphabet_code = static_cast<std::uint8_t>(raw_alphabet);
        runtime->storage_unit_byte_width = static_cast<std::uint8_t>(raw_width);
        runtime->query_alphabet_code = static_cast<std::uint8_t>(raw_query_alphabet);
        runtime->query_storage_unit_byte_width = static_cast<std::uint8_t>(raw_query_width);
        if (runtime->alphabet_code != runtime->query_alphabet_code) {
            raise_mmsearch_alphabet_mismatch(
                compact_sequence_stdc_type_params{runtime->alphabet_code,
                                                  runtime->storage_unit_byte_width},
                compact_sequence_stdc_type_params{runtime->query_alphabet_code,
                                                  runtime->query_storage_unit_byte_width},
                "post-join recipe begin");
        }
        TupleDesc scan_desc = node->ss.ss_ScanTupleSlot == nullptr
                                  ? nullptr
                                  : node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
        if (scan_desc == nullptr || runtime->child_column_count > scan_desc->natts ||
            list_length(sites) != scan_desc->natts - runtime->child_column_count ||
            runtime->target_slot_resno > runtime->child_column_count ||
            runtime->query_slot_resno > runtime->child_column_count) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqPerRowMmsearch scan tuple layout is invalid")));
        }
        const int32 expected_typmod =
            compact_sequence_stdc_type_params{runtime->alphabet_code,
                                              runtime->storage_unit_byte_width}
                .encode();
        const int32 expected_query_typmod =
            compact_sequence_stdc_type_params{runtime->query_alphabet_code,
                                              runtime->query_storage_unit_byte_width}
                .encode();
        const std::array<AttrNumber, 2> operand_resnos{runtime->target_slot_resno,
                                                       runtime->query_slot_resno};
        const std::array<int, 2> typmod_erased_flags{target_typmod_erased, query_typmod_erased};
        for (std::size_t index = 0u; index < operand_resnos.size(); ++index) {
            const AttrNumber resno = operand_resnos[index];
            Form_pg_attribute attribute = TupleDescAttr(scan_desc, resno - 1);
            if (attribute->atttypid != get_cached_stub_oids().bioseq_seq_type_oid ||
                attribute->atttypmod !=
                    (typmod_erased_flags[index] != 0
                         ? -1
                         : (index == 0u ? expected_typmod : expected_query_typmod))) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATATYPE_MISMATCH),
                         errmsg("BioseqPerRowMmsearch operand type is invalid")));
            }
        }
        Bitmapset* placeholder_resnos = nullptr;
        for (List* encoded : postgres_list<List>(sites)) {
            if (encoded == nullptr || !IsA(encoded, List) ||
                list_length(encoded) != k_bioseq_match_helper_site_fields) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqPerRowMmsearch placeholder ABI is invalid")));
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
                             errmsg("BioseqPerRowMmsearch placeholder scalar is invalid")));
                }
            }
            const int raw_resno = intVal(list_nth(encoded, k_match_site_resno));
            if (raw_resno <= runtime->child_column_count || raw_resno > scan_desc->natts ||
                bms_is_member(raw_resno, placeholder_resnos)) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqPerRowMmsearch placeholder resno is invalid")));
            }
            placeholder_resnos = bms_add_member(placeholder_resnos, raw_resno);
            const batch_join_placeholder_site site{
                .resno = static_cast<AttrNumber>(raw_resno),
                .funcid = static_cast<Oid>(intVal(list_nth(encoded, k_match_site_funcid))),
                .typmod = intVal(list_nth(encoded, k_match_site_typmod)),
                .collation = static_cast<Oid>(intVal(list_nth(encoded, k_match_site_collation))),
                .binding_kind = static_cast<match_helper_binding_kind>(
                    intVal(list_nth(encoded, k_match_site_binding_kind))),
                .stable_match_id = intVal(list_nth(encoded, k_match_site_stable_id)),
            };
            Form_pg_attribute attribute = TupleDescAttr(scan_desc, site.resno - 1);
            if (!is_bioseq_projection_funcid(site.funcid, get_cached_stub_oids()) ||
                attribute->atttypid != get_func_rettype(site.funcid) ||
                attribute->atttypmod != site.typmod || attribute->attcollation != site.collation ||
                (site.binding_kind != match_helper_binding_kind::exact_recipe &&
                 site.binding_kind != match_helper_binding_kind::unique_lineage) ||
                site.stable_match_id != stable_match_id) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATATYPE_MISMATCH),
                         errmsg("BioseqBatchJoin post-join recipe placeholder type is invalid")));
            }
            runtime->placeholder_sites.push_back(site);
        }
        bms_free(placeholder_resnos);

        bioseq::simd::minimap2_parameters params = build_params_from_gucs();
        bioseq::indexing::index_read_requirement decoded_requirement =
            bioseq::indexing::index_read_requirement::allow;
        (void)apply_aligner_options_override(
            mmsearch, params, decoded_requirement, get_cached_stub_oids());
        finalize_simd_chaining_policy(params);
        params.alphabet_code = runtime->alphabet_code;
        if (decoded_requirement ==
            bioseq::indexing::index_read_requirement::require_compatible_persisted) {
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("transformed mmsearch target cannot require a persisted index")));
        }
        params.do_cigar = false;
        params.compute_divergence = false;
        params.compute_mapq = false;
        params.compute_edit_distance = false;
        const bioseq_stub_oids& placeholder_oids = get_cached_stub_oids();
        for (auto const& site : runtime->placeholder_sites) {
            params.do_cigar = params.do_cigar || site.funcid == placeholder_oids.cigar_oid ||
                              site.funcid == placeholder_oids.has_cigar_oid ||
                              site.funcid == placeholder_oids.edit_oid;
            params.compute_edit_distance =
                params.compute_edit_distance || site.funcid == placeholder_oids.edit_oid;
            params.compute_divergence =
                params.compute_divergence || site.funcid == placeholder_oids.divergence_oid;
            params.compute_mapq = params.compute_mapq || site.funcid == placeholder_oids.mapq_oid;
        }
        Plan* child_plan = reinterpret_cast<Plan*>(linitial(cscan->custom_plans));
        batch_join_plan_helper_audit_first_failure helper_audit_failure{};
        if (batch_join_plan_has_unowned_executable_match_helper(
                child_plan, get_cached_stub_oids(), cscan, helper_audit_failure, "none")) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqPerRowMmsearch child retained an executable match helper"),
                     errdetail("reason=%s; store=%s; plan_node_tag=%d; custom_name=%s; "
                               "projection_capable=%s; first_helper_resno=%d; "
                               "whole_helper_valid=%s; finalized_batch_descendant_matches=%zu; "
                               "finalized_base_descendant_matches=%zu; current_parent_matches=%zu; "
                               "boundary_encountered=%s; setrefs did not preserve the "
                               "descendant-owner helper route; no child was initialized",
                               helper_audit_failure.reason,
                               helper_audit_failure.store,
                               helper_audit_failure.plan_node_tag,
                               helper_audit_failure.custom_name,
                               helper_audit_failure.projection_capable ? "true" : "false",
                               helper_audit_failure.first_helper_resno,
                               helper_audit_failure.whole_helper_valid ? "true" : "false",
                               helper_audit_failure.finalized_batch_descendant_matches,
                               helper_audit_failure.finalized_base_descendant_matches,
                               helper_audit_failure.current_parent_matches,
                               helper_audit_failure.boundary_encountered)));
        }
        runtime->child = ExecInitNode(child_plan, estate, eflags);
        runtime->child_initialized = true;
        node->custom_ps = list_make1(runtime->child);
        runtime->target_relation = table_open(target_relation_oid, NoLock);
        if (runtime->target_relation == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_TABLE),
                     errmsg("BioseqPerRowMmsearch target relation is unavailable")));
        }
        if (runtime->target_relation->rd_tableam != column_table_access_method()) {
            ereport(ERROR,
                    (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                     errmsg("BioseqPerRowMmsearch target relation changed table AM")));
        }
        TupleDesc relation_desc = RelationGetDescr(runtime->target_relation);
        Form_pg_attribute storage_attribute =
            target_storage_attno > relation_desc->natts
                ? nullptr
                : TupleDescAttr(relation_desc, target_storage_attno - 1);
        if (storage_attribute == nullptr || storage_attribute->attisdropped ||
            storage_attribute->atttypid != get_cached_stub_oids().bioseq_seq_type_oid ||
            storage_attribute->atttypmod != expected_typmod) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("BioseqPerRowMmsearch target storage column changed")));
        }
        runtime->aligner_params = params;
        runtime->aligner_params_captured = true;
        runtime->pipeline_desc = describe_pipeline_for_pg_alphabet(
            params,
            runtime->alphabet_code,
            query_sketch_plan_route_for(bioseq::indexing::index_read_requirement::require_absent));
        runtime->buffer.configure_params(params,
                                         bioseq::indexing::index_read_requirement::require_absent,
                                         /*read_sequence_index=*/false,
                                         target_relation_oid,
                                         runtime->target_relation->rd_locator.dbOid,
                                         runtime->target_relation->rd_locator.spcOid,
                                         runtime->target_relation->rd_locator.relNumber,
                                         runtime->target_relation,
                                         estate->es_snapshot,
                                         estate->es_query_cxt);
        char* column_name =
            get_attname(target_relation_oid, target_storage_attno, /*missing_ok=*/false);
        runtime->buffer.configure_mmsearch_per_row(runtime->target_slot_resno,
                                                   std::string{column_name},
                                                   runtime->query_slot_resno,
                                                   runtime->target_slot_resno ==
                                                       runtime->query_slot_resno,
                                                   runtime->alphabet_code,
                                                   runtime->storage_unit_byte_width,
                                                   runtime->query_alphabet_code,
                                                   runtime->query_storage_unit_byte_width,
                                                   InvalidAttrNumber);
        pfree(column_name);
        const int plan_node_id = cscan->scan.plan.plan_node_id;
        if (plan_node_id < 0) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqPerRowMmsearch plan node id is invalid")));
        }
        runtime->alignment_registration =
            bioseq::postgres::table::alignment_execution::bind_alignment_node(
                estate, static_cast<std::uint32_t>(plan_node_id));
        runtime->buffer.bind_persisted_gpu_operation_stats_sink(
            *bioseq::postgres::table::alignment_execution::
                borrow_persisted_gpu_operation_accumulator(runtime->alignment_registration));
    }

    /** @brief Initializes a per-row join while preserving PostgreSQL and C++ error boundaries. */
    void bioseq_per_row_join_begin_custom_scan(CustomScanState* node, EState* estate, int eflags) {
        MemoryContext saved_context = CurrentMemoryContext;
        ErrorData* volatile original_error = nullptr;
        ErrorData* volatile converted_pg_error = nullptr;
        std::exception_ptr cpp_error;
        PG_TRY();
        {
            try {
                bioseq_per_row_join_begin_custom_scan_impl(node, estate, eflags);
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
        // Invalidates partially initialized runtime state before an error is rethrown.
        auto abandon = [node]() noexcept {
            if (per_row_join_runtime* runtime = try_get_per_row_join_runtime(node);
                runtime != nullptr) {
                per_row_join_abandon_after_error(node, *runtime);
                if (!runtime->cleanup_callback_registered) {
                    auto* state = reinterpret_cast<per_row_join_state*>(node);
                    state->runtime = nullptr;
                    std::destroy_at(runtime);
                }
            }
        };
        if (original_error != nullptr) {
            abandon();
            ErrorData* exact = original_error;
            ReThrowError(exact);
        }
        if (converted_pg_error != nullptr) {
            abandon();
            ErrorData* exact = converted_pg_error;
            ReThrowError(exact);
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
                     errmsg("BioseqBatchJoin post-join recipe Begin raised a C++ exception"),
                     errdetail_internal("%s", detail.data())));
        }
    }

    /** @brief Populates match placeholders in the retained child row for parent projection. */
    [[nodiscard]] TupleTableSlot*
    per_row_join_project_output(CustomScanState* node,
                                per_row_join_runtime& runtime,
                                bioseq::scheduler::dispatch_next_output const& output) {
        if (output.scan_slot_copy == nullptr || output.match == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqPerRowMmsearch output references invalid input")));
        }
        ExprContext* context = node->ss.ps.ps_ExprContext;
        if (context == nullptr || context->ecxt_per_tuple_memory == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin post-join recipe expression context is missing")));
        }
        ResetExprContext(context);
        TupleTableSlot* combined = node->ss.ss_ScanTupleSlot;
        ExecClearTuple(combined);
        slot_getallattrs(output.scan_slot_copy);
        if (runtime.child_column_count > output.scan_slot_copy->tts_tupleDescriptor->natts ||
            runtime.child_column_count > combined->tts_tupleDescriptor->natts) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqPerRowMmsearch output tuple layout changed")));
        }
        for (int index = 0; index < runtime.child_column_count; ++index) {
            combined->tts_values[index] = output.scan_slot_copy->tts_values[index];
            combined->tts_isnull[index] = output.scan_slot_copy->tts_isnull[index];
        }
        const bioseq_stub_oids& oids = get_cached_stub_oids();
        for (auto const& site : runtime.placeholder_sites) {
            batch_join_set_placeholder_value(
                combined, site, *output.match, oids, context->ecxt_per_tuple_memory);
        }
        ExecStoreVirtualTuple(combined);
        context->ecxt_scantuple = combined;
        return combined;
    }

    /** @brief Feeds child rows through per-row alignment and returns the next completed match. */
    TupleTableSlot* bioseq_per_row_join_exec_custom_scan_impl(CustomScanState* node) {
        per_row_join_runtime* runtime = try_get_per_row_join_runtime(node);
        if (runtime == nullptr || runtime->child == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqPerRowMmsearch runtime is missing")));
        }
        /* ExecScan may reject an Access result and call us again without returning
         * through BioseqPerRowJoin_ExecCustomScan. Clear that prior virtual row on
         * every Access entry so a subsequent clear_slots() cannot invalidate a
         * still-parent-visible expanded recipe. */
        clear_recipe_scan_parent_tuple_references(node);
        bioseq::postgres::table::alignment_execution::ensure_alignment_node_registered(
            runtime->alignment_registration);
        for (;;) {
            if (runtime->buffer.has_finished_writing()) {
                auto output = runtime->buffer.read_next_output();
                if (output.has_value()) {
                    return per_row_join_project_output(node, *runtime, *output);
                }
                if (runtime->source_exhausted) {
                    if (!runtime->pass_fully_drained) {
                        bioseq::postgres::table::alignment_execution::
                            mark_alignment_pass_fully_drained(runtime->alignment_registration);
                        runtime->pass_fully_drained = true;
                    }
                    return ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
                }
                runtime->buffer.clear_slots();
                runtime->buffer.release_per_row_slice_async_owner_after_drain();
                continue;
            }
            TupleTableSlot* child_slot = ExecProcNode(runtime->child);
            if (TupIsNull(child_slot)) {
                runtime->source_exhausted = true;
                if (runtime->buffer.run_batch_and_build_cursors()) {
                    auto delta = runtime->buffer.execution_stats();
                    runtime->aggregate_stats.merge(delta);
                    bioseq::postgres::table::alignment_execution::merge_alignment_batch(
                        runtime->alignment_registration, delta);
                }
                continue;
            }
            bool target_is_null = false;
            bool query_is_null = false;
            const Datum target =
                slot_getattr(child_slot, runtime->target_slot_resno, &target_is_null);
            const Datum query = slot_getattr(child_slot, runtime->query_slot_resno, &query_is_null);
            if (target_is_null || query_is_null)
                continue;
            slice_async_owner_guard* async_owner = nullptr;
            for (const Datum operand : std::array{target, query}) {
                auto* attachment = pg_sequence_trace_attachment_for_datum(operand);
                auto* candidate = borrow_slice_async_owner_noexcept(attachment);
                if (candidate == nullptr)
                    continue;
                if (async_owner != nullptr && async_owner != candidate) {
                    ereport(ERROR,
                            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                             errmsg("BioseqPerRowMmsearch operands have distinct async owners")));
                }
                async_owner = candidate;
            }
            if (async_owner != nullptr) {
                runtime->buffer.bind_slice_async_owner(async_owner);
            }
            runtime->buffer.submit_scan(child_slot);
            if (runtime->buffer.ready_to_dispatch()) {
                if (runtime->buffer.run_batch_and_build_cursors()) {
                    auto delta = runtime->buffer.execution_stats();
                    runtime->aggregate_stats.merge(delta);
                    bioseq::postgres::table::alignment_execution::merge_alignment_batch(
                        runtime->alignment_registration, delta);
                }
            }
        }
    }

    /** @brief Adapts the per-row producer to PostgreSQL's ExecScan access callback. */
    [[nodiscard]] TupleTableSlot* bioseq_per_row_join_access(ScanState* scan) {
        return bioseq_per_row_join_exec_custom_scan_impl(reinterpret_cast<CustomScanState*>(scan));
    }

    /** @brief Replays the owned alignment recipe for EvalPlanQual replacement rows. */
    [[nodiscard]] bool bioseq_per_row_join_recheck(ScanState* scan, TupleTableSlot*) {
        auto* custom = reinterpret_cast<CustomScanState*>(scan);
        if (custom->ss.ps.state == nullptr || custom->ss.ps.state->es_epq_active == nullptr)
            return true;
        /* The child plan is EPQ-aware. Pull its substituted row through the same
         * owned recipe/alignment pipeline, but return only the scan tuple here:
         * ExecScan performs the final projection exactly once after recheck. */
        return !TupIsNull(bioseq_per_row_join_exec_custom_scan_impl(custom));
    }

    /** @brief Executes one per-row join step and contains all errors within PostgreSQL's ABI. */
    TupleTableSlot* bioseq_per_row_join_exec_custom_scan(CustomScanState* node) {
        TupleTableSlot* result = nullptr;
        MemoryContext saved_context = CurrentMemoryContext;
        ErrorData* volatile original_error = nullptr;
        ErrorData* volatile converted_pg_error = nullptr;
        std::exception_ptr cpp_error;
        PG_TRY();
        {
            try {
                clear_recipe_scan_parent_tuple_references(node);
                result =
                    ExecScan(&node->ss, bioseq_per_row_join_access, bioseq_per_row_join_recheck);
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
        // Invalidates executor-owned runtime state before an error is rethrown.
        auto abandon = [node]() noexcept {
            if (per_row_join_runtime* runtime = try_get_per_row_join_runtime(node);
                runtime != nullptr) {
                per_row_join_abandon_after_error(node, *runtime);
            }
        };
        if (original_error != nullptr) {
            abandon();
            ErrorData* exact = original_error;
            ReThrowError(exact);
        }
        if (converted_pg_error != nullptr) {
            abandon();
            ErrorData* exact = converted_pg_error;
            ReThrowError(exact);
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
                     errmsg("BioseqBatchJoin post-join recipe Exec raised a C++ exception"),
                     errdetail_internal("%s", detail.data())));
        }
        return result;
    }

    /** @brief Ends the child plan and releases per-row asynchronous ownership. */
    void bioseq_per_row_join_end_custom_scan(CustomScanState* node) {
        per_row_join_runtime* runtime = try_get_per_row_join_runtime(node);
        if (runtime == nullptr || runtime->cleanup_done)
            return;
        try {
            per_row_join_release_checked(node, *runtime);
        } catch (bioseq::postgres::support::postgres_error& error) {
            per_row_join_abandon_after_error(node, *runtime);
            ErrorData* exact = error.release_error_data();
            ReThrowError(exact);
        } catch (const std::exception& error) {
            per_row_join_abandon_after_error(node, *runtime);
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqPerRowMmsearch End raised a C++ exception"),
                     errdetail_internal("%s", error.what())));
        }
    }

    /** @brief Resets per-row buffers and propagates changed parameters to the owned child. */
    void bioseq_per_row_join_rescan_custom_scan(CustomScanState* node) {
        per_row_join_runtime* runtime = try_get_per_row_join_runtime(node);
        if (runtime == nullptr || runtime->child == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqPerRowMmsearch rescan runtime is missing")));
        }
        bioseq::postgres::table::alignment_execution::record_alignment_rescan(
            runtime->alignment_registration);
        clear_recipe_scan_parent_tuple_references(node);
        runtime->buffer.clear_slots();
        runtime->buffer.release_per_row_slice_async_owner_after_drain();
        runtime->source_exhausted = false;
        runtime->pass_fully_drained = false;
        /* Like SliceProject, this CustomScan owns its child through custom_ps.
         * Transfer only the parameters relevant to that child's allParam before
         * ExecReScan; UpdateChangedParamSet performs the required filtering and
         * unions with any invalidation already owned by the child. */
        if (node->ss.ps.chgParam != nullptr) {
            UpdateChangedParamSet(runtime->child, node->ss.ps.chgParam);
        }
        ExecScanReScan(&node->ss);
        ExecReScan(runtime->child);
    }

    /** @brief Adds per-row ownership, plan-audit, cost, and execution details to EXPLAIN. */
    void bioseq_per_row_join_explain_custom_scan(CustomScanState* node, List*, ExplainState* es) {
        per_row_join_runtime* runtime = try_get_per_row_join_runtime(node);
        ExplainPropertyText("Bioseq Mmsearch Ownership", "per-row recipe", es);
        ExplainPropertyText("Bioseq Reconstructed Join", "child plan", es);
        per_row_mmsearch_plan_audit child_audit{};
        auto* cscan = reinterpret_cast<CustomScan*>(node->ss.ps.plan);
        FuncExpr* owned_mmsearch =
            cscan != nullptr &&
                    list_length(cscan->custom_private) == k_bioseq_per_row_join_private_fields
                ? try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(
                      list_nth(cscan->custom_private, k_per_row_mmsearch_expr)))
                : nullptr;
        if (cscan != nullptr && list_length(cscan->custom_plans) == 1) {
            per_row_audit_mmsearch_plan(reinterpret_cast<Plan*>(linitial(cscan->custom_plans)),
                                        child_audit,
                                        get_cached_stub_oids(),
                                        owned_mmsearch);
        }
        /* A nonzero path count cannot reach plan creation: the exact pointer gate
         * above runs on the reconstructed Path before copyObject.  The remaining
         * values are recomputed from the serialized executable plan, not constants. */
        ExplainPropertyInteger("Bioseq Owned RestrictInfo Path Copies", nullptr, 0, es);
        ExplainPropertyInteger("Bioseq Executable Mmsearch Qual Copies",
                               nullptr,
                               static_cast<std::int64_t>(child_audit.executable_qual_copies),
                               es);
        ExplainPropertyInteger("Bioseq Vector Mmsearch Qual Copies",
                               nullptr,
                               static_cast<std::int64_t>(child_audit.vector_qual_copies),
                               es);
        ExplainPropertyInteger("Bioseq Storage Mmsearch Qual Copies",
                               nullptr,
                               static_cast<std::int64_t>(child_audit.storage_qual_copies),
                               es);
        ExplainPropertyInteger("Bioseq Descendant Mmsearch Owner Copies",
                               nullptr,
                               static_cast<std::int64_t>(child_audit.descendant_owner_copies),
                               es);
        ExplainPropertyText("Bioseq Residual Mmsearch Qual", "absent", es);
        ExplainPropertyInteger("Bioseq Batch Capacity",
                               nullptr,
                               static_cast<std::int64_t>(k_bioseq_runtime_buffer_reserve),
                               es);
        ExplainPropertyText(
            "Bioseq Index Read Requirement", "require_absent (transformed target)", es);
        if (runtime == nullptr)
            return;
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
        explain_pipeline_execution_stats(
            runtime->aggregate_stats,
            es,
            runtime->aligner_params_captured ? &runtime->aligner_params : nullptr,
            runtime->pipeline_desc.has_value() ? &*runtime->pipeline_desc : nullptr,
            query_sketch_plan_route_for(bioseq::indexing::index_read_requirement::require_absent),
            /*highway_dispatch_already_rendered=*/false);
    }

} // export custom-scan partition declarations

/**
 * @file
 * @brief Aligns target and query expressions from each joined row  and returns their matches.
 */
module;
// Include Highway here so its target-specific definitions share the same
// translation-unit context across imported modules.
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
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

export module bioseq.postgres.table:custom_scan_per_row_join_execution;

import bioseq.highway.dispatch_contract;
import bioseq.indexing.type;
import bioseq.pipeline.pipe;
import bioseq.postgres.scheduler.dispatch_buffer.api;
import bioseq.postgres.support;
import bioseq.postgres.table.alignment_execution;
import bioseq.postgres.types.compact_sequence;
import bioseq.sequences.compact_sequence_typmod;

import :table_access_method_contracts;
import :custom_scan_join_ownership_descendant_neutralization_contracts;
import :custom_scan_join_ownership_helper_presence_contracts;
import :custom_scan_join_ownership_cost_contracts;
import :custom_scan_join_ownership_alphabet_mismatch_contracts;
import :custom_scan_match_analysis_match_owner_descriptor_contracts;
import :custom_scan_match_analysis_descendant_neutralization_contracts;
import :custom_scan_match_analysis_transformed_verification_contracts;
import :custom_scan_oid_expression_support_oid_catalog_core_contracts;
import :custom_scan_plan_abi_constants;
import :custom_scan_planner_aligner_contracts;
import :custom_scan_per_row_join_runtime_contracts;
import :custom_scan_runtime_buffer_contracts;
import :custom_scan_runtime_match_contracts;
import :custom_scan_runtime_pipeline_contracts;
import :custom_scan_execution_stats_contracts;
import :custom_scan_semantic_payload_query_sketch_route_contracts;

import :custom_scan_batch_join_execution_resource_lifecycle_contracts;
import :custom_scan_batch_join_execution_tuple_production_contracts;
export import :custom_scan_per_row_join_execution_contracts;

using namespace bioseq::postgres::support;

export {

    /** @brief Returns the per-row C++ runtime stored behind a custom scan state. */
    [[nodiscard]] per_row_join_runtime*
    try_get_per_row_join_runtime(CustomScanState* node) noexcept {
        auto* state = reinterpret_cast<per_row_join_state*>(node);
        return state == nullptr ? nullptr : static_cast<per_row_join_runtime*>(state->runtime);
    }

    /**
     * @brief Abandons buffered work and forgets PostgreSQL resources after an error.
     *
     * Clear child and relation pointers so later cleanup uannot reuse them.
     * PostgreSQL handles their error cleanup.
     */
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

    /** @brief Abandons remaining work and destroys the C++ runtime when query memory is reset. */
    void per_row_join_runtime_cleanup(void* argument) {
        auto* state = static_cast<per_row_join_state*>(argument);
        if (state == nullptr || state->runtime == nullptr)
            return;
        auto* runtime = static_cast<per_row_join_runtime*>(state->runtime);
        per_row_join_abandon_after_error(&state->custom_scan_state, *runtime);
        state->runtime = nullptr;
        std::destroy_at(runtime);
    }

    /**
     * @brief Releases buffered rows before closing the child plan and target relation.
     *
     * Clear output references first, then clear the buffer and release its slice
     * owner. The child may free the sequence  storage once those references are gone.
     */
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
     * @brief Starts a per-row mmsearch scan from the saved plan
     *
     * Validate operand types and match-helper bindings before initializing the child.
     * Build the alignment plan using the requested helper outputs, then configure
     * the buffer to align each child row without a persisted index. Register query
     * cleanup as soon as the runtime exists.
     *
     * @throws PostgreSQL ERROR for an unsupported, or inconsistent plan.
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
        const int raw_target_source_tid_resno =
            private_integer(k_per_row_target_source_tid_resno);
        const int raw_target_source_tableoid_resno =
            private_integer(k_per_row_target_source_tableoid_resno);
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
        const bool source_identity_absent =
            raw_target_source_tid_resno == InvalidAttrNumber &&
            raw_target_source_tableoid_resno == InvalidAttrNumber;
        const bool source_identity_carried =
            raw_target_source_tid_resno > 0 &&
            raw_target_source_tid_resno <= runtime->child_column_count &&
            raw_target_source_tid_resno <=
                static_cast<int>((std::numeric_limits<AttrNumber>::max)()) &&
            raw_target_source_tableoid_resno > 0 &&
            raw_target_source_tableoid_resno <= runtime->child_column_count &&
            raw_target_source_tableoid_resno <=
                static_cast<int>((std::numeric_limits<AttrNumber>::max)());
        if (version != k_bioseq_per_row_join_plan_version || !OidIsValid(target_relation_oid) ||
            target_storage_attno <= 0 || runtime->target_slot_resno <= 0 ||
            runtime->query_slot_resno <= 0 || runtime->child_column_count <= 0 ||
            (!source_identity_absent && !source_identity_carried) ||
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
        runtime->target_source_tid_resno =
            static_cast<AttrNumber>(raw_target_source_tid_resno);
        runtime->target_source_tableoid_resno =
            static_cast<AttrNumber>(raw_target_source_tableoid_resno);
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
            runtime->query_slot_resno > runtime->child_column_count ||
            runtime->target_source_tid_resno > runtime->child_column_count ||
            runtime->target_source_tableoid_resno > runtime->child_column_count) {
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
            Form_pg_attribute attribute = bioseq::postgres::support::tuple_descriptor_attribute(scan_desc, resno - 1);
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
        if (source_identity_carried) {
            Form_pg_attribute target_source_tid_attribute =
                bioseq::postgres::support::tuple_descriptor_attribute(scan_desc, runtime->target_source_tid_resno - 1);
            Form_pg_attribute target_source_tableoid_attribute =
                bioseq::postgres::support::tuple_descriptor_attribute(scan_desc, runtime->target_source_tableoid_resno - 1);
            if (target_source_tid_attribute->attisdropped ||
                target_source_tid_attribute->atttypid != TIDOID ||
                target_source_tid_attribute->atttypmod != -1 ||
                OidIsValid(target_source_tid_attribute->attcollation) ||
                target_source_tableoid_attribute->attisdropped ||
                target_source_tableoid_attribute->atttypid != OIDOID ||
                target_source_tableoid_attribute->atttypmod != -1 ||
                OidIsValid(target_source_tableoid_attribute->attcollation)) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATATYPE_MISMATCH),
                         errmsg("BioseqPerRowMmsearch target source identity type is invalid")));
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
            Form_pg_attribute attribute = bioseq::postgres::support::tuple_descriptor_attribute(scan_desc, site.resno - 1);
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
        const int plan_node_id = cscan->scan.plan.plan_node_id;
        if (plan_node_id < 0) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqPerRowMmsearch plan node id is invalid")));
        }
        runtime->alignment_registration =
            bioseq::postgres::table::alignment_execution::bind_alignment_node(
                estate, static_cast<std::uint32_t>(plan_node_id));
        params.downstream_active_wall = bioseq::postgres::table::alignment_execution::
                borrow_downstream_active_wall_tracker(runtime->alignment_registration);
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
        runtime->execution_plan =
            bioseq::scheduler::make_postgres_statement_execution_plan(std::move(params));
        [[maybe_unused]] const auto& frozen_params = runtime->execution_plan->frozen_parameters;
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
                : bioseq::postgres::support::tuple_descriptor_attribute(relation_desc, target_storage_attno - 1);
        if (storage_attribute == nullptr || storage_attribute->attisdropped ||
            storage_attribute->atttypid != get_cached_stub_oids().bioseq_seq_type_oid ||
            storage_attribute->atttypmod != expected_typmod) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("BioseqPerRowMmsearch target storage column changed")));
        }
        runtime->pipeline_desc = describe_pipeline_for_pg_alphabet(
            runtime->execution_plan,
            runtime->alphabet_code,
            query_sketch_plan_route_for(bioseq::indexing::index_read_requirement::require_absent));
        runtime->buffer.configure_params(runtime->execution_plan,
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
        runtime->buffer.bind_persisted_gpu_operation_stats_sink(
            *bioseq::postgres::table::alignment_execution::
                borrow_persisted_gpu_operation_accumulator(runtime->alignment_registration));
        runtime->buffer.bind_downstream_active_wall_tracker(
            *bioseq::postgres::table::alignment_execution::
                borrow_downstream_active_wall_tracker(runtime->alignment_registration));
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        if (frozen_params.downstream_active_wall == nullptr ||
            !frozen_params.downstream_active_wall
                 ->begin_benchmark_e2e_statement_plan_proof(
                     runtime->execution_plan,
                     bioseq::simd::benchmark_e2e_statement_plan_shape::per_row_join)) {
            throw std::logic_error{
                "BioseqPerRowMmsearch admission changed its statement-plan control block"};
        }
#endif
    }

    /**
     * @brief Starts a per-row scan and cleans up failed initialization before reporting errors.
     *
     * Preserve PostgreSQL error details. Convert other C++ failures to PostgreSQL
     * internal errors after abandoning the  partially initialized runtime.
     */
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
        // Abandon partial initialization before reporting the saved ereor.
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

    /** @brief Combines the saved child row with this match's helper values for projection. */
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

    /**
     * @brief Returns the next match while feeding joined rows to the alignment buffer.
     *
     * Each child row supplies one target/query pair. Skip NULL operands and keep
     * the row for any matches it produces. Drain completed output before reusing
     * the buffer, and run the final partial batch when the child is exhausted.
     */
    TupleTableSlot* bioseq_per_row_join_exec_custom_scan_impl(CustomScanState* node) {
        per_row_join_runtime* runtime = try_get_per_row_join_runtime(node);
        if (runtime == nullptr || runtime->child == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqPerRowMmsearch runtime is missing")));
        }
        /* ExecScan can reject a row and call this access method again within
         * one scan step. Clear  the previous output each time, before resetting
         * the buffer can free sequence data still referenced by that row. */
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
            const bool has_source_identity = runtime->target_source_tid_resno > 0;
            Oid source_relation_oid = InvalidOid;
            ItemPointerData source_tid_copy{};
            if (has_source_identity) {
                bool source_tid_is_null = true;
                bool source_tableoid_is_null = true;
                const Datum source_tid_datum = slot_getattr(
                    child_slot, runtime->target_source_tid_resno, &source_tid_is_null);
                const Datum source_tableoid_datum = slot_getattr(
                    child_slot, runtime->target_source_tableoid_resno, &source_tableoid_is_null);
                ItemPointer source_tid =
                    source_tid_is_null || source_tid_datum == Datum{0}
                        ? nullptr
                        : DatumGetItemPointer(source_tid_datum);
                source_relation_oid =
                    source_tableoid_is_null ? InvalidOid : DatumGetObjectId(source_tableoid_datum);
                if (source_tid_is_null || source_tid == nullptr || !ItemPointerIsValid(source_tid) ||
                    source_tableoid_is_null || !OidIsValid(source_relation_oid)) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("BioseqPerRowMmsearch target source identity is null or invalid")));
                }
                source_tid_copy = *source_tid;
            }
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
            if (has_source_identity) {
                runtime->buffer.submit_scan_with_source_tid(
                    child_slot, source_relation_oid, source_tid_copy);
            } else {
                // Without explicit source columns, ignore any TID left in the parent slot
                runtime->buffer.submit_scan_recipe_without_source_tid(child_slot);
            }
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

    /** @brief Aligns the child's replacement row during an EvalPlanQual recheck. */
    [[nodiscard]] bool bioseq_per_row_join_recheck(ScanState* scan, TupleTableSlot*) {
        auto* custom = reinterpret_cast<CustomScanState*>(scan);
        if (custom->ss.ps.state == nullptr || custom->ss.ps.state->es_epq_active == nullptr)
            return true;
        /* The child supplies the replacement row for EvalPlanQual. Run it
         * through alignment and return the scan tuple so ExecScan can apply
         * the final projection once. */
        return !TupIsNull(bioseq_per_row_join_exec_custom_scan_impl(custom));
    }

    /** @brief Runs a per-row scan step and reports PostgreSQL or C++ failures after cleanup. */
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
        // Abandon the runtime before reporting the saved execution  errok.
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

    /** @brief Closes the per-row scan after releasing buffered sequence references. */
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

    /** @brief Clears buffered matches and restarts the child with updated query parameters. */
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
        /* Forward parameter changes to the owned child before restarting it.
         * UpdateChangedParamSet keeps only parameters the child uses and
         * preserves changes already recorded there. */
        if (node->ss.ps.chgParam != nullptr) {
            UpdateChangedParamSet(runtime->child, node->ss.ps.chgParam);
        }
        ExecScanReScan(&node->ss);
        ExecReScan(runtime->child);
    }

    /** @brief Shows per-row match execution, child-plan checks, and alignment costs in EXPLAIN. */
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
        /* Path construction already rejects a child that still owns this
         * RestrictInfo, so its path-copy count is, zero. Recount executable
         * mmsearch expressions from the saved child plan for the other fields. */
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
            runtime->execution_plan ? &runtime->execution_plan->frozen_parameters : nullptr,
            runtime->pipeline_desc.has_value() ? &*runtime->pipeline_desc : nullptr,
            query_sketch_plan_route_for(bioseq::indexing::index_read_requirement::require_absent),
            /*highway_dispatch_already_rendered=*/false);
    }

} // export custom-scan partition declarations

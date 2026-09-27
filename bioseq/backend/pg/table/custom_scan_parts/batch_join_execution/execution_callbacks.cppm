/**
 * @file
 * @brief Execute sequence joins in query batches and report their statistics.
 */
module;
// Include Highway here so imported target-specific types share one
// translation-unit definition.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
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

export module bioseq.postgres.table:custom_scan_batch_join_execution_execution_callbacks;

import bioseq.indexing.type;
import bioseq.pipeline.pipe;
import bioseq.postgres.configuration.core;
import bioseq.postgres.scheduler.dispatch_buffer.api;
import bioseq.postgres.support;
import bioseq.postgres.table.alignment_execution;
import bioseq.postgres.table.scan_state;
import bioseq.sequences.compact_sequence_typmod;

import :api;
import :custom_scan_join_ownership_helper_presence_contracts;
import :custom_scan_join_ownership_cost_contracts;
import :custom_scan_join_ownership_sequence_typmod_contract;
import :custom_scan_match_analysis_match_owner_descriptor_contracts;
import :custom_scan_oid_expression_support_oid_catalog_core_contracts;
import :custom_scan_execution_method_contracts;
import :custom_scan_execution_stats_contracts;
import :custom_scan_plan_abi_constants;
import :custom_scan_planner_aligner_contracts;
import :custom_scan_planner_method_contracts;
import :custom_scan_runtime_pipeline_contracts;
import :custom_scan_batch_join_execution_runtime_state_contracts;
import :custom_scan_scan_runtime_state_contracts;
import :custom_scan_semantic_payload_query_sketch_route_contracts;

import :custom_scan_batch_join_execution_resource_lifecycle_contracts;
import :custom_scan_batch_join_execution_query_materialization_contracts;
import :custom_scan_batch_join_execution_tuple_production_contracts;

using namespace bioseq::postgres::support;

export {
    /** Check for the one-child per-row join plan handled by these shared callbacks. */
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

    /**
     * @brief Validate the join plan and initialize its children and execution state.
     * @throws PostgreSQL ERROR for invalid plan fields, column types, or source bindings.
     */
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
        /* Match helpers remain in custom_scan_tlist as column descriptions.
         * PostgreSQL must replace executable helper calls with slot references
         * before execution. Reject a remaining call before initializing children,
         * since the helper stub cannot compute an alignment result. */
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
                /* NIL is a valid empty helper list. Check non-null nodes have List type. */
                const bool sites_shape_valid = sites_node == nullptr || IsA(sites_node, List);
                List* sites = sites_node == nullptr ? NIL : reinterpret_cast<List*>(sites_node);
                FuncExpr* mmsearch = try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(
                    list_nth(cscan->custom_private, k_batch_join_mmsearch_expr)));
                if (version != k_bioseq_batch_join_plan_version || token <= 0 ||
                    !OidIsValid(runtime->target_relation_oid) ||
                    runtime->target_storage_seq_attno <= 0 || runtime->target_slot_seq_resno <= 0 ||
                    runtime->target_slot_rowno_resno <= 0 || runtime->query_slot_seq_resno <= 0 ||
                    runtime->target_column_count <= 0 || runtime->query_column_count <= 0 ||
                    raw_alphabet_code < 0 ||
                    raw_alphabet_code >
                        static_cast<int>(std::numeric_limits<std::uint8_t>::max()) ||
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

                /* Decode aligner options again before starting either child. Check the
                 * saved index requirement still agrees with the catalog value. */
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
                /* Vulkan target preparation happens in the dispatch buffer after the
                 * children start. It validates the persisted target before alignment,
                 * so initialization here must let that preparation run. */

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
                            : bioseq::postgres::support::tuple_descriptor_attribute(scan_desc, site.resno - 1);
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
                /* Check the completed child plans here. ExecProcNode can return a
                 * child's scan slot directly, so validate each returned slot again
                 * before reading its values. */
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

                /* Save the validated sequence types and requested result fields in the
                 * execution plan before initializing children. */
                bioseq::simd::minimap2_parameters params = std::move(prevalidated_params);
                const auto requirement = decoded_requirement;
                params.alphabet_code = runtime->alphabet_code;
                /* Match helpers have become INDEX_VAR slot references. Use their saved
                 * descriptions to enable expensive result calculations such as CIGAR
                 * and mapping quality. */
                const int plan_node_id = cscan->scan.plan.plan_node_id;
                if (plan_node_id < 0) {
                    ereport(ERROR,
                            (errcode(ERRCODE_INTERNAL_ERROR),
                             errmsg("BioseqBatchJoin plan node id is invalid")));
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

                runtime->execution_plan =
                    bioseq::scheduler::make_postgres_statement_execution_plan(std::move(params));
                [[maybe_unused]] const auto& frozen_params = runtime->execution_plan->frozen_parameters;

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

                /* Borrow the Relation kept open by the target child until ExecEndNode. */
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
                        : bioseq::postgres::support::tuple_descriptor_attribute(relation_desc, runtime->target_storage_seq_attno - 1);
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
                /* Flushing writes may have changed the target child's snapshot. Use its
                 * current registered snapshot for fragments too. Cleanup drains the
                 * buffer before ending that child and releasing the snapshot. */
                runtime->pipeline_desc = describe_pipeline_for_pg_alphabet(
                    runtime->execution_plan,
                    runtime->alphabet_code, query_sketch_plan_route_for(requirement));
                runtime->buffer.configure_params(runtime->execution_plan,
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
                runtime->buffer.bind_persisted_gpu_operation_stats_sink(
                    *bioseq::postgres::table::alignment_execution::
                        borrow_persisted_gpu_operation_accumulator(
                            runtime->alignment_registration));
                runtime->buffer.bind_downstream_active_wall_tracker(
                    *bioseq::postgres::table::alignment_execution::
                        borrow_downstream_active_wall_tracker(
                            runtime->alignment_registration));
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                if (frozen_params.downstream_active_wall == nullptr ||
                    !frozen_params.downstream_active_wall
                         ->begin_benchmark_e2e_statement_plan_proof(
                             runtime->execution_plan,
                             bioseq::simd::benchmark_e2e_statement_plan_shape::batch_join)) {
                    throw std::logic_error{
                        "BioseqBatchJoin admission changed its statement-plan control block"};
                }
#endif
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

    /** Return the next match, running query batches against one target at a time.
     *
     * Buffer the query side once, then drain each batch's results before moving
     * to another batch or target. Apply the output predicates and projection to
     * each match before returning it.
     */
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
                /* A null target cannot match the strict INNER join predicate. */
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

    /**
     * @brief Return the next join result and preserve PostgreSQL error details.
     * @return A projected row, or an empty slot after all targets and results are consumed.
     */
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

    /** Release join resources and report cleanup failures as PostgreSQL errors. */
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

    /** Clear buffered work and restart both children for another join pass. */
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

    /** Show the join's saved settings, costs, memory usage, and alignment work in EXPLAIN. */
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
            runtime->execution_plan && runtime->execution_plan->uses_vulkan_seed()
                ? "disabled: selected Vulkan seed route owns target preparation"
            : runtime->index_read_requirement ==
                    bioseq::indexing::index_read_requirement::require_absent
                ? "native query sketches retained; one transient target across query waves"
            : runtime->index_read_requirement ==
                    bioseq::indexing::index_read_requirement::require_compatible_persisted
                ? "compatible persisted target reader retained across query waves"
            : !runtime->buffer.reads_sequence_index()
                ? "disabled: effective sequence-index reads are off; per-pair target route"
                : "prepared query owner; one target index per parameter group",
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
                runtime->query_planner_admission_bytes,
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))),
            es);
        ExplainPropertyInteger(
            "Bioseq Query Actual Materialization",
            "bytes",
            static_cast<std::int64_t>(std::min<std::uint64_t>(
                runtime->query_actual_materialized_bytes,
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))),
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
            runtime->execution_plan ? &runtime->execution_plan->frozen_parameters : nullptr,
            runtime->pipeline_desc.has_value() ? &*runtime->pipeline_desc : nullptr,
            query_sketch_plan_route_for(runtime->index_read_requirement),
            /*highway_dispatch_already_rendered=*/false);
    }

    /** Return the PostgreSQL state-creation callback for Bioseq table scans. */
    [[nodiscard]] const CustomScanMethods* bioseq_column_scan_scan_methods() noexcept {
        static const CustomScanMethods methods{
            .CustomName = "BioseqScan",
            .CreateCustomScanState = bioseq_column_scan_create_custom_scan_state,
        };
        return &methods;
    }

    /** Return the PostgreSQL state-creation callback for sequence joins. */
    [[nodiscard]] const CustomScanMethods* bioseq_batch_join_scan_methods() noexcept {
        static const CustomScanMethods methods{
            .CustomName = "BioseqBatchJoin",
            .CreateCustomScanState = bioseq_batch_join_create_custom_scan_state,
        };
        return &methods;
    }

    /** Return the PostgreSQL execution callbacks shared by both sequence-join plans. */
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

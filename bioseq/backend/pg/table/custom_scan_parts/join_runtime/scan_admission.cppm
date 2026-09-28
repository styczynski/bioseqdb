/**
 * @file
 * @brief Starts BioseqScan by checking its  plan and configuring sequence reads and alignment.
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

export module bioseq.postgres.table:custom_scan_join_runtime_scan_admission;

import std;
import bioseq.common.logging;
import bioseq.postgres.configuration.aligner;
import bioseq.postgres.configuration.core;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.scanner.source_provider_factory;
import bioseq.postgres.scheduler.supervisor.vulkan;
import bioseq.postgres.support;
import bioseq.postgres.table.alignment_execution;
import bioseq.postgres.table.scan_state;
import bioseq.postgres.table.types;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.vectorization.types;
import bioseq.indexing.type;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.sequences.simd;

import :utils;
import :custom_scan_plan_abi_constants;
import :custom_scan_scan_feature_contracts;
import :custom_scan_scan_runtime_state_contracts;
import :custom_scan_runtime_pipeline_contracts;
import :custom_scan_oid_expression_support_oid_catalog_contracts;
import :custom_scan_oid_expression_support_expression_analysis_contracts;
import :custom_scan_oid_expression_support_restriction_ownership_contracts;
import :custom_scan_oid_expression_support_semantic_analysis_contracts;
import :custom_scan_semantic_payload_payload_contracts;
import :custom_scan_semantic_payload_query_sketch_route_contracts;
import :custom_scan_planner_aligner_contracts;
import :custom_scan_match_analysis_carrier_validation_contracts;
import :custom_scan_join_ownership_helper_contracts;
import :custom_scan_join_ownership_sequence_typmod_validation_contracts;
import :custom_scan_join_ownership_semantic_admission_contracts;

import :custom_scan_join_runtime_parameter_binding_contracts;

using namespace bioseq::postgres::support;

export {
    /**
     * @brief Checks a BioseqScan plan and prepares its runtime for this query
     *
     * Bind sequence reads to the scan snapshot, configure the owned mmsearch
     * inputs, and select the alignment work required by the output fields.
     * The query memory context owns the cleanup callback.
     *
     * @throws PostgreSQL ERROR for invalid saved plan data or unsupported
     * execution settings.
     */
    void bioseq_column_scan_begin_custom_scan_impl(CustomScanState* cscanstate,
                                                   EState* estate,
                                                   int eflags) {
        bioseq_log<logging_level::debug>("[bioseq] pg: BioseqColumnScan_BeginCustomScan() entered");
        CustomScan* cscan = reinterpret_cast<CustomScan*>(cscanstate->ss.ps.plan);
        scan_state* bioseq_scan_state = scan_state::from_custom_scan_state(cscanstate);
        ExprContext* stdecontext = cscanstate->ss.ps.ps_ExprContext;
        cscanstate->ss.ss_currentScanDesc = nullptr;
        register_runtime_cleanup_callback(bioseq_scan_state, estate);
        bioseq_scan_runtime& runtime = bioseq_runtime(bioseq_scan_state);
        runtime.locator_passthrough = false;
        runtime.lazy_source_projection = false;
        runtime.lazy_source_projected_bindings.clear();
        runtime.lazy_source_physical_producer_ids.clear();
        runtime.lazy_source_physical_bound.clear();
        runtime.mmsearch_pre_eval_source_bound.clear();
        runtime.mmsearch_output_source_bound.clear();
        runtime.mmsearch_pre_eval_sources_present = false;
        runtime.locator_parent_token = 0;
        runtime.locator_rowno_resno = InvalidAttrNumber;
        runtime.edit_predicate_active = false;
        runtime.edit_exclusive_upper_bound = 0;
        runtime.edit_sequence_attno = InvalidAttrNumber;
        runtime.match_sequence_varno = 0;
        runtime.match_sequence_attno = InvalidAttrNumber;
        runtime.match_target_recipe = nullptr;
        runtime.match_query_recipe = nullptr;
        runtime.match_target_lineage = NIL;
        runtime.match_query_lineage = NIL;
        runtime.match_target_expr_state = nullptr;
        runtime.match_query_expr_state = nullptr;
        runtime.match_target_typmod = -1;
        runtime.match_query_typmod = -1;
        runtime.match_target_evaluated = false;
        runtime.match_query_evaluated = false;
        runtime.match_evaluated_submission = false;
        runtime.match_helper_sites = NIL;
        runtime.stable_match_id = 0;
        runtime.mmsearch_null_input = false;
        frozen_scan_semantics frozen_semantics{};
        if (!decode_bioseq_semantic_payload(cscan->custom_private, &frozen_semantics)) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqScan semantic payload is malformed"),
                     errdetail("Expected semantic_payload format version %d with exactly %d "
                               "typed fields.",
                               k_bioseq_semantic_payload_format_version,
                               k_bioseq_semantic_payload_field_count)));
        }
        Node* base_match_target_recipe =
            initialize_base_match_owner_runtime(cscan, cscanstate->ss.ss_currentRelation, runtime);
        for (Node* private_node : postgres_list<Node>(cscan->custom_private)) {
            if (private_node == nullptr || !IsA(private_node, List))
                continue;
            List* tag = reinterpret_cast<List*>(private_node);
            if (list_length(tag) == 0 || !IsA(reinterpret_cast<Node*>(linitial(tag)), Integer))
                continue;
            const int magic = intVal(linitial(tag));
            if (magic == k_bioseq_locator_passthrough_magic) {
                if (list_length(tag) != 4 ||
                    intVal(list_nth(tag, 1)) != k_bioseq_batch_join_plan_version) {
                    ereport(ERROR,
                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                             errmsg("Bioseq locator passthrough plan tag is invalid")));
                }
                runtime.locator_passthrough = true;
                runtime.locator_parent_token = intVal(list_nth(tag, 2));
                runtime.locator_rowno_resno = static_cast<AttrNumber>(intVal(list_nth(tag, 3)));
            } else if (magic == k_bioseq_slice_source_magic) {
                Node* version_node =
                    list_length(tag) >= 2 ? reinterpret_cast<Node*>(list_nth(tag, 1)) : nullptr;
                Node* bindings_node =
                    list_length(tag) == 3 ? reinterpret_cast<Node*>(list_nth(tag, 2)) : nullptr;
                if (list_length(tag) != 3 || version_node == nullptr ||
                    !IsA(version_node, Integer) ||
                    intVal(version_node) != k_bioseq_slice_source_version ||
                    (bindings_node != nullptr && !IsA(bindings_node, List))) {
                    ereport(ERROR,
                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                             errmsg("Bioseq SliceProject source tag is invalid")));
                }
                runtime.lazy_source_projection = true;
                TupleDesc relation_descriptor = RelationGetDescr(cscanstate->ss.ss_currentRelation);
                const int relation_natts = relation_descriptor->natts;
                const int projected_natts = list_length(cscan->scan.plan.targetlist);
                runtime.lazy_source_projected_bindings.assign(
                    static_cast<std::size_t>(projected_natts + 1), {});
                runtime.lazy_source_physical_producer_ids.assign(
                    static_cast<std::size_t>(relation_natts + 1), 0u);
                runtime.lazy_source_physical_bound.assign(
                    static_cast<std::size_t>(relation_natts + 1), 0u);
                const Oid seq_oid = get_cached_stub_oids().bioseq_seq_type_oid;
                List* bindings =
                    bindings_node == nullptr ? NIL : reinterpret_cast<List*>(bindings_node);
                for (Node* raw_binding : postgres_list<Node>(bindings)) {
                    List* binding = raw_binding != nullptr && IsA(raw_binding, List)
                                        ? reinterpret_cast<List*>(raw_binding)
                                        : NIL;
                    if (list_length(binding) != 3 ||
                        !IsA(reinterpret_cast<Node*>(linitial(binding)), Integer) ||
                        !IsA(reinterpret_cast<Node*>(lsecond(binding)), Integer) ||
                        !IsA(reinterpret_cast<Node*>(lthird(binding)), Integer)) {
                        ereport(ERROR,
                                (errcode(ERRCODE_DATA_CORRUPTED),
                                 errmsg("Bioseq SliceProject source binding is invalid")));
                    }
                    const int resno = intVal(linitial(binding));
                    const int producer_id = intVal(lsecond(binding));
                    const int column_attno = intVal(lthird(binding));
                    TargetEntry* target_entry =
                        resno > 0 && resno <= projected_natts
                            ? try_cast_pg_node<TargetEntry>(reinterpret_cast<Node*>(
                                  list_nth(cscan->scan.plan.targetlist, resno - 1)))
                            : nullptr;
                    Form_pg_attribute relation_attribute =
                        column_attno > 0 && column_attno <= relation_natts
                            ? bioseq::postgres::support::tuple_descriptor_attribute(relation_descriptor, column_attno - 1)
                            : nullptr;
                    if (resno <= 0 || resno > projected_natts || producer_id < 0 ||
                        column_attno <= 0 || column_attno > relation_natts ||
                        target_entry == nullptr ||
                        exprType(reinterpret_cast<Node*>(target_entry->expr)) != seq_oid ||
                        relation_attribute == nullptr || relation_attribute->attisdropped ||
                        relation_attribute->atttypid != seq_oid ||
                        runtime.lazy_source_projected_bindings[resno].physical_attno !=
                            InvalidAttrNumber ||
                        runtime.lazy_source_physical_bound[column_attno] != 0u) {
                        ereport(ERROR,
                                (errcode(ERRCODE_DATA_CORRUPTED),
                                 errmsg("Bioseq SliceProject source binding is out of range")));
                    }
                    runtime.lazy_source_projected_bindings[resno] = {
                        .producer_id = static_cast<std::uint32_t>(producer_id),
                        .physical_attno = static_cast<AttrNumber>(column_attno),
                    };
                    runtime.lazy_source_physical_producer_ids[column_attno] =
                        static_cast<std::uint32_t>(producer_id);
                    runtime.lazy_source_physical_bound[column_attno] = 1u;
                }
            } else if (magic == k_bioseq_edit_predicate_magic) {
                Const* threshold =
                    list_length(tag) == 4
                        ? try_cast_pg_node<Const>(reinterpret_cast<Node*>(list_nth(tag, 2)))
                        : nullptr;
                Node* sequence_attno_node =
                    list_length(tag) == 4 ? reinterpret_cast<Node*>(list_nth(tag, 3)) : nullptr;
                if (list_length(tag) != 4 ||
                    intVal(list_nth(tag, 1)) != k_bioseq_edit_predicate_version ||
                    threshold == nullptr || threshold->constisnull ||
                    threshold->consttype != INT8OID || sequence_attno_node == nullptr ||
                    !IsA(sequence_attno_node, Integer) || intVal(sequence_attno_node) <= 0 ||
                    intVal(sequence_attno_node) > MaxAttrNumber) {
                    ereport(ERROR,
                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                             errmsg("Bioseq edit-predicate plan tag is invalid")));
                }
                runtime.edit_predicate_active = true;
                runtime.edit_exclusive_upper_bound = DatumGetInt64(threshold->constvalue);
                runtime.edit_sequence_attno = static_cast<AttrNumber>(intVal(sequence_attno_node));
            }
        }

        /* Keep parameter values and scan expressions in a context that survives
         * the per-tuple resets of the normal expression context. */
        ExecAssignExprContext(estate, &cscanstate->ss.ps);
        bioseq_scan_state->runtime_expression_context = cscanstate->ss.ps.ps_ExprContext;
        cscanstate->ss.ps.ps_ExprContext = stdecontext;

        ResetExprContext(bioseq_scan_state->runtime_expression_context);
        postgres_list<List> custom_exprs(cscan->custom_exprs);
        List* plain_clauses = custom_exprs.get_first();
        List* owned_mmsearch_quals = list_length(cscan->custom_exprs) >= 4
                                         ? reinterpret_cast<List*>(list_nth(cscan->custom_exprs, 3))
                                         : NIL;
        if (list_length(owned_mmsearch_quals) > 2) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("Bioseq scan plan owns too many mmsearch expressions")));
        }
        if ((base_match_target_recipe != nullptr) != (list_length(owned_mmsearch_quals) >= 1)) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqScan match owner expression identity is incomplete")));
        }
        List* storage_plain_clauses =
            storage_prunable_quals_without_mmsearch(plain_clauses, get_cached_stub_oids());
        bioseq_scan_state->qualifiers = bioseq::postgres::support::postgres_list<Node>{
            reinterpret_cast<List*>(eval_params(reinterpret_cast<Node*>(storage_plain_clauses),
                                                bioseq_scan_state->runtime_expression_context))};

        /* custom_exprs[2] holds the  optional vectorized row filters. */
        bioseq_log<logging_level::debug>("[bioseq] pg: BioseqColumnScan_BeginCustomScan() "
                                         "examining custom_exprs for vectorized qual");
        List* vectorized_qual_list = custom_exprs.get_third();
        if (vectorized_qual_list != NIL) {
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: BioseqColumnScan_BeginCustomScan() vectorized qual list present");
            bioseq_scan_state->vectorization.qualifiers =
                bioseq::postgres::support::postgres_list<Node>{vectorized_qual_list};
        }

        bioseq_log<logging_level::debug>(
            "[bioseq] pg: BioseqColumnScan_BeginCustomScan() iterating custom_private");
        for (auto private_node : postgres_list<Node>(cscan->custom_private)) {
            Const* private_const_node = try_cast_pg_node<Const>(private_node);
            if (private_const_node == nullptr) {
                continue;
            }
            if (private_const_node->consttype == k_vectorized_aggregate_private_marker) {
                bioseq_log<logging_level::debug>("[bioseq] pg: BioseqColumnScan_BeginCustomScan() "
                                                 "enabling aggregate vectorization");
                bioseq_scan_state->vectorization.is_aggregate = private_const_node->constvalue;
            }
        }
        bioseq_log<logging_level::debug>("[bioseq] pg: BioseqColumnScan_BeginCustomScan() "
                                         "custom_private iteration complete");

        /* Enable vectorized filtering or aggregation when requested, except
         * for scans that pass sequence locators or lazy values to, a parsnt. */
        bioseq_scan_state->vectorization.is_enabled =
            bioseq::settings::vectorization::enabled && !runtime.locator_passthrough &&
            !runtime.lazy_source_projection &&
            (!bioseq_scan_state->vectorization.qualifiers.is_empty() ||
             bioseq_scan_state->vectorization.is_aggregate);

        bioseq_log<logging_level::debug>("[bioseq] pg: BioseqColumnScan_BeginCustomScan() "
                                         "guc_vec={} has_vec_qual={} vec_aggregate={}",
                                         bioseq::settings::vectorization::enabled,
                                         !bioseq_scan_state->vectorization.qualifiers.is_empty(),
                                         bioseq_scan_state->vectorization.is_aggregate);
        if (bioseq_scan_state->vectorization.is_aggregate) {
            ScanState* node =
                reinterpret_cast<ScanState*>(&bioseq_scan_state->custom_scan_state.ss);

            if (node->ps.ps_ProjInfo) {
                bioseq_scan_state->vectorization.result_vector_slot =
                    create_vector_tuple_table_slot(
                        node->ps.ps_ProjInfo->pi_state.resultslot->tts_tupleDescriptor);
            } else {
                bioseq_scan_state->vectorization.result_vector_slot =
                    create_vector_tuple_table_slot(node->ps.ps_ResultTupleDesc);
            }
        }

        if (bioseq_scan_state->vectorization.is_enabled) {
            bioseq_scan_state->vectorization.scan_vector_slot = create_vector_tuple_table_slot(
                cscanstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor);
        }

        /* mmsearch lives in custom_exprs[3] because the scan executes it.
         * Its target and query columns are still required, even for count(*).
         * Include those expressions when finding columns to read, or the
         * reader may omit the sequence and report a false NULL. Copy list
         * cells only and leave the private expression out of SQL evaluation. */
        List* attr_needed_custom_expressions = list_concat_copy(
            bioseq_scan_state->vectorization.qualifiers.get(), owned_mmsearch_quals);
        bioseq_scan_state->attributes_needed = bioseq::postgres::support::postgres_bitmapset{
            collect_needed_column_attributes(&cscanstate->ss, attr_needed_custom_expressions)};
        list_free(attr_needed_custom_expressions);

        bioseq_log<logging_level::debug>("[bioseq] pg: BioseqColumnScan_BeginCustomScan() "
                                         "iterating needed-attribute bitmap members");
        for (int attr_index : bioseq_scan_state->attributes_needed) {
            bioseq_scan_state->vectorization.attributes_needed.append(attr_index);
        }

        /* Flush pending row-deletion masks and new stripes before reading.
         * A  parallel scan cannot perform these writes, so do this only
         * outside parallel mode and keep the resulting scan snapshot. */
        bioseq_scan_state->snapshot = estate->es_snapshot;
        bioseq_scan_state->owns_snapshot_registration = false;
        Oid relation_oid = cscanstate->ss.ss_currentRelation->rd_locator.relNumber;

        bioseq_log<logging_level::debug>(
            "[bioseq] pg: BioseqColumnScan_BeginCustomScan() checking parallel mode");
        if (!IsInParallelMode()) {
            flush_row_mask_write_state_for_relation_file(relation_oid,
                                                         GetCurrentSubTransactionId());

            flush_write_state_with_fresh_snapshot(relation_oid,
                                                  &bioseq_scan_state->snapshot,
                                                  &bioseq_scan_state->owns_snapshot_registration);
        }
        const bool owns_mmsearch_source = owned_mmsearch_quals != NIL;
        if (runtime.lazy_source_projection || runtime.locator_passthrough || owns_mmsearch_source) {
            runtime.lazy_source_provider = bioseq::storage::create_scan_pg_sequence_source_provider(
                cscanstate->ss.ss_currentRelation, bioseq_scan_state->snapshot);
            if (runtime.lazy_source_provider == nullptr) {
                ereport(ERROR,
                        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                         errmsg("Bioseq source provider is unavailable")));
            }
            runtime.buffer.bind_sequence_source_provider(runtime.lazy_source_provider);
        }

        /* Find an optional integer key argument in a direct match-helper
         * projection. The plan targetlist still holds the helper expressions
         * here, before execution builds its replacement projection */
        const bioseq_stub_oids& scan_stub_oids = get_cached_stub_oids();
        for (Node* executable_qual : postgres_list<Node>(cscan->scan.plan.qual)) {
            if (contains_mmsearch_qualifier(executable_qual, scan_stub_oids)) {
                FuncExpr* top_level = try_cast_pg_node<FuncExpr>(executable_qual);
                if (top_level == nullptr ||
                    !is_mmsearch_funcid(top_level->funcid, scan_stub_oids)) {
                    ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                             errmsg("nested mmsearch qualifier shape is unsupported"),
                             errdetail("mmsearch must be consumed by the alignment runtime; "
                                       "executing its SQL stub without alignment is forbidden"),
                             errhint("Place mmsearch(...) directly in a top-level AND conjunct.")));
                }
                ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("mmsearch qualifier is not owned by a Bioseq executor"),
                         errdetail("Nested, stacked, volatile, security-sensitive, or "
                                   "otherwise unsupported "
                                   "mmsearch shapes cannot execute through the SQL function."),
                         errhint("Use exactly one top-level mmsearch predicate with supported "
                                 "operands, or split the query into separate levels.")));
            }
        }
        AttrNumber primary_key_attno = InvalidAttrNumber;
        for (auto tgt : postgres_list<TargetEntry>(cscan->scan.plan.targetlist)) {
            if (tgt == nullptr)
                continue;
            FuncExpr* fe = try_cast_pg_node<FuncExpr>(to_pg_node(tgt->expr));
            if (fe == nullptr)
                continue;
            if (!is_bioseq_projection_funcid(fe->funcid, scan_stub_oids))
                continue;
            if (list_length(fe->args) < 1)
                continue;
            postgres_list<Node> fe_args(fe->args);
            Node* arg0 = fe_args.get_first();
            Var* pk_var = try_cast_pg_node<Var>(arg0);
            if (pk_var == nullptr)
                continue;
            if (pk_var->varattno <= 0)
                continue;
            /* Helpers such as edit(seq) and cigar(seq) read the current match
             * result. Capture an optional key only for an INT4 argument. */
            if (pk_var->vartype != INT4OID)
                continue;
            primary_key_attno = pk_var->varattno;
            bioseq_log<logging_level::debug>("[bioseq] pg: BeginCustomScan() detected primary_key "
                                             "varattno={} from bioseq projection funcid={}",
                                             static_cast<int>(primary_key_attno),
                                             fe->funcid);
            break;
        }

        /* Keep query-context cleanup registered  before configuring the buffer.
         * Registration is idempotent and also covers PostgreSQL errors. */
        register_runtime_cleanup_callback(bioseq_scan_state, estate);

        /* Configure alignmeny from the planner-owned custom_exprs entries. */
        /* Begin rebuilds all search inputs, including constant extras.
         * A NULL parameter on ReScan clears the primary query while keeping
         * those extras for the next non-NULL parameter value. */
        runtime.buffer.clear_mmsearch_context(/*clear_extras=*/true);
        runtime.param_rebind = mmsearch_parameter_rebind{};

        Relation relation_for_attname = cscanstate->ss.ss_currentRelation;
        const Oid relation_oid_for_attname = RelationGetRelid(relation_for_attname);

        /* Track the single primary search. The planner can also admit one
         * constant filter on the same target with a parameterized primary. */
        bool primary_configured = false;
        AttrNumber primary_seq_attno = InvalidAttrNumber;
        FuncExpr* primary_mmsearch_fe = nullptr;

        /* Row-dependent and, parameter queries need the scan's single primary
         * slot. Classify the  planner-owned searches first so an optional
         * constant filter comes afterwards regardless of clause order.
         * Every admitted search must be consumed here. Reject unsupported
         * combinations before an mmsearch SQL stub could run. */
        // Records whether this search uses a constant, parameter, column, or row expression.
        struct classified_mmsearch_qualifier {
            FuncExpr* fe;
            Node* arg0;
            Node* arg1;
            bool is_const;
            bool is_param;
            bool is_var_same_rel;
            bool is_evaluated_row_query;
        };
        std::vector<classified_mmsearch_qualifier> classified_quals;
        std::size_t top_level_mmsearch_qual_count = 0;
        for (auto qual_node : postgres_list<Node>(owned_mmsearch_quals)) {
            FuncExpr* fe_classify = try_cast_pg_node<FuncExpr>(qual_node);
            if (fe_classify == nullptr ||
                !is_mmsearch_funcid(fe_classify->funcid, scan_stub_oids)) {
                /* The alignment runtime accepts a top-level mmsearch call. A wrapper
                 * such as NOT or CASE would leave the search unconfigured. Reject it
                 * here because the SQL stub only marks work  for the custom execumor */
                if (contains_mmsearch_qualifier(qual_node, scan_stub_oids)) {
                    ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                             errmsg("nested mmsearch qualifier shape is unsupported"),
                             errdetail("mmsearch must be consumed by the alignment runtime; "
                                       "executing its SQL stub without alignment is forbidden"),
                             errhint("Place mmsearch(...) directly in a top-level AND conjunct.")));
                }
                continue;
            }
            ++top_level_mmsearch_qual_count;
            /* Check constant options now, even if this search will later be
             * rejected for its primary or extra-query shape. */
            (void)require_aligner_options_const(fe_classify, scan_stub_oids);
            pg_first_two_node_arguments classify_args =
                get_pg_first_two_arguments_if_arity_between(*fe_classify, 2, 3);
            if (!classify_args.is_present()) {
                continue;
            }
            Node* a0 = classify_args.first_argument;
            Node* a1 = classify_args.second_argument;
            if (base_match_target_recipe == nullptr || !equal(a0, base_match_target_recipe)) {
                continue;
            }
            const bool is_c = pg_node_view{a1}.is_type<Const>();
            const bool is_p = pg_node_view{a1}.is_type<Param>();
            Var* a1_var = try_cast_pg_node<Var>(a1);
            const bool is_v =
                a1_var != nullptr && a1_var->varno == cscan->scan.scanrelid && a1_var->varattno > 0;
            const bool is_evaluated_query = !is_c && !is_p && !is_v && a1 != nullptr &&
                                            exprType(a1) == scan_stub_oids.bioseq_seq_type_oid;
            if (!is_c && !is_p && !is_v && !is_evaluated_query) {
                bioseq_log<logging_level::debug>("[bioseq] pg: BeginCustomScan() classifier "
                                                 "skipping mmsearch (arg1 tag={} is "
                                                 "not Const/Param/same-rel-Var)",
                                                 get_pg_node_tag_name(get_pg_node_tag(a1)));
                continue;
            }
            classified_quals.push_back({fe_classify, a0, a1, is_c, is_p, is_v, is_evaluated_query});
        }
        /* Put row expressions first, then parameters, then constants. Keep
         * planner order among equal priorities. Only one search can use the
         * primary slot, and any additional search must pass the extra-query checks. */
        std::stable_sort(
            classified_quals.begin(),
            classified_quals.end(),
            [](const classified_mmsearch_qualifier& lhs, const classified_mmsearch_qualifier& rhs) {
                // Gives row queries and parameters priority over optional constant filters.
                auto pri = [](const classified_mmsearch_qualifier& q) {
                    if (q.is_var_same_rel)
                        return 0;
                    if (q.is_evaluated_row_query)
                        return 0;
                    if (q.is_param)
                        return 1;
                    return 2; /* Const */
                };
                return pri(lhs) < pri(rhs);
            });

        std::size_t consumed_mmsearch_qual_count = 0;
        for (auto& classified : classified_quals) {
            FuncExpr* fe = classified.fe;
            Node* arg0 = classified.arg0;
            Node* arg1 = classified.arg1;
            const bool arg1_is_const = classified.is_const;
            const bool arg1_is_param = classified.is_param;
            const bool arg1_is_var_same_rel = classified.is_var_same_rel;
            const bool arg1_is_evaluated_row = classified.is_evaluated_row_query;
            Var* col_var = runtime.match_target_lineage == NIL
                               ? nullptr
                               : reinterpret_cast<Var*>(linitial(runtime.match_target_lineage));
            if (col_var == nullptr || arg0 == nullptr || !equal(arg0, runtime.match_target_recipe))
                continue;

            if (arg1_is_const) {
                Const* query_const = reinterpret_cast<Const*>(arg1);
                if (query_const->consttype != scan_stub_oids.bioseq_seq_type_oid) {
                    bioseq_log<logging_level::debug>("[bioseq] pg: BeginCustomScan() mmsearch arg1 "
                                                     "is not a bioseq.seq Const (type={}, null={})",
                                                     query_const->consttype,
                                                     query_const->constisnull);
                    continue;
                }
            } else if (arg1_is_param) {
                Param* query_param = reinterpret_cast<Param*>(arg1);
                if (query_param->paramtype != scan_stub_oids.bioseq_seq_type_oid) {
                    bioseq_log<logging_level::debug>(
                        "[bioseq] pg: BeginCustomScan() mmsearch arg1 Param has paramtype={}, "
                        "expected bioseq.seq ({})",
                        query_param->paramtype,
                        scan_stub_oids.bioseq_seq_type_oid);
                    continue;
                }
                /* Nested loops supply PARAM_EXEC values. PREPARE or the client
                 * supplies PARAM_EXTERN values. Read both through PostgreSQL's
                 * expression context. */
                if (query_param->paramkind != PARAM_EXEC &&
                    query_param->paramkind != PARAM_EXTERN) {
                    bioseq_log<logging_level::debug>("[bioseq] pg: BeginCustomScan() mmsearch arg1 "
                                                     "Param has unsupported paramkind={}",
                                                     static_cast<int>(query_param->paramkind));
                    continue;
                }
            } else if (arg1_is_var_same_rel) {
                Var* query_var = reinterpret_cast<Var*>(arg1);
                if (query_var->vartype != scan_stub_oids.bioseq_seq_type_oid) {
                    bioseq_log<logging_level::debug>("[bioseq] pg: BeginCustomScan() mmsearch arg1 "
                                                     "Var vartype={} not bioseq.seq",
                                                     query_var->vartype);
                    continue;
                }
            } else if (!arg1_is_evaluated_row) {
                continue;
            }

            TupleDesc tuple_descriptor = RelationGetDescr(relation_for_attname);
            if (col_var->varattno <= 0 || col_var->varattno > tuple_descriptor->natts) {
                bioseq_log<logging_level::debug>("[bioseq] pg: BeginCustomScan() mmsearch arg0 "
                                                 "varattno={} out of range natts={}",
                                                 static_cast<int>(col_var->varattno),
                                                 tuple_descriptor->natts);
                continue;
            }
            Form_pg_attribute attribute_form =
                bioseq::postgres::support::tuple_descriptor_attribute(tuple_descriptor, col_var->varattno - 1);
            if (attribute_form->atttypid != scan_stub_oids.bioseq_seq_type_oid) {
                bioseq_log<logging_level::debug>("[bioseq] pg: BeginCustomScan() mmsearch arg0 "
                                                 "column type is {}, expected bioseq.seq ({})",
                                                 attribute_form->atttypid,
                                                 scan_stub_oids.bioseq_seq_type_oid);
                continue;
            }

            /* The query and target  must use the same alphabet. A different
             * storage width can be repacked to the target type. Reject a known
             * alphabet mismatch before reading or dispatching the sequence. */
            if (runtime.match_target_typmod < 0) {
                bioseq_log<logging_level::debug>(
                    "[bioseq] pg: BeginCustomScan() mmsearch arg0 has unconstrained typmod; "
                    "bioseq.seq columns require an explicit (alphabet,width)");
                continue;
            }
            int32 arg1_typmod = -1;
            if (arg1_is_const) {
                arg1_typmod = reinterpret_cast<Const*>(arg1)->consttypmod;
            } else if (arg1_is_param) {
                arg1_typmod = reinterpret_cast<Param*>(arg1)->paramtypmod;
            } else if (arg1_is_var_same_rel) {
                arg1_typmod = reinterpret_cast<Var*>(arg1)->vartypmod;
            } else {
                const base_local_sequence_typmod_resolution query_type =
                    resolve_base_local_sequence_typmod(arg1, scan_stub_oids);
                if (query_type.invalid || query_type.typmod < 0)
                    continue;
                arg1_typmod = query_type.typmod;
            }
            const bool runtime_typed_external_param =
                arg1_is_param && arg1_typmod < 0 &&
                reinterpret_cast<Param*>(arg1)->paramkind == PARAM_EXTERN;
            if (arg1_typmod < 0 && !runtime_typed_external_param) {
                bioseq_log<logging_level::debug>(
                    "[bioseq] pg: BeginCustomScan() mmsearch arg1 has unconstrained typmod; "
                    "add "
                    "::bioseq.seq('dna4', N) to the literal/param");
                continue;
            }
            const auto unpacked_typmod =
                compact_sequence_stdc_type_params::decode(runtime.match_target_typmod);
            /* PREPARE can leave paramtypmod at -1 even for a typed sequence.
             * resolve_mmsearch_param_query() checks the type stored in the value
             * when, binding the parameter at Begin or ReScan. */
            const auto unpacked_arg1_typmod =
                runtime_typed_external_param
                    ? unpacked_typmod
                    : compact_sequence_stdc_type_params::decode(arg1_typmod);
            const bool same_alphabet =
                unpacked_typmod.alphabet_code == unpacked_arg1_typmod.alphabet_code;
            if (!runtime_typed_external_param && !same_alphabet) {
                raise_mmsearch_alphabet_mismatch(
                    unpacked_typmod, unpacked_arg1_typmod, "column scan begin");
            }

            /* Log an unexpected type for the optional integer key. Row identity
             * inside a batch comes from the buffer entry index, so  this check
             * does not discard rows. */
            if (primary_key_attno != InvalidAttrNumber &&
                primary_key_attno <= tuple_descriptor->natts) {
                Form_pg_attribute pk_attr_form =
                    bioseq::postgres::support::tuple_descriptor_attribute(tuple_descriptor, primary_key_attno - 1);
                if (pk_attr_form->atttypid != INT4OID) {
                    bioseq_log<logging_level::debug>("[bioseq] pg: BeginCustomScan() primary "
                                                     "key column type is {} not INT4OID; "
                                                     "rows will be mis-keyed",
                                                     pk_attr_form->atttypid);
                }
            }

            char* attname_cstr =
                get_attname(relation_oid_for_attname, col_var->varattno, /*missing_ok=*/true);
            if (attname_cstr == nullptr) {
                bioseq_log<logging_level::debug>(
                    "[bioseq] pg: BeginCustomScan() get_attname returned NULL for attno={}",
                    static_cast<int>(col_var->varattno));
                continue;
            }

            if (primary_configured) {
                if (!arg1_is_const || col_var->varattno != primary_seq_attno) {
                    ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                             errmsg("stacked mmsearch qualifiers are unsupported"),
                             errdetail("Only a constant same-target filter can accompany a "
                                       "parameterized primary mmsearch.")));
                }
                Const* query_const = reinterpret_cast<Const*>(arg1);
                if (query_const->constisnull || !bounded_batch_fallback_options_are_supported(
                                                    primary_mmsearch_fe, fe, scan_stub_oids)) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("Bioseq scan bounded mmsearch stack is invalid"),
                             errdetail(
                                 "The constant extra must be non-NULL, use default biological "
                                 "options, and accompany an explicitly require-absent primary.")));
                } else {
                    bioseq::pg_types::seq* query_hdr = reinterpret_cast<bioseq::pg_types::seq*>(
                        PG_DETOAST_DATUM(query_const->constvalue));
                    const auto physical_query_type = query_hdr->type_parameters();
                    if (physical_query_type.alphabet_code != unpacked_typmod.alphabet_code) {
                        const auto mismatch = physical_query_type;
                        if (reinterpret_cast<void*>(query_hdr) !=
                            reinterpret_cast<void*>(DatumGetPointer(query_const->constvalue)))
                            pfree(query_hdr);
                        raise_mmsearch_alphabet_mismatch(
                            unpacked_typmod, mismatch, "constant extra binding");
                    }
                    bioseq::pg_types::seq* normalized_query = query_hdr;
                    if (physical_query_type != unpacked_typmod) {
                        normalized_query =
                            ::bioseq::simd::active_target::stdc_rewidth_to_type_params(
                                *query_hdr, unpacked_typmod, pg_alloc);
                        if (normalized_query == nullptr) {
                            if (reinterpret_cast<void*>(query_hdr) !=
                                reinterpret_cast<void*>(DatumGetPointer(query_const->constvalue)))
                                pfree(query_hdr);
                            ereport(ERROR,
                                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                                     errmsg("bioseq mmsearch constant extra rewidth rejected an "
                                            "incompatible alphabet")));
                        }
                    }
                    const size_t query_total_bytes = static_cast<size_t>(bioseq::postgres::support::varlena_4byte_size(normalized_query));
                    std::vector<char> query_varlena_bytes(query_total_bytes);
                    std::memcpy(query_varlena_bytes.data(), normalized_query, query_total_bytes);
                    if (normalized_query != query_hdr)
                        pfree(normalized_query);
                    if (reinterpret_cast<void*>(query_hdr) !=
                        reinterpret_cast<void*>(DatumGetPointer(query_const->constvalue)))
                        pfree(query_hdr);
                    runtime.buffer.add_extra_const_query(std::move(query_varlena_bytes));
                }
                ++consumed_mmsearch_qual_count;
                pfree(attname_cstr);
                continue;
            }

            if (arg1_is_const) {
                /* Keep an owned copy of the constant quer in the dispatch buffer
                 * for reuse across rows. Detoast it here so row submission does not
                 * repeatedly fetch the same query value */
                Const* query_const = reinterpret_cast<Const*>(arg1);
                if (query_const->constisnull) {
                    runtime.mmsearch_null_input = true;
                } else {
                    bioseq::pg_types::seq* query_hdr = reinterpret_cast<bioseq::pg_types::seq*>(
                        PG_DETOAST_DATUM(query_const->constvalue));
                    const auto physical_query_type = query_hdr->type_parameters();
                    if (physical_query_type.alphabet_code != unpacked_typmod.alphabet_code) {
                        const auto mismatch = physical_query_type;
                        if (reinterpret_cast<void*>(query_hdr) !=
                            reinterpret_cast<void*>(DatumGetPointer(query_const->constvalue))) {
                            pfree(query_hdr);
                        }
                        raise_mmsearch_alphabet_mismatch(
                            unpacked_typmod, mismatch, "literal binding");
                    }
                    const size_t query_total_bytes = static_cast<size_t>(bioseq::postgres::support::varlena_4byte_size(query_hdr));
                    std::vector<char> query_varlena_bytes(query_total_bytes);
                    std::memcpy(query_varlena_bytes.data(), query_hdr, query_total_bytes);
                    if (reinterpret_cast<void*>(query_hdr) !=
                        reinterpret_cast<void*>(DatumGetPointer(query_const->constvalue))) {
                        pfree(query_hdr);
                    }

                    runtime.buffer.configure_mmsearch(col_var->varattno,
                                                      std::string(attname_cstr),
                                                      std::move(query_varlena_bytes),
                                                      unpacked_typmod.alphabet_code,
                                                      unpacked_typmod.storage_unit_byte_width,
                                                      primary_key_attno);
                    /* Repacking changes storage width while preserving the letters, so
                     * the target's saved minimizer index remains compatible. */
                    if (physical_query_type != unpacked_typmod) {
                        /* Fetch the literal again because the first detoasted copy was
                         * released. Repack it to the target width and keep the replacement
                         * bytes in the buffer through apply_typmod_coalesce(). */
                        bioseq::pg_types::seq* src_hdr2 = reinterpret_cast<bioseq::pg_types::seq*>(
                            PG_DETOAST_DATUM(query_const->constvalue));
                        bioseq::pg_types::seq* rewidthed =
                            ::bioseq::simd::active_target::stdc_rewidth_to_type_params(
                                *src_hdr2, unpacked_typmod, pg_alloc);
                        if (reinterpret_cast<void*>(src_hdr2) !=
                            reinterpret_cast<void*>(DatumGetPointer(query_const->constvalue))) {
                            pfree(src_hdr2);
                        }
                        if (rewidthed == nullptr) {
                            ereport(ERROR,
                                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                                     errmsg("bioseq mmsearch literal rewidth rejected an "
                                            "incompatible alphabet")));
                        }
                        if (rewidthed != nullptr) {
                            const size_t rewidthed_bytes = static_cast<size_t>(bioseq::postgres::support::varlena_4byte_size(rewidthed));
                            std::vector<char> coalesced_bytes(rewidthed_bytes);
                            std::memcpy(coalesced_bytes.data(), rewidthed, rewidthed_bytes);
                            pfree(rewidthed);
                            runtime.buffer.apply_typmod_coalesce(std::move(coalesced_bytes));
                        }
                    }
                }
            } else if (arg1_is_param) {
                /* Save  the parameter identity for later rebinding. PARAM_EXTERN is
                 * available now. A nested loop installs PARAM_EXEC before calling
                 * ReScan on this scan. */
                Param* query_param = reinterpret_cast<Param*>(arg1);
                runtime.param_rebind.active = true;
                runtime.param_rebind.paramid = query_param->paramid;
                runtime.param_rebind.paramkind = static_cast<int>(query_param->paramkind);
                runtime.param_rebind.param_typoid = query_param->paramtype;
                runtime.param_rebind.param_typmod = query_param->paramtypmod;
                runtime.param_rebind.seq_attno = col_var->varattno;
                runtime.param_rebind.column_name = std::string(attname_cstr);
                runtime.param_rebind.alphabet_code = unpacked_typmod.alphabet_code;
                runtime.param_rebind.storage_unit_byte_width =
                    unpacked_typmod.storage_unit_byte_width;
                runtime.param_rebind.primary_key_attno = primary_key_attno;
                runtime.param_rebind.mmsearch_fe = fe;
                if (query_param->paramkind == PARAM_EXTERN) {
                    (void)resolve_mmsearch_param_query(
                        runtime, cscanstate->ss.ps.ps_ExprContext, "BeginCustomScan");
                }
                bioseq_log<logging_level::debug>(
                    "[bioseq] pg: BeginCustomScan() registering param-driven mmsearch rebind "
                    "paramid={} paramkind={} typmod=({},{})",
                    query_param->paramid,
                    static_cast<int>(query_param->paramkind),
                    static_cast<unsigned>(unpacked_typmod.alphabet_code),
                    static_cast<unsigned>(unpacked_typmod.storage_unit_byte_width));
            } else /* Query column or evaluated expression from the same row. */
            {
                const bool evaluated_submission =
                    runtime.match_target_evaluated || arg1_is_evaluated_row;
                if (evaluated_submission) {
                    runtime.match_evaluated_submission = true;
                    runtime.match_query_evaluated = true;
                    runtime.match_query_recipe = arg1;
                    runtime.match_query_lineage =
                        canonical_match_base_lineage(arg1, scan_stub_oids);
                    runtime.match_query_typmod = arg1_typmod;
                    runtime.buffer.configure_mmsearch_per_row_evaluated(
                        col_var->varattno,
                        std::string(attname_cstr),
                        unpacked_typmod.alphabet_code,
                        unpacked_typmod.storage_unit_byte_width,
                        unpacked_arg1_typmod.alphabet_code,
                        unpacked_arg1_typmod.storage_unit_byte_width,
                        primary_key_attno);
                } else {
                    Var* query_var = reinterpret_cast<Var*>(arg1);
                    const bool self_alignment = query_var->varattno == col_var->varattno;
                    runtime.buffer.configure_mmsearch_per_row(
                        col_var->varattno,
                        std::string(attname_cstr),
                        query_var->varattno,
                        self_alignment,
                        unpacked_typmod.alphabet_code,
                        unpacked_typmod.storage_unit_byte_width,
                        unpacked_arg1_typmod.alphabet_code,
                        unpacked_arg1_typmod.storage_unit_byte_width,
                        primary_key_attno);
                    bioseq_log<logging_level::debug>(
                        "[bioseq] pg: BeginCustomScan() registering per-row mmsearch "
                        "col_attno={} "
                        "arg1_attno={} self_alignment={}",
                        static_cast<int>(col_var->varattno),
                        static_cast<int>(query_var->varattno),
                        self_alignment);
                }
            }
            if (runtime.match_target_evaluated) {
                runtime.match_evaluated_submission = true;
            }

            primary_configured = true;
            ++consumed_mmsearch_qual_count;
            primary_mmsearch_fe = fe;
            primary_seq_attno = col_var->varattno;
            runtime.match_sequence_varno = col_var->varno;
            runtime.match_sequence_attno = col_var->varattno;

            pfree(attname_cstr);
            /* Continue so the next owned call can be checked as the optional
             * constant filter on this same target. */
        }
        if (consumed_mmsearch_qual_count != top_level_mmsearch_qual_count &&
            (eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0) {
            ereport(
                ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("mmsearch qualifier shape is unsupported"),
                 errdetail("the alignment runtime consumed %llu of %llu top-level mmsearch "
                           "qualifiers; executing any remainder through the SQL stub is forbidden",
                           static_cast<unsigned long long>(consumed_mmsearch_qual_count),
                           static_cast<unsigned long long>(top_level_mmsearch_qual_count)),
                 errhint("Use a bioseq.seq column as the first argument and a non-null Const, "
                         "PARAM_EXEC, PARAM_EXTERN, or same-relation bioseq.seq Var as the second "
                         "argument.")));
        }
        Node* configured_match_target =
            primary_mmsearch_fe == nullptr || list_length(primary_mmsearch_fe->args) < 1
                ? nullptr
                : reinterpret_cast<Node*>(linitial(primary_mmsearch_fe->args));
        if (base_match_target_recipe != nullptr &&
            (configured_match_target == nullptr ||
             !equal(configured_match_target, base_match_target_recipe) ||
             runtime.match_target_lineage == NIL)) {
            ereport(
                ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("BioseqScan configured match target disagrees with its finalized owner")));
        }
        if (runtime.match_evaluated_submission) {
            /* Initialize the operand expressions after setrefs. The enclosing
             * mmsearch call is metadata and must never become an ExprState. */
            runtime.match_target_recipe = configured_match_target;
            runtime.match_target_expr_state =
                ExecInitExpr(reinterpret_cast<Expr*>(configured_match_target), &cscanstate->ss.ps);
            if (runtime.match_query_evaluated) {
                if (runtime.match_query_recipe == nullptr) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("BioseqScan evaluated query recipe is missing")));
                }
                runtime.match_query_expr_state = ExecInitExpr(
                    reinterpret_cast<Expr*>(runtime.match_query_recipe), &cscanstate->ss.ps);
            }
        }
        if (runtime.edit_predicate_active &&
            (runtime.match_target_evaluated || primary_seq_attno == InvalidAttrNumber ||
             runtime.edit_sequence_attno != primary_seq_attno)) {
            ereport(
                ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bioseq.edit predicate must reference the mmsearch target column"),
                 errdetail("bioseq.edit attribute %d does not match mmsearch target attribute %d",
                           static_cast<int>(runtime.edit_sequence_attno),
                           static_cast<int>(primary_seq_attno))));
        }

        /* Build one execution plan after resolving call options, index policy,
         * sequence type, and requested match fields. A scan without mmsearch
         * does  not need an alignment plan. */
        scan_feature_flags features = detect_feature_flags(
            cscan->scan.plan.targetlist, cscan->scan.plan.qual, scan_stub_oids);
        features.has_mmsearch = owned_mmsearch_quals != NIL;
        features.uses_edit = features.uses_edit || runtime.edit_predicate_active;
        features.any_match_projection =
            features.any_match_projection || runtime.edit_predicate_active;
        if (features.uses_edit && !features.has_mmsearch) {
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("bioseq.edit requires mmsearch in the same scan")));
        }
        if (features.has_mmsearch && IsParallelWorker()) {
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("bioseq alignment custom scans are parallel unsafe"),
                     errdetail("statement execution attestation is backend-local and DSM "
                               "aggregation is not implemented")));
        }
        bioseq::simd::minimap2_parameters pending_parameters =
            features.has_mmsearch ? build_params_from_gucs()
                                  : bioseq::simd::minimap2_parameters{};
        auto index_read_requirement = bioseq::indexing::index_read_requirement::allow;
        bool primary_vulkan_enabled_explicit = false;
        if (features.has_mmsearch) {
            (void)apply_aligner_options_override(primary_mmsearch_fe,
                                                 pending_parameters,
                                                 index_read_requirement,
                                                 scan_stub_oids,
                                                 &primary_vulkan_enabled_explicit);
            finalize_simd_chaining_policy(pending_parameters);
        }
        const bool executor_read_sequence_index =
            bioseq::settings::index::sequence_read::enabled;
        if (features.has_mmsearch) {
            if (runtime.match_target_typmod < 0) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqScan mmsearch target typmod is missing")));
            }
            const auto match_target_type =
                compact_sequence_stdc_type_params::decode(runtime.match_target_typmod);
            if (!is_supported_alphabet_code(match_target_type.alphabet_code)) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqScan mmsearch target alphabet is unsupported"),
                         errdetail("alphabet_code=%u",
                                   static_cast<unsigned>(match_target_type.alphabet_code))));
            }
            pending_parameters.alphabet_code = match_target_type.alphabet_code;
        }
        if (runtime.match_evaluated_submission) {
            if (index_read_requirement ==
                bioseq::indexing::index_read_requirement::require_compatible_persisted) {
                ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("transformed mmsearch target cannot require a persisted index")));
            }
            index_read_requirement = bioseq::indexing::index_read_requirement::require_absent;
        }
        if (features.has_mmsearch) {
            const int plan_node_id = cscan->scan.plan.plan_node_id;
            if (plan_node_id < 0) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq alignment scan has an invalid plan node id")));
            }
            runtime.alignment_registration =
                bioseq::postgres::table::alignment_execution::bind_alignment_node(
                    estate, static_cast<std::uint32_t>(plan_node_id));
        }
        pending_parameters.downstream_active_wall = features.has_mmsearch
            ? bioseq::postgres::table::alignment_execution::
                borrow_downstream_active_wall_tracker(runtime.alignment_registration) : nullptr;
        pending_parameters.do_cigar = features.uses_cigar || features.uses_edit;
        pending_parameters.compute_divergence = features.uses_divergence;
        pending_parameters.compute_mapq = features.uses_mapq;
        pending_parameters.compute_edit_distance = features.uses_edit;
        pending_parameters.exact_edit_filter_enabled = runtime.edit_predicate_active;
        pending_parameters.exact_edit_filter_exclusive_upper_bound = runtime.edit_exclusive_upper_bound;
        if (runtime.edit_predicate_active && runtime.edit_exclusive_upper_bound > 0 &&
            static_cast<std::uint64_t>(runtime.edit_exclusive_upper_bound - 1) <=
                (std::numeric_limits<std::uint32_t>::max)()) {
            pending_parameters.prefilter_enabled = true;
            pending_parameters.prefilter_max_edits =
                static_cast<std::uint32_t>(runtime.edit_exclusive_upper_bound - 1);
            const bool existing_vulkan_route =
                pending_parameters.chaining_algorithm ==
                    bioseq::simd::chaining_algorithm::vulkan_tiled_dynamic_programming ||
                pending_parameters.minimizer_algorithm ==
                    bioseq::simd::minimizer_algorithm::vulkan_minimap_integer ||
                pending_parameters.vulkan_seed_hits_enabled ||
                pending_parameters.vulkan_fused_frontend_enabled ||
                pending_parameters.vulkan_gpu_index_build_enabled ||
                pending_parameters.vulkan_extend_enabled;
            const bool standalone_vulkan_prefilter_requested =
                primary_vulkan_enabled_explicit && pending_parameters.vulkan_enabled;
            pending_parameters.prefilter_backend =
                (existing_vulkan_route || standalone_vulkan_prefilter_requested)
                    ? bioseq::prefilter::prefilter_backend::vulkan
                    : (bioseq::settings::vectorization::enabled
                           ? bioseq::prefilter::prefilter_backend::simd
                           : bioseq::prefilter::prefilter_backend::scalar);
        }
        if (features.has_mmsearch) {
            runtime.aligner_state.execution_plan =
                bioseq::scheduler::make_postgres_statement_execution_plan(std::move(pending_parameters));
        }
        const auto& aligner_params = runtime.aligner_state.execution_plan
            ? runtime.aligner_state.execution_plan->frozen_parameters : pending_parameters;
        const bool selected_vulkan =
            bioseq::simd::pipe::vulkan_components_requested(aligner_params);
        const bool selected_vulkan_accelerator =
            bioseq::simd::pipe::vulkan_accelerator_requested(aligner_params);
        const bool executable_scan = (eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0;
        /* Ordinary BioseqScan cannot hold the READY index generation
         * required by require_compatible_persisted. Reject that policy before
         * starting Vulkan. Recomputed seeds and cached results cannat, satisfy
         * it. Plain EXPLAIN can still show the policy without running a search. */
        if (index_read_requirement ==
                bioseq::indexing::index_read_requirement::require_compatible_persisted &&
            executable_scan) {
            ereport(
                ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("required compatible persisted minimizer index unavailable"),
                 errdetail("ordinal 2 requires a READY generation with the current "
                           "head-and-manifest identity retained by the BatchJoin prepared-reader "
                           "lifecycle; ordinary column scans have no compatible persisted reader"),
                 errhint("Use BioseqBatchJoin with a compatible READY generation, or choose "
                         "index_read_requirement=allow/require_absent; recompute and cache "
                         "fallback are forbidden.")));
        }
        /* Plain EXPLAIN describes the chosen pipeline without starting Vulkan
         * or changing its Metal policy. SELECT and EXPLAIN ANALYZE must pass
         * the runtime enablement and hardware-device checks below */
        const bool executable_vulkan =
            features.has_mmsearch && selected_vulkan_accelerator && executable_scan;
        if (executable_vulkan && !aligner_params.vulkan_enabled) {
            raise_column_scan_vulkan_error_as_cpp(
                "selected Vulkan execution requires "
                "bioseq.aligner.vulkan.enabled=on",
                "Set bioseq.aligner.vulkan.enabled=on, remove the per-call "
                "standalone Vulkan prefilter request, or select non-Vulkan "
                "minimizer/chaining algorithms before dispatch.");
        }
        if (executable_vulkan && aligner_params.vulkan_enabled) {
            auto& vulkan_runtime = bioseq::postgres::scheduler::ensure_pg_vulkan_runtime_ready();
            if (!vulkan_runtime.is_usable()) {
                const std::string diagnostic =
                    fmt::format("selected Vulkan execution requires a usable Vulkan runtime; "
                                "initialization_error={}",
                                vulkan_runtime.initialization_error());
                raise_column_scan_vulkan_error_as_cpp(
                    diagnostic,
                    "Install a working Vulkan ICD, remove or set false the per-call "
                    "aligner_options(vulkan_enabled) standalone prefilter request, "
                    "or select non-Vulkan minimizer/chaining algorithms before dispatch.");
            }
            if (vulkan_runtime.features().software_device) {
                const std::string diagnostic =
                    fmt::format("selected Vulkan execution requires a hardware GPU; active "
                                "Vulkan device '{}' is a software/CPU Vulkan device "
                                "(VkPhysicalDeviceType={})",
                                vulkan_runtime.features().device_name.data(),
                                vulkan_runtime.features().device_type);
                raise_column_scan_vulkan_error_as_cpp(
                    diagnostic,
                    "Use a hardware Vulkan ICD, remove or set false the per-call "
                    "aligner_options(vulkan_enabled) standalone prefilter request, "
                    "or select non-Vulkan minimizer/chaining algorithms before dispatch.");
            }
        }

        runtime.aligner_state.features = features;
        runtime.aligner_state.index_read_requirement = index_read_requirement;
        runtime.aligner_state.read_sequence_index = executor_read_sequence_index;

        /* Describe the selected pipeline for EXPLAIN using the execution  plan
         * and target alphabet. Stage descriptions do not run alignment work. */
        if (features.has_mmsearch) {
            runtime.aligner_state.pipeline_desc = describe_pipeline_for_pg_alphabet(
                runtime.aligner_state.execution_plan,
                aligner_params.alphabet_code,
                query_sketch_plan_route_for(index_read_requirement));
        }

        /* Enable saved-index access only for an mmsearch scan. Scans without
         * mmsearch do not need to open the sequence index or report its strategy. */
        const Oid resolved_user_relid = cscanstate->ss.ss_currentRelation != nullptr
                                            ? RelationGetRelid(cscanstate->ss.ss_currentRelation)
                                            : InvalidOid;
        /* Supply database, tablespace, and relation-file ids so the buffer
         * can find the sequence fragments belonging to this table. */
        Oid scan_db_oid = InvalidOid;
        Oid scan_spc_oid = InvalidOid;
        Oid scan_relfile = InvalidOid;
        if (cscanstate->ss.ss_currentRelation != nullptr) {
            scan_db_oid = cscanstate->ss.ss_currentRelation->rd_locator.dbOid;
            scan_spc_oid = cscanstate->ss.ss_currentRelation->rd_locator.spcOid;
            scan_relfile = cscanstate->ss.ss_currentRelation->rd_locator.relNumber;
        }
        runtime.buffer.configure_params(runtime.aligner_state.execution_plan,
                                        index_read_requirement,
                                        features.has_mmsearch &&
                                            executor_read_sequence_index &&
                                            !runtime.match_evaluated_submission,
                                        resolved_user_relid,
                                        scan_db_oid,
                                        scan_spc_oid,
                                        scan_relfile,
                                        cscanstate->ss.ss_currentRelation,
                                        bioseq_scan_state->snapshot,
                                        estate->es_query_cxt);
        if (features.has_mmsearch) {
            runtime.buffer.bind_persisted_gpu_operation_stats_sink(
                *bioseq::postgres::table::alignment_execution::
                    borrow_persisted_gpu_operation_accumulator(runtime.alignment_registration));
            runtime.buffer.bind_downstream_active_wall_tracker(
                *bioseq::postgres::table::alignment_execution::
                    borrow_downstream_active_wall_tracker(runtime.alignment_registration));
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            if (runtime.aligner_state.execution_plan->frozen_parameters.downstream_active_wall ==
                    nullptr ||
                !runtime.aligner_state.execution_plan->frozen_parameters.downstream_active_wall
                     ->begin_benchmark_e2e_statement_plan_proof(
                         runtime.aligner_state.execution_plan,
                         bioseq::simd::benchmark_e2e_statement_plan_shape::column_scan)) {
                throw std::logic_error{
                    "BioseqColumnScan admission changed its statement-plan control block"};
            }
#endif
        }

        bioseq_log<logging_level::debug>(
            "[bioseq] pg: BioseqColumnScan_BeginCustomScan() scan initialised successfully");
        /* scan slot is already initialized */
    }

} // export custom-scan partition declarations

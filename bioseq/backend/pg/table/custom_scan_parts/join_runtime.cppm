/**
 * @file
 * @brief Prepares custom-scan expressions, parameters, and match placeholders for execution.
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

export module bioseq.postgres.table:custom_scan_join_runtime;

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

using namespace bioseq::postgres::support;

export {

    Node* bioseq_column_scan_create_custom_scan_state(CustomScan* cscan) {
        bioseq_log<logging_level::debug>(
            "[bioseq] pg: BioseqColumnScan_CreateCustomScanState() entered");
        scan_state* bioseq_scan_state = scan_state::allocate();
        bioseq_scan_state->as_custom_scan_state()->methods = bioseq_column_scan_execute_methods();
        bioseq_scan_state->as_custom_scan_state()->ss.ss_currentScanDesc = nullptr;
        /*
         * Defer runtime allocation to BeginCustomScan: that's where we
         * have the EState (and therefore the per-query MemoryContext we
         * register the cleanup callback against). A scan that never enters
         * Begin (e.g. EXPLAIN without ANALYZE) therefore never allocates
         * a runtime — bioseq_runtime() in the Explain hook is the only
         * other caller that would lazily allocate, and that case is
         * benign because EXPLAIN doesn't run the executor body. The
         * `void*` storage on scan_state lets the public scan-state
         * header stay standard-layout; we re-cast in bioseq_runtime().
         */
        bioseq_scan_state->runtime_state = nullptr;
        bioseq_log<logging_level::debug>(
            "[bioseq] pg: BioseqColumnScan_CreateCustomScanState() returning");
        return bioseq_scan_state->as_node();
    }

    /*
     * eval_params_mutator evaluates Params in the expression and replaces them
     * with Consts, preserving the existing ExecInitExprWithParams / ExecEvalExpr
     * folding path for every Param (not a PARAM_EXEC-only direct lookup).
     */
    struct eval_params_mutator {
        [[nodiscard]] pg_node_mutation
        operator()(pg_expression_mutation_cursor, pg_node_view node, ExprContext& econtext) const {
            if (Param* param = node.try_cast<Param>()) {
                int16_t typ_len;
                bool typ_by_val;
                bool isnull;

                get_typlenbyval(param->paramtype, &typ_len, &typ_by_val);

                /* XXX: should save ExprState for efficiency */
                ExprState* expr_state = ExecInitExprWithParams(to_pg_expression(node.get()),
                                                               econtext.ecxt_param_list_info);
                Datum pval = ExecEvalExpr(expr_state, &econtext, &isnull);

                return pg_node_mutation::replace_with(
                    reinterpret_cast<Node*>(makeConst(param->paramtype,
                                                      param->paramtypmod,
                                                      param->paramcollid,
                                                      static_cast<int>(typ_len),
                                                      pval,
                                                      isnull,
                                                      typ_by_val)));
            }

            return pg_node_mutation::descend();
        }
    };

    /*
     * Thin wrapper for the direct callers (qual folding at Begin/ReScan and the
     * per-Param rebind fold) so they keep the (node, econtext) call shape.
     */
    [[nodiscard]] Node* eval_params(Node* node, ExprContext* econtext) {
        eval_params_mutator mutator;
        return mutate_pg_expression(node, *econtext, mutator);
    }

    enum class mmsearch_parameter_resolution : std::uint8_t {
        configured,
        null_value,
    };

    /* Resolve the exact Param that registered the primary mmsearch query.  Both
     * supported Param kinds use PostgreSQL's expression machinery so PARAM_EXEC
     * and PARAM_EXTERN read from their correct executor-owned storage. */
    [[nodiscard]] mmsearch_parameter_resolution resolve_mmsearch_param_query(
        bioseq_scan_runtime& runtime, ExprContext* econtext, const char* stage) {
        if (!runtime.param_rebind.active || econtext == nullptr ||
            (runtime.param_rebind.paramkind != PARAM_EXEC &&
             runtime.param_rebind.paramkind != PARAM_EXTERN) ||
            runtime.param_rebind.mmsearch_fe == nullptr ||
            list_length(runtime.param_rebind.mmsearch_fe->args) < 2) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("bioseq mmsearch Param rebind metadata is invalid"),
                     errdetail("stage=%s paramid=%d paramkind=%d",
                               stage,
                               runtime.param_rebind.paramid,
                               runtime.param_rebind.paramkind)));
        }

        postgres_list<Node> arguments(runtime.param_rebind.mmsearch_fe->args);
        Param* param = try_cast_pg_node<Param>(arguments.get_second());
        if (param == nullptr ||
            static_cast<int>(param->paramkind) != runtime.param_rebind.paramkind ||
            param->paramid != runtime.param_rebind.paramid ||
            param->paramtype != runtime.param_rebind.param_typoid ||
            param->paramtypmod != runtime.param_rebind.param_typmod) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq mmsearch Param changed after registration"),
                     errdetail("stage=%s paramid=%d paramkind=%d",
                               stage,
                               runtime.param_rebind.paramid,
                               runtime.param_rebind.paramkind)));
        }

        Node* folded = eval_params(reinterpret_cast<Node*>(param), econtext);
        Const* query_const = try_cast_pg_node<Const>(folded);
        if (query_const == nullptr || query_const->consttype != runtime.param_rebind.param_typoid ||
            query_const->consttypmod != runtime.param_rebind.param_typmod) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("bioseq mmsearch Param resolved to an invalid value"),
                     errdetail("stage=%s paramid=%d paramkind=%d",
                               stage,
                               runtime.param_rebind.paramid,
                               runtime.param_rebind.paramkind)));
        }
        if (query_const->constisnull) {
            runtime.buffer.clear_mmsearch_context();
            runtime.mmsearch_null_input = true;
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: {} mmsearch Param paramid={} kind={} resolved "
                "to NULL; strict qual matches no rows",
                stage,
                runtime.param_rebind.paramid,
                runtime.param_rebind.paramkind);
            return mmsearch_parameter_resolution::null_value;
        }

        bioseq::pg_types::seq* query_hdr =
            reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(query_const->constvalue));
        const auto query_type_params = query_hdr->type_parameters();
        if (runtime.param_rebind.param_typmod >= 0) {
            const auto declared_type_params =
                compact_sequence_stdc_type_params::decode(runtime.param_rebind.param_typmod);
            if (query_type_params != declared_type_params) {
                const unsigned declared_alphabet = declared_type_params.alphabet_code;
                const unsigned declared_width = declared_type_params.storage_unit_byte_width;
                const unsigned actual_alphabet = query_type_params.alphabet_code;
                const unsigned actual_width = query_type_params.storage_unit_byte_width;
                if (reinterpret_cast<void*>(query_hdr) !=
                    reinterpret_cast<void*>(DatumGetPointer(query_const->constvalue))) {
                    pfree(query_hdr);
                }
                ereport(ERROR,
                        (errcode(ERRCODE_DATATYPE_MISMATCH),
                         errmsg("bioseq mmsearch Param payload disagrees with its declared typmod"),
                         errdetail(
                             "stage=%s paramid=%d paramkind=%d paramtypmod=%d "
                             "declared_alphabet=%u declared_width=%u "
                             "actual_alphabet=%u actual_width=%u "
                             "target_alphabet=%u target_width=%u",
                             stage,
                             runtime.param_rebind.paramid,
                             runtime.param_rebind.paramkind,
                             runtime.param_rebind.param_typmod,
                             declared_alphabet,
                             declared_width,
                             actual_alphabet,
                             actual_width,
                             static_cast<unsigned>(runtime.param_rebind.alphabet_code),
                             static_cast<unsigned>(runtime.param_rebind.storage_unit_byte_width))));
            }
        }
        const compact_sequence_stdc_type_params target_type_params{
            runtime.param_rebind.alphabet_code, runtime.param_rebind.storage_unit_byte_width};
        if (query_type_params.alphabet_code != target_type_params.alphabet_code) {
            const auto mismatched_query_type_params = query_type_params;
            if (reinterpret_cast<void*>(query_hdr) !=
                reinterpret_cast<void*>(DatumGetPointer(query_const->constvalue))) {
                pfree(query_hdr);
            }
            raise_mmsearch_alphabet_mismatch(
                target_type_params, mismatched_query_type_params, stage);
        }
        if (!pgseq_supported_storage_unit_byte_widths::contains_runtime(
                query_type_params.storage_unit_byte_width)) {
            const unsigned actual_alphabet = query_type_params.alphabet_code;
            const unsigned actual_width = query_type_params.storage_unit_byte_width;
            if (reinterpret_cast<void*>(query_hdr) !=
                reinterpret_cast<void*>(DatumGetPointer(query_const->constvalue))) {
                pfree(query_hdr);
            }
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("bioseq mmsearch Param payload typmod is incompatible"),
                     errdetail("stage=%s paramid=%d paramkind=%d "
                               "expected_alphabet=%u expected_width=%u "
                               "actual_alphabet=%u actual_width=%u",
                               stage,
                               runtime.param_rebind.paramid,
                               runtime.param_rebind.paramkind,
                               static_cast<unsigned>(target_type_params.alphabet_code),
                               static_cast<unsigned>(target_type_params.storage_unit_byte_width),
                               actual_alphabet,
                               actual_width)));
        }
        const std::size_t query_total_bytes = static_cast<std::size_t>(VARSIZE(query_hdr));
        std::vector<char> query_varlena_bytes(query_total_bytes);
        std::memcpy(query_varlena_bytes.data(), query_hdr, query_total_bytes);
        const std::uint32_t bp_size = query_hdr->bp_size;
        std::vector<char> rewidthed_query_varlena_bytes;
        if (query_type_params.storage_unit_byte_width !=
            target_type_params.storage_unit_byte_width) {
            bioseq::pg_types::seq* rewidthed =
                ::bioseq::simd::active_target::stdc_rewidth_to_type_params(
                    *query_hdr, target_type_params, pg_alloc);
            if (rewidthed == nullptr) {
                if (reinterpret_cast<void*>(query_hdr) !=
                    reinterpret_cast<void*>(DatumGetPointer(query_const->constvalue))) {
                    pfree(query_hdr);
                }
                ereport(
                    ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("bioseq mmsearch Param rewidth rejected an incompatible alphabet")));
            }
            const std::size_t rewidthed_bytes = static_cast<std::size_t>(VARSIZE(rewidthed));
            rewidthed_query_varlena_bytes.assign(reinterpret_cast<const char*>(rewidthed),
                                                 reinterpret_cast<const char*>(rewidthed) +
                                                     rewidthed_bytes);
            pfree(rewidthed);
        }
        if (reinterpret_cast<void*>(query_hdr) !=
            reinterpret_cast<void*>(DatumGetPointer(query_const->constvalue))) {
            pfree(query_hdr);
        }
        runtime.buffer.configure_mmsearch(runtime.param_rebind.seq_attno,
                                          runtime.param_rebind.column_name,
                                          std::move(query_varlena_bytes),
                                          runtime.param_rebind.alphabet_code,
                                          runtime.param_rebind.storage_unit_byte_width,
                                          runtime.param_rebind.primary_key_attno);
        if (!rewidthed_query_varlena_bytes.empty()) {
            runtime.buffer.apply_typmod_coalesce(std::move(rewidthed_query_varlena_bytes));
        }
        runtime.mmsearch_null_input = false;
        bioseq_log<logging_level::debug>(
            "[bioseq] pg: {} rebound mmsearch query bp_size={} extras_count={} "
            "from Param paramid={} kind={}",
            stage,
            bp_size,
            runtime.buffer.extra_query_count(),
            runtime.param_rebind.paramid,
            runtime.param_rebind.paramkind);
        return mmsearch_parameter_resolution::configured;
    }

    [[noreturn]] void raise_column_scan_vulkan_error_as_cpp(std::string_view detail,
                                                            std::string_view hint) {
        const std::string owned_detail{detail};
        const std::string owned_hint{hint};
        const char* detail_cstr = owned_detail.c_str();
        const char* hint_cstr = owned_hint.c_str();
        bioseq::postgres::support::invoke_pg_as_cpp([detail_cstr, hint_cstr] {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg_internal("%s", "Vulkan aligner execution failed"),
                     errdetail_internal("%s", detail_cstr),
                     errhint("%s", hint_cstr)));
        });
        pg_unreachable();
    }

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
                            ? TupleDescAttr(relation_descriptor, column_attno - 1)
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

        /*
         * Make a new ExprContext just like the existing one, except that we don't
         * reset it every tuple.
         */
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

        /*
         * Third list in custom_exprs should contain vectorized qual list if not
         * NULL.
         */
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

        /*
         * Vectorization is enabled if global variable is set and there is at least one
         * filter which can be vectorized.
         */
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

        /* The owned mmsearch was deliberately removed from plan.qual and lives in
         * custom_exprs[3], but its target/query Vars remain physical scan-column
         * dependencies.  In projection-free shapes such as count(*), omitting that
         * private expression makes the columnar reader optimize the target column
         * away and submit_scan observes a false NULL.  Combine only list cells for
         * dependency discovery; the private expression remains non-executable. */
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

        /*
         * If we have pending changes that need to be flushed (row_mask after update/delete)
         * or new stripe we need to do them here because sequential columnar scan
         * can have parallel execution and updated are not allowed in parallel mode.
         */
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
            runtime.lazy_source_provider = bioseq::storage::create_pg_sequence_source_provider(
                cscanstate->ss.ss_currentRelation, bioseq_scan_state->snapshot);
            if (runtime.lazy_source_provider == nullptr) {
                ereport(ERROR,
                        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                         errmsg("Bioseq source provider is unavailable")));
            }
            runtime.buffer.bind_sequence_source_provider(runtime.lazy_source_provider);
        }

        /*
         * Discover the primary-key column by scanning the unmutated
         * targetlist for the first bioseq.<field>(Var) FuncExpr and using
         * its Var argument's varattno. BeginCustomScan runs before
         * CustomExecScan, so at this point cscan->scan.plan.targetlist
         * still contains the FuncExprs — they haven't been rewritten to
         * Const* yet (the rewrite target is newtargetlist, not
         * scan.plan.targetlist).
         */
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
            /* Projection helpers are semantically tied to the current match_result,
             * not to an int key argument.  The direct-query surface therefore
             * permits bioseq.edit(seq) / bioseq.cigar(seq).  Retain the
             * optional PK capture only when the supplied Var is actually INT4. */
            if (pk_var->vartype != INT4OID)
                continue;
            primary_key_attno = pk_var->varattno;
            bioseq_log<logging_level::debug>("[bioseq] pg: BeginCustomScan() detected primary_key "
                                             "varattno={} from bioseq projection funcid={}",
                                             static_cast<int>(primary_key_attno),
                                             fe->funcid);
            break;
        }

        /*
         * Register the per-query MemoryContext callback BEFORE touching
         * the runtime so an ereport(ERROR) further down still triggers
         * cleanup on transaction abort. Idempotent across rescans.
         */
        register_runtime_cleanup_callback(bioseq_scan_state, estate);

        /*
         * Decode the planner-owned expression set from custom_exprs and configure
         * the dispatch buffer. The executable qual list is intentionally ignored.
         */
        /* Begin is the only place that should drop extras as part of the
         * primary reset — every Begin call repopulates them from scratch
         * via the qual loop. ReScan / failure paths must NOT drop extras
         * (see clear_mmsearch_context for the retention contract). */
        runtime.buffer.clear_mmsearch_context(/*clear_extras=*/true);
        runtime.param_rebind = mmsearch_parameter_rebind{};

        Relation relation_for_attname = cscanstate->ss.ss_currentRelation;
        const Oid relation_oid_for_attname = RelationGetRelid(relation_for_attname);

        /*
         * State used by the classifier/configuration branches. Planner
         * ownership guarantees this loop contains one primary expression and, only
         * for the parameterized join shape, at most one same-target Const extra.
         *
         * `primary_configured` flips once the primary owned expression is installed
         * through the Const, Param-rebind, or same-relation Var path.
         */
        bool primary_configured = false;
        AttrNumber primary_seq_attno = InvalidAttrNumber;
        FuncExpr* primary_mmsearch_fe = nullptr;

        /*
         * Process only the private expressions selected by the planner. Executable
         * quals never participate in alignment ownership. General stacked calls are
         * rejected before a CustomPath is built; the bounded Param-primary/Const-
         * extra shape is sorted and configured here.
         *
         * Solution: classify quals first, then pick the primary by
         * priority:
         *   - Var-same-rel  > Param  > Const
         * Var-same-rel and Param NEED to be the primary (their machinery
         * lives in the buffer's mmsearch_context / param_rebind slots
         * which only exist once per scan). Const can be either primary
         * OR an extra. So putting Param first guarantees the AND-stack
         * works regardless of planner qual order.
         *
         * If we discover a Var-same-rel and a Param in the same plan
         * we reject the Param (we only have one primary slot); same for
         * multi-Param or multi-Var-same-rel. Every top-level mmsearch qual must
         * be accounted for below: unsupported combinations fail closed rather
         * rather than reaching the fail-closed SQL rendezvous stub.
         */
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
                /* The storage-pruning copy deliberately drops every clause whose
                 * subtree contains mmsearch(), but the alignment runtime currently
                 * consumes only a top-level mmsearch FuncExpr. Letting a BooleanTest,
                 * NOT, CASE, COALESCE, or other wrapper proceed would leave the
                 * runtime unconfigured and evaluate the immutable SQL stub instead.
                 * That stub is only a planner/executor rendezvous marker and must never
                 * become the query's alignment result. */
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
            /* Reject a parameterized/column-valued third argument even when this
             * qualifier is later rejected as an unsupported primary/extra shape. */
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
        /*
         * Sort by priority (Var-same-rel > Param > Const) using stable
         * partition so quals of equal priority keep planner order. The
         * Var-same-rel and Param branches can each fire AT MOST ONCE per
         * scan (they consume the single primary slot); unsupported subsequent
         * quals fail closed below.
         */
        std::stable_sort(
            classified_quals.begin(),
            classified_quals.end(),
            [](const classified_mmsearch_qualifier& lhs, const classified_mmsearch_qualifier& rhs) {
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
                /* PARAM_EXEC is installed by a parameterized Nested Loop;
                 * PARAM_EXTERN is supplied by PREPARE/client parameters. The
                 * shared evaluator below routes each kind through PostgreSQL's
                 * own ExprContext storage. */
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
                TupleDescAttr(tuple_descriptor, col_var->varattno - 1);
            if (attribute_form->atttypid != scan_stub_oids.bioseq_seq_type_oid) {
                bioseq_log<logging_level::debug>("[bioseq] pg: BeginCustomScan() mmsearch arg0 "
                                                 "column type is {}, expected bioseq.seq ({})",
                                                 attribute_form->atttypid,
                                                 scan_stub_oids.bioseq_seq_type_oid);
                continue;
            }

            /*
             * Resolve arg1's typmod. There are exactly three outcomes:
             *   - same (alphabet, width): direct dispatch, no extras.
             *   - same alphabet, different width: deterministic rewidth to
             *     the target column's physical shape.
             *   - different bound alphabets: SQLSTATE 42804 before any
             *     detoast, dispatch, sidecar read, or generic recoder.
             */
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
            /* PostgreSQL's generic PREPARE Param retains only the base type OID and
             * commonly reports paramtypmod=-1 even when the declaration spells a
             * bioseq typmod.  Its varlena header remains authoritative and is checked
             * on every Begin/ReScan by resolve_mmsearch_param_query(). */
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

            /*
             * Sanity-check the primary key column: the scan contract requires INT4.
             * If it is not INT4 or InvalidAttrNumber we still configure — the
             * batch processor will treat the column as null-PK and filter
             * those rows out (see tuple_dispatch_buffer::run_batch_and_build_cursors).
             */
            if (primary_key_attno != InvalidAttrNumber &&
                primary_key_attno <= tuple_descriptor->natts) {
                Form_pg_attribute pk_attr_form =
                    TupleDescAttr(tuple_descriptor, primary_key_attno - 1);
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
                    const size_t query_total_bytes = static_cast<size_t>(VARSIZE(normalized_query));
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
                /*
                 * Literal-query case. Detoast the query bioseq.seq varlena once;
                 * submit_scan reuses the bytes across every row so the TOAST read
                 * never happens on the hot path. The buffer owns a std::vector<char>
                 * copy so the bytes outlive the planner's per-query memory context.
                 */
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
                    const size_t query_total_bytes = static_cast<size_t>(VARSIZE(query_hdr));
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
                    /* Same-alphabet width coalescing preserves logical letters and keeps
                     * the target column's sidecar valid. Alphabet mismatch was rejected
                     * above and can never enter this block. */
                    if (physical_query_type != unpacked_typmod) {
                        /*
                         * Rewidth the literal at the column's typmod when its physical
                         * payload has not already been normalized to that shape. We
                         * have to re-detoast here because the previous
                         * detoast scope above already pfree'd the local
                         * copy. apply_typmod_coalesce stores the result
                         * inside the buffer's mmsearch_context.
                         */
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
                            const size_t rewidthed_bytes = static_cast<size_t>(VARSIZE(rewidthed));
                            std::vector<char> coalesced_bytes(rewidthed_bytes);
                            std::memcpy(coalesced_bytes.data(), rewidthed, rewidthed_bytes);
                            pfree(rewidthed);
                            runtime.buffer.apply_typmod_coalesce(std::move(coalesced_bytes));
                        }
                    }
                }
            } else if (arg1_is_param) {
                /*
                 * Parameterized-query case. Stash the Param coordinates for the
                 * shared evaluator. PARAM_EXTERN is available and installed now;
                 * PARAM_EXEC remains inactive until its Nested Loop parent installs
                 * the value and calls ReScan.
                 */
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
            } else /* same-row raw Var or complete evaluated query recipe */
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
            /*
             * Don't break — subsequent loop iterations that also match
             * an mmsearch FuncExpr will fall into the
             * `primary_configured` branch above and stack as additional
             * AND filters.
             */
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
            /* These are the exact post-setrefs argument cells in custom_exprs[3].
             * The metadata-only mmsearch FuncExpr itself is never initialized. */
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

        /*
         * Build the per-scan minimap2_parameters from the planner-authored semantic
         * payload, then flip conditional-pipeline flags based on which bioseq.*
         * projections the finalized target list references.  The only live
         * aligner GUC read here is performance-only profiling.
         *
         * The resulting struct is pushed into the tuple_dispatch_buffer so every
         * batch row inherits the same parameters, and copied into the per-scan
         * runtime.aligner_state so EXPLAIN can reproduce the user-visible
         * decision tree without re-walking the plan.
         */
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
        bioseq::simd::minimap2_parameters aligner_params = frozen_semantics.params;
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
            aligner_params.alphabet_code = match_target_type.alphabet_code;
        }
        /* Profiling is diagnostic/performance-only and deliberately remains live. */
        aligner_params.enable_profiling = bioseq::aligner::settings::profiling::enabled;
        /*
         * The explicitness bit selects only the prefilter execution backend and is
         * not a result-semantic field.  Recover it from the already-frozen
         * aligner_options Const without consulting any GUC; discard the decoded
         * semantic values because semantic_payload is authoritative.
         */
        bool primary_vulkan_enabled_explicit = false;
        auto explicitness_probe_params = aligner_params;
        auto explicitness_probe_requirement = frozen_semantics.index_read_requirement;
        (void)apply_aligner_options_override(primary_mmsearch_fe,
                                             explicitness_probe_params,
                                             explicitness_probe_requirement,
                                             scan_stub_oids,
                                             &primary_vulkan_enabled_explicit);
        finalize_simd_chaining_policy(aligner_params);
        const bioseq::indexing::index_read_requirement index_read_requirement =
            frozen_semantics.index_read_requirement;
        if (runtime.match_evaluated_submission &&
            index_read_requirement != bioseq::indexing::index_read_requirement::require_absent) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqScan semantic payload has an invalid evaluated-target index "
                            "policy")));
        }
        aligner_params.do_cigar = features.uses_cigar || features.uses_edit;
        aligner_params.compute_divergence = features.uses_divergence;
        aligner_params.compute_mapq = features.uses_mapq;
        aligner_params.compute_edit_distance = features.uses_edit;
        aligner_params.exact_edit_filter_enabled = runtime.edit_predicate_active;
        aligner_params.exact_edit_filter_exclusive_upper_bound = runtime.edit_exclusive_upper_bound;
        if (runtime.edit_predicate_active && runtime.edit_exclusive_upper_bound > 0 &&
            static_cast<std::uint64_t>(runtime.edit_exclusive_upper_bound - 1) <=
                (std::numeric_limits<std::uint32_t>::max)()) {
            aligner_params.prefilter_enabled = true;
            aligner_params.prefilter_max_edits =
                static_cast<std::uint32_t>(runtime.edit_exclusive_upper_bound - 1);
            const bool existing_vulkan_route =
                aligner_params.chaining_algorithm ==
                    bioseq::simd::chaining_algorithm::vulkan_tiled_dynamic_programming ||
                aligner_params.minimizer_algorithm ==
                    bioseq::simd::minimizer_algorithm::vulkan_minimap_integer ||
                aligner_params.vulkan_seed_hits_enabled ||
                aligner_params.vulkan_fused_frontend_enabled ||
                aligner_params.vulkan_gpu_index_build_enabled ||
                aligner_params.vulkan_extend_enabled;
            const bool standalone_vulkan_prefilter_requested =
                primary_vulkan_enabled_explicit && aligner_params.vulkan_enabled;
            aligner_params.prefilter_backend =
                (existing_vulkan_route || standalone_vulkan_prefilter_requested)
                    ? bioseq::prefilter::prefilter_backend::vulkan
                    : (bioseq::settings::vectorization::enabled
                           ? bioseq::prefilter::prefilter_backend::simd
                           : bioseq::prefilter::prefilter_backend::scalar);
        }
        const bool selected_vulkan =
            bioseq::simd::pipe::vulkan_components_requested(aligner_params);
        const bool selected_vulkan_accelerator =
            bioseq::simd::pipe::vulkan_accelerator_requested(aligner_params);
        const bool executable_scan = (eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0;
        /*
         * C1 is deliberately fail-closed: the readable persisted minimizer-fragment
         * and minimizer-sequence
         * layouts lack the READY head-and-manifest identity required by ordinal 2.
         * Rebuilding pairs or trusting a warm
         * assembled/GPU cache would counterfeit a persisted-compatible read.
         * Reject executable scans here before Vulkan initialization and retain the
         * typed-processor check as defence in depth for non-standard callers. Plain
         * EXPLAIN keeps the ordinal visible without initializing either payload.
         */
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
        /* Plain EXPLAIN must describe the selected pipeline without touching the
         * process-owned Vulkan runtime or its immutable Metal policy.  PostgreSQL
         * clears EXEC_FLAG_EXPLAIN_ONLY for SELECT and EXPLAIN (ANALYZE), so every
         * executable path retains the fail-closed enablement, cache and device
         * checks below. */
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

        runtime.aligner_state.params = aligner_params;
        runtime.aligner_state.features = features;
        runtime.aligner_state.index_read_requirement = index_read_requirement;
        runtime.aligner_state.read_sequence_index = frozen_semantics.read_sequence_index;
        runtime.aligner_state.params_captured = true;

        /*
         * Capture the structured pipeline description for EXPLAIN.  Walks the
         * same dispatch ladder `dispatch_pipeline` would but invokes each stage's
         * `describe()` instead of `on_batch(...)`.  The cold planner selects the
         * already-validated target alphabet once, then constructs its own empty
         * query/target prototypes; this is not another dispatch-chain dimension.
         */
        if (features.has_mmsearch) {
            runtime.aligner_state.pipeline_desc = describe_pipeline_for_pg_alphabet(
                aligner_params,
                aligner_params.alphabet_code,
                query_sketch_plan_route_for(index_read_requirement));
        }

        /*
         * Only consult the sidecar when mmsearch is actually present. Without
         * mmsearch there is no batch_processor call so opening the sidecar
         * relation would be wasted work (and would surface a confusing
         * "Recomputed" line in EXPLAIN on a query that doesn't use any index).
         */
        const Oid resolved_user_relid = cscanstate->ss.ss_currentRelation != nullptr
                                            ? RelationGetRelid(cscanstate->ss.ss_currentRelation)
                                            : InvalidOid;
        /*
         * Pass the parent's three-part physical identity
         * (database, tablespace, relfilenode) so the dispatch buffer can route
         * its per-bucket SM::fetch_payload_bulk calls to the right sub-tables.
         */
        Oid scan_db_oid = InvalidOid;
        Oid scan_spc_oid = InvalidOid;
        Oid scan_relfile = InvalidOid;
        if (cscanstate->ss.ss_currentRelation != nullptr) {
            scan_db_oid = cscanstate->ss.ss_currentRelation->rd_locator.dbOid;
            scan_spc_oid = cscanstate->ss.ss_currentRelation->rd_locator.spcOid;
            scan_relfile = cscanstate->ss.ss_currentRelation->rd_locator.relNumber;
        }
        runtime.buffer.configure_params(aligner_params,
                                        index_read_requirement,
                                        features.has_mmsearch &&
                                            frozen_semantics.read_sequence_index &&
                                            !runtime.match_evaluated_submission,
                                        resolved_user_relid,
                                        scan_db_oid,
                                        scan_spc_oid,
                                        scan_relfile,
                                        cscanstate->ss.ss_currentRelation,
                                        bioseq_scan_state->snapshot,
                                        estate->es_query_cxt);
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
            runtime.buffer.bind_persisted_gpu_operation_stats_sink(
                *bioseq::postgres::table::alignment_execution::
                    borrow_persisted_gpu_operation_accumulator(runtime.alignment_registration));
        }

        bioseq_log<logging_level::debug>(
            "[bioseq] pg: BioseqColumnScan_BeginCustomScan() scan initialised successfully");
        /* scan slot is already initialized */
    }

    void
    bioseq_column_scan_begin_custom_scan(CustomScanState* cscanstate, EState* estate, int eflags) {
        ErrorData* exact_error = nullptr;
        bool cpp_error = false;
        bool unknown_cpp_error = false;
        std::array<char, 512> detail{"std::exception without detail"};
        try {
            bioseq_column_scan_begin_custom_scan_impl(cscanstate, estate, eflags);
            return;
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

        scan_state* state = scan_state::from_custom_scan_state(cscanstate);
        if (state != nullptr && state->runtime_state != nullptr) {
            auto* runtime = static_cast<bioseq_scan_runtime*>(state->runtime_state);
            runtime->buffer.abandon_join_runtime_after_pg_error_noexcept();
        }
        if (exact_error != nullptr) {
            ReThrowError(exact_error);
        }
        if (cpp_error) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqColumnScan Begin raised a C++ exception"),
                     errdetail_internal("%s", detail.data())));
        }
        if (unknown_cpp_error) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqColumnScan Begin raised an unknown C++ exception")));
        }
    }

    /*
     * ExecScanFetch -- check interrupts & fetch next potential tuple
     *
     * This routine is concerned with substituting a test tuple if we are
     * inside an EvalPlanQual recheck.  If we aren't, just execute
     * the access method's next-tuple routine.
     */
    inline TupleTableSlot*
    exec_scan_fetch(ScanState* node, ExecScanAccessMtd access_mtd, ExecScanRecheckMtd recheck_mtd) {
        bioseq_log<logging_level::debug>("[bioseq] pg: ExecScanFetch() entered");
        EState* estate = node->ps.state;

        CHECK_FOR_INTERRUPTS();

        if (estate->es_epq_active != nullptr) {
            EPQState* epqstate = estate->es_epq_active;

            /*
             * We are inside an EvalPlanQual recheck.  Return the test tuple if
             * one is available, after rechecking any access-method-specific
             * conditions.
             */
            Index scanrelid = reinterpret_cast<Scan*>(node->ps.plan)->scanrelid;

            if (scanrelid == 0) {
                /*
                 * This is a ForeignScan or CustomScan which has pushed down a
                 * join to the remote side.  The recheck method is responsible not
                 * only for rechecking the scan/join quals but also for storing
                 * the correct tuple in the slot.
                 */

                TupleTableSlot* slot = node->ss_ScanTupleSlot;

                if (!(*recheck_mtd)(node, slot))
                    ExecClearTuple(slot); /* would not be returned by scan */
                return slot;
            } else if (epqstate->relsubs_done[scanrelid - 1]) {
                /*
                 * Return empty slot, as we already performed an EPQ substitution
                 * for this relation.
                 */

                TupleTableSlot* slot = node->ss_ScanTupleSlot;

                /* Return empty slot, as we already returned a tuple */
                return ExecClearTuple(slot);
            } else if (epqstate->relsubs_slot[scanrelid - 1] != nullptr) {
                /*
                 * Return replacement tuple provided by the EPQ caller.
                 */

                TupleTableSlot* slot = epqstate->relsubs_slot[scanrelid - 1];

                Assert(epqstate->relsubs_rowmark[scanrelid - 1] == nullptr);

                /* Mark to remember that we shouldn't return more */
                epqstate->relsubs_done[scanrelid - 1] = true;

                /* Return empty slot if we haven't got a test tuple */
                if (TupIsNull(slot))
                    return nullptr;

                /* Check if it meets the access-method conditions */
                if (!(*recheck_mtd)(node, slot))
                    return ExecClearTuple(slot); /* would not be returned by
                                                  * scan */
                return slot;
            } else if (epqstate->relsubs_rowmark[scanrelid - 1] != nullptr) {
                /*
                 * Fetch and return replacement tuple using a non-locking rowmark.
                 */

                TupleTableSlot* slot = node->ss_ScanTupleSlot;

                /* Mark to remember that we shouldn't return more */
                epqstate->relsubs_done[scanrelid - 1] = true;

                if (!EvalPlanQualFetchRowMark(epqstate, scanrelid, slot))
                    return nullptr;

                /* Return empty slot if we haven't got a test tuple */
                if (TupIsNull(slot))
                    return nullptr;

                /* Check if it meets the access-method conditions */
                if (!(*recheck_mtd)(node, slot))
                    return ExecClearTuple(slot); /* would not be returned by
                                                  * scan */
                return slot;
            }
        }

        /*
         * Run the node-type-specific access method function to get the next tuple
         */
        return (*access_mtd)(node);
    }

    /*
     * Dispatch table: given a matched funcid, which placeholder Const do we
     * substitute in? bioseq.score is an alias for bioseq.dp_score so both
     * resolve to the same Const* (paradedb's score/pdb.score alias pattern).
     */
    Const* lookup_bioseq_placeholder_const(Oid funcid, const var_injector_context* ctx) {
        const bioseq_stub_oids* o = ctx->oids;
        const placeholder_constants* c = ctx->consts;
        if (funcid == o->score_oid || funcid == o->dp_score_oid)
            return c->dp_score_node;
        if (funcid == o->query_len_oid)
            return c->query_len_node;
        if (funcid == o->query_start_oid)
            return c->query_start_node;
        if (funcid == o->query_end_oid)
            return c->query_end_node;
        if (funcid == o->target_len_oid)
            return c->target_len_node;
        if (funcid == o->target_start_oid)
            return c->target_start_node;
        if (funcid == o->target_end_oid)
            return c->target_end_node;
        if (funcid == o->num_matches_oid)
            return c->num_matches_node;
        if (funcid == o->block_length_oid)
            return c->block_length_node;
        if (funcid == o->edit_oid)
            return c->edit_distance_node;
        if (funcid == o->mapq_oid)
            return c->mapq_node;
        if (funcid == o->chain_score_oid)
            return c->chain_score_node;
        if (funcid == o->dp_max_score_oid)
            return c->dp_max_score_node;
        if (funcid == o->seed_count_oid)
            return c->seed_count_node;
        if (funcid == o->secondary_chain_score_oid)
            return c->secondary_chain_score_node;
        if (funcid == o->is_reverse_oid)
            return c->is_reverse_node;
        if (funcid == o->is_primary_oid)
            return c->is_primary_node;
        if (funcid == o->has_cigar_oid)
            return c->has_cigar_node;
        if (funcid == o->divergence_oid)
            return c->divergence_node;
        if (funcid == o->cigar_oid)
            return c->cigar_node;
        return nullptr;
    }

    [[nodiscard]] bool
    base_match_helper_is_finalized_for_scan(FuncExpr* function,
                                            const var_injector_context& context) {
        if (function == nullptr || context.oids == nullptr || context.stable_match_id < 0 ||
            context.match_helper_sites == NIL ||
            batch_join_well_formed_match_helper(reinterpret_cast<Node*>(function), *context.oids) ==
                nullptr) {
            return false;
        }
        Node* argument = batch_join_match_helper_argument(function);
        if (argument == nullptr)
            return false;
        for (List* site : postgres_list<List>(context.match_helper_sites)) {
            if (site == nullptr || !IsA(site, List) ||
                list_length(site) != k_bioseq_match_helper_site_fields)
                continue;
            Node* canonical_argument =
                reinterpret_cast<Node*>(list_nth(site, k_match_site_canonical_argument));
            if (static_cast<Oid>(intVal(list_nth(site, k_match_site_funcid))) == function->funcid &&
                (static_cast<match_helper_binding_kind>(
                     intVal(list_nth(site, k_match_site_binding_kind))) ==
                     match_helper_binding_kind::exact_recipe ||
                 static_cast<match_helper_binding_kind>(
                     intVal(list_nth(site, k_match_site_binding_kind))) ==
                     match_helper_binding_kind::unique_lineage) &&
                intVal(list_nth(site, k_match_site_stable_id)) == context.stable_match_id &&
                canonical_argument != nullptr && equal(canonical_argument, argument))
                return true;
        }
        return false;
    }

    /*
     * Rewrite a single `bioseq.symbol_count(Var, 'L')` call site into a
     * placeholder Const that the executor populates per-row from meta_sidecar.
     * Returns the replacement Const*, or nullptr when the FuncExpr does not
     * match the supported shape (in which case the caller must leave the
     * original FuncExpr in place so the scalar O(n) path runs).
     *
     * Matching is deliberately strict: arg0 must be a Var (a column reference
     * with a concrete bioseq.seq typmod), and arg1 must be a single-byte
     * non-null text Const whose letter is in the column's alphabet. Any call
     * outside this shape — e.g. symbol_count('ACGT'::bioseq.seq, 'A') — keeps
     * its original FuncExpr and runs through the scalar entry point in
     * mod.cppm, which is cost-inflated by bioseq_symbol_count_support.
     */
    Const* try_rewrite_symbol_count(FuncExpr* fe, var_injector_context* ctx) {
        if (ctx == nullptr || fe == nullptr || ctx->consts == nullptr) {
            return nullptr;
        }
        if (list_length(fe->args) != 2) {
            return nullptr;
        }
        postgres_list<Node> fe_args(fe->args);
        Node* arg0 = fe_args.get_first();
        Node* arg1 = fe_args.get_second();
        if (arg0 == nullptr || arg1 == nullptr) {
            return nullptr;
        }
        Var* col_var = try_cast_pg_node<Var>(arg0);
        Const* letter_const = try_cast_pg_node<Const>(arg1);
        if (col_var == nullptr || letter_const == nullptr) {
            bioseq_log<logging_level::debug>("[bioseq] pg: var_injector_mutator() leaving "
                                             "symbol_count(...) alone; arg0_tag={} arg1_tag={}",
                                             static_cast<int>(nodeTag(arg0)),
                                             static_cast<int>(nodeTag(arg1)));
            return nullptr;
        }
        if (col_var->vartypmod < 0 || col_var->vartype != ctx->oids->bioseq_seq_type_oid) {
            bioseq_log<logging_level::debug>("[bioseq] pg: VarInjectorMutator() symbol_count arg0 "
                                             "has wrong type/typmod (type={}, typmod={})",
                                             col_var->vartype,
                                             col_var->vartypmod);
            return nullptr;
        }
        if (letter_const->constisnull) {
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: VarInjectorMutator() symbol_count arg1 is NULL; skipping");
            return nullptr;
        }

        text* letter_text = reinterpret_cast<text*>(DatumGetPointer(letter_const->constvalue));
        if (letter_text == nullptr) {
            return nullptr;
        }
        text* letter_detoasted =
            reinterpret_cast<text*>(PG_DETOAST_DATUM(letter_const->constvalue));
        const int32 letter_size = VARSIZE_ANY_EXHDR(letter_detoasted);
        if (letter_size != 1) {
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: VarInjectorMutator() symbol_count arg1 size={} (expected 1)",
                letter_size);
            if (reinterpret_cast<void*>(letter_detoasted) !=
                reinterpret_cast<void*>(DatumGetPointer(letter_const->constvalue))) {
                pfree(letter_detoasted);
            }
            return nullptr;
        }
        const char letter_char = *VARDATA_ANY(letter_detoasted);
        if (reinterpret_cast<void*>(letter_detoasted) !=
            reinterpret_cast<void*>(DatumGetPointer(letter_const->constvalue))) {
            pfree(letter_detoasted);
        }

        const compact_sequence_stdc_type_params typmod =
            compact_sequence_stdc_type_params::decode(col_var->vartypmod);
        const int rank =
            bioseq::storage::indexing::letter_to_rank(typmod.alphabet_code, letter_char);
        if (rank < 0) {
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: VarInjectorMutator() symbol_count letter '{}' not in "
                "alphabet_code={}; keeping scalar call",
                letter_char,
                static_cast<unsigned>(typmod.alphabet_code));
            return nullptr;
        }

        /*
         * Build the placeholder Const. INT8 is byval on LP64 (pg_type.typbyval
         * for bigint is 't'), so constbyval=true + constlen=8 is correct. The
         * initial value is NULL until the executor populates it per-row.
         */
        Const* placeholder =
            makeConst(INT8OID,
                      -1,
                      InvalidOid,
                      8,
                      bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(0)).raw(),
                      /*constisnull=*/true,
                      /*constbyval=*/true);

        symbol_count_call_site site;
        site.placeholder = placeholder;
        site.rank = rank;
        site.letter = letter_char;
        site.column_attno = static_cast<int16_t>(col_var->varattno);
        if (ctx->current_relation != nullptr && col_var->varattno >= 1) {
            char* attname_cstr = get_attname(RelationGetRelid(ctx->current_relation),
                                             col_var->varattno,
                                             /*missing_ok=*/true);
            if (attname_cstr != nullptr) {
                site.column_name = std::string(attname_cstr);
                pfree(attname_cstr);
            }
        }
        bioseq_log<logging_level::debug>("[bioseq] pg: VarInjectorMutator() installing "
                                         "symbol_count placeholder column='{}' letter='{}' rank={}",
                                         site.column_name,
                                         letter_char,
                                         rank);

        ctx->consts->symbol_count_sites.push_back(std::move(site));
        return placeholder;
    }

    struct var_injector_mutator {
        [[nodiscard]] pg_node_mutation operator()(pg_expression_mutation_cursor,
                                                  pg_node_view node,
                                                  var_injector_context& injector_context) const {
            if (FuncExpr* func_expr = node.try_cast<FuncExpr>()) {
                if (!edit_sequence_argument_matches_recipe(func_expr,
                                                           injector_context.match_target_recipe,
                                                           injector_context.match_sequence_varno,
                                                           injector_context.match_sequence_attno,
                                                           *injector_context.oids)) {
                    ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                             errmsg("bioseq.edit projection must reference the mmsearch target "
                                    "column")));
                }
                Const* replacement =
                    base_match_helper_is_finalized_for_scan(func_expr, injector_context)
                        ? lookup_bioseq_placeholder_const(func_expr->funcid, &injector_context)
                        : nullptr;
                if (replacement != nullptr) {
                    bioseq_log<logging_level::debug>(
                        "[bioseq] pg: var_injector_mutator() replacing bioseq funcid={} with "
                        "placeholder Const",
                        func_expr->funcid);
                    return pg_node_mutation::replace_with(to_pg_node(replacement));
                }
                if (func_expr->funcid == injector_context.oids->symbol_count_oid) {
                    Const* symbol_placeholder =
                        try_rewrite_symbol_count(func_expr, &injector_context);
                    if (symbol_placeholder != nullptr) {
                        return pg_node_mutation::replace_with(to_pg_node(symbol_placeholder));
                    }
                    /* Call shape not supported (literal arg, non-Const letter,
                     * letter outside alphabet, …) — let the scalar entry point
                     * in mod.cppm handle it in place. */
                }
                /* Fall through to recurse into arg subtrees of unrelated FuncExprs. */
            }
            /*
             * PlaceHolderVar handling. By Postgres convention the planner
             * resolves PHVs to Vars / Params by execution time (pathnodes.h:
             * "it is not recognized by the parser or executor"), so seeing
             * one here is a planner bug. We log loudly and pass the node
             * through — unwrapping would silently drop the PHV's
             * phnullingrels metadata and could produce wrong results in
             * outer-join nullable positions (a plain Const has no nulling
             * semantics). Returning descend() does the right thing because
             * the cursor recurses into phexpr, giving var_injector_mutator a
             * chance to rewrite our FuncExpr in place; the PHV node itself is
             * preserved.
             */
            if (PlaceHolderVar* phv = node.try_cast<PlaceHolderVar>()) {
                bioseq_log<logging_level::debug>(
                    "[bioseq] pg: var_injector_mutator() encountered PlaceHolderVar phid={} at "
                    "exec time — passing through",
                    phv->phid);
                /* fall through to descend */
            }
            return pg_node_mutation::descend();
        }
    };

    /*
     * Thin wrapper for the direct callers (execution-time targetlist rewrite)
     * so they keep the (node, ctx) call shape.
     */
    [[nodiscard]] Node* var_injector(Node* node, var_injector_context* injector_context) {
        var_injector_mutator mutator;
        return mutate_pg_expression(node, *injector_context, mutator);
    }

    /*
     * Per-tuple update of every placeholder Const* from a match_result's
     * fields. Fields we don't have a Const for (because the query didn't
     * project them) are skipped thanks to the nullptr guard. The cigar text
     * Datum is allocated via CStringGetTextDatum in CurrentMemoryContext.
     */
    void populate_placeholder_consts(placeholder_constants& consts,
                                     const bioseq::simd::match_result& match) {
        using bioseq::simd::format_cigar;
        // Absolute coordinates and lengths are unsigned 64-bit values. They are projected
        // through BIGINT (int8) placeholders so positions / lengths in sequences
        // longer than `INT32_MAX` (~2.15 Gbp) are NOT truncated. `set_bigint`
        // fills those eight (query_len … block_length) via `Int64GetDatum`. The
        // remaining match_result fields (dp_score, mapq, chain_score, seed_count,
        // …) are genuinely `int32_t` in the struct, so they keep INT4 placeholders
        // and `set_int` — widening them at this boundary cannot recover precision
        // the source type never held.
        //
        // `set_bigint`'s Assert is a debug-time witness that no real coordinate
        // reaches the int64 sign boundary: the project's pessimistic ceiling is
        // 1 Tbp (~2^40), ~23 bits below INT64_MAX, so the uint64→int64 reinterpret
        // is always value-preserving.
        auto set_int = [](Const* node, int64_t value) {
            if (node == nullptr)
                return;
            Assert(value >= INT32_MIN && value <= INT32_MAX);
            node->constvalue =
                bioseq::postgres::support::pg_datum::from(static_cast<std::int32_t>(value)).raw();
            node->constisnull = false;
        };
        // Parameter is the underlying type of `sequence_size_t` (std::uint64_t)
        // spelled directly — this TU does not import bioseq_storage_size, so the
        // `sequence_size_t` alias is not in scope here.
        auto set_bigint = [](Const* node, std::uint64_t value) {
            if (node == nullptr)
                return;
            Assert(value <= static_cast<std::uint64_t>(INT64_MAX));
            node->constvalue =
                bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(value)).raw();
            node->constisnull = false;
        };
        auto set_bool = [](Const* node, bool value) {
            if (node == nullptr)
                return;
            node->constvalue = bioseq::postgres::support::pg_datum::from(value).raw();
            node->constisnull = false;
        };
        auto set_float = [](Const* node, float value) {
            if (node == nullptr)
                return;
            node->constvalue = bioseq::postgres::support::pg_datum::from(value).raw();
            node->constisnull = false;
        };

        set_int(consts.dp_score_node, match.dp_score);
        // The eight sequence_size_t (uint64) coordinates/lengths → BIGINT, no narrow.
        set_bigint(consts.query_len_node, match.query_len);
        set_bigint(consts.query_start_node, match.query_start);
        set_bigint(consts.query_end_node, match.query_end);
        set_bigint(consts.target_len_node, match.target_len);
        set_bigint(consts.target_start_node, match.target_start);
        set_bigint(consts.target_end_node, match.target_end);
        set_bigint(consts.num_matches_node, match.num_matches);
        set_bigint(consts.block_length_node, match.block_length);
        set_bigint(consts.edit_distance_node, match.edit_distance);
        set_int(consts.mapq_node, match.mapq);
        set_int(consts.chain_score_node, match.chain_score);
        set_int(consts.dp_max_score_node, match.dp_max_score);
        set_int(consts.seed_count_node, match.seed_count);
        set_int(consts.secondary_chain_score_node, match.secondary_chain_score);
        set_bool(consts.is_reverse_node, match.is_reverse);
        set_bool(consts.is_primary_node, match.is_primary);
        set_bool(consts.has_cigar_node, match.has_cigar);
        set_float(consts.divergence_node, match.divergence);

        if (consts.cigar_node != nullptr) {
            std::string cigar_str = format_cigar(match.cigar);
            consts.cigar_node->constvalue =
                bioseq::postgres::support::pg_datum::from_text_cstring(cigar_str.c_str()).raw();
            consts.cigar_node->constisnull = false;
        }
    }

} // export custom-scan partition declarations

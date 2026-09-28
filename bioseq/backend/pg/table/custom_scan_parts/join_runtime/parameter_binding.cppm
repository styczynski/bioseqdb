/**
 * @file
 * @brief Creates BioseqScan state and binds PostgreSQL parameters to scan inputs.
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

export module bioseq.postgres.table:custom_scan_join_runtime_parameter_binding;

import std;
import bioseq.common.logging;
import bioseq.postgres.support;
import bioseq.postgres.table.scan_state;
import bioseq.postgres.types.compact_sequence;
import bioseq.sequences.simd;

import :custom_scan_execution_method_contracts;
import :custom_scan_scan_runtime_state_contracts;
import :custom_scan_join_ownership_sequence_typmod_validation_contracts;
export import :custom_scan_join_runtime_parameter_binding_contracts;

using namespace bioseq::postgres::support;

export {

    /**
     * @brief Allocates the BioseqScan node and leaves its C++ runtime for BeginCustomScan.
     * @return A PostgreSQL node in the current memory context.
     */
    Node* bioseq_column_scan_create_custom_scan_state(CustomScan* cscan) {
        bioseq_log<logging_level::debug>(
            "[bioseq] pg: BioseqColumnScan_CreateCustomScanState() entered");
        scan_state* bioseq_scan_state = scan_state::allocate();
        bioseq_scan_state->as_custom_scan_state()->methods = bioseq_column_scan_execute_methods();
        bioseq_scan_state->as_custom_scan_state()->ss.ss_currentScanDesc = nullptr;
        /* BeginCustomScan has the EState needed to attach runtime cleanup
         * to the query memory context. Leave allocation until then
         * The void* field keeps scan_state standard-layout while storing
         * the C++ runtime separately. */
        bioseq_scan_state->runtime_state = nullptr;
        bioseq_log<logging_level::debug>(
            "[bioseq] pg: BioseqColumnScan_CreateCustomScanState() returning");
        return bioseq_scan_state->as_node();
    }

    /**
     * @brief Replaces each Param with its current  value as a Const.
     *
     * Use PostgreSQL's expression evaluator so both PARAM_EXEC and PARAM_EXTERN
     * read the right parameter storage.
     */
    struct eval_params_mutator {
        /** @brief Evaluates a Param node and preserves its declared type metadata. */
        [[nodiscard]] pg_node_mutation
        operator()(pg_expression_mutation_cursor, pg_node_view node, ExprContext& econtext) const {
            if (Param* param = node.try_cast<Param>()) {
                int16_t typ_len;
                bool typ_by_val;
                bool isnull;

                get_typlenbyval(param->paramtype, &typ_len, &typ_by_val);

                /* Each, Param needs its own ExprState to read its current executor value. */
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

    /** @brief Evaluates every Param in an expression against the supplied executor context. */
    [[nodiscard]] Node* eval_params(Node* node, ExprContext* econtext) {
        eval_params_mutator mutator;
        return mutate_pg_expression(node, *econtext, mutator);
    }

    /**
     * @brief Reads the current query parameter and configures mmsearch for Begin or ReScan.
     *
     * Evaluate PARAM_EXEC and PARAM_EXTERN through PostgreSQL, then check the
     * sequence payload against its declared type and the target alphabet. Repack
     * a different supported storage width whn needed. A NULL query clears the
     * search context  and produces no matches.
     *
     * @throws PostgreSQL ERROR for changed parameter metadata or an incompatible
     * sequence type.
     */
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
        const std::size_t query_total_bytes = static_cast<std::size_t>(bioseq::postgres::support::varlena_4byte_size(query_hdr));
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
            const std::size_t rewidthed_bytes = static_cast<std::size_t>(bioseq::postgres::support::varlena_4byte_size(rewidthed));
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

    /** @brief Reports a Vulkan failure as a PostgreSQL error carried through the C++ exception bridge. */
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

} // export custom-scan partition declarations

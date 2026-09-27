/**
 * @file
 * @brief Validates sequence typmods used by mmsearch planner owners.
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

export module bioseq.postgres.table:custom_scan_join_ownership_sequence_typmod;

import std;
import bioseq.sequences;
import bioseq.postgres.support;
import bioseq.postgres.types.compact_sequence;
import :custom_scan_oid_expression_support_oid_catalog;
import :custom_scan_slice_path_codec_contract;
export import :custom_scan_join_ownership_sequence_typmod_validation_contracts;
export import :custom_scan_join_ownership_expression_signature_contract;

using namespace bioseq::postgres::support;

export {
    /** @brief Raises a PostgreSQL datatype error for operands with different alphabets. */
    [[noreturn]] void
    raise_mmsearch_alphabet_mismatch(const compact_sequence_stdc_type_params target,
                                     const compact_sequence_stdc_type_params query,
                                     const char* stage) {
        const auto target_name =
            pgseq_supported_alphabets::name_of_alphabet_code(target.alphabet_code);
        const auto query_name =
            pgseq_supported_alphabets::name_of_alphabet_code(query.alphabet_code);
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("bioseq mmsearch requires operands with the same bound alphabet"),
                 errdetail("stage=%s target=seq('%.*s', %u) query=seq('%.*s', %u)",
                           stage,
                           static_cast<int>(target_name.size()),
                           target_name.data(),
                           static_cast<unsigned>(target.storage_unit_byte_width) * 8u,
                           static_cast<int>(query_name.size()),
                           query_name.data(),
                           static_cast<unsigned>(query.storage_unit_byte_width) * 8u),
                 errhint("Explicitly convert the data before binding it to one alphabet; mmsearch "
                         "never performs cross-alphabet recoding.")));
        pg_unreachable();
    }

    /**
     * @brief Derives one exact typmod through supported sequence-preserving expressions.
     *
     * PostgreSQL does not propagate typmods through ordinary FuncExpr results,
     * even when the exact catalog function is sequence-type preserving. Admit an
     * erased descriptor only after recursively validating one identical concrete
     * typmod at every sequence-valued leaf. Non-sequence literal or array operands
     * are neutral only for the exact registered concat overloads.
     */
    [[nodiscard]] std::optional<per_row_sequence_typmod_validation>
    per_row_validate_sequence_typmod(Node* expression, bioseq_stub_oids const& oids) {
        check_stack_depth();
        if (expression == nullptr || exprType(expression) != oids.bioseq_seq_type_oid) {
            return std::nullopt;
        }
        const int32 descriptor_typmod = exprTypmod(expression);
        auto finish =
            [descriptor_typmod](
                const int32 validated_typmod) -> std::optional<per_row_sequence_typmod_validation> {
            if (!batch_join_typmod_is_supported(validated_typmod) ||
                (descriptor_typmod != -1 && descriptor_typmod != validated_typmod)) {
                return std::nullopt;
            }
            return per_row_sequence_typmod_validation{.typmod = validated_typmod,
                                                      .descriptor_erased = descriptor_typmod == -1};
        };
        if (IsA(expression, RelabelType)) {
            auto* relabel = reinterpret_cast<RelabelType*>(expression);
            if (relabel->resulttype != oids.bioseq_seq_type_oid || relabel->arg == nullptr)
                return std::nullopt;
            auto child =
                per_row_validate_sequence_typmod(reinterpret_cast<Node*>(relabel->arg), oids);
            return child.has_value() ? finish(child->typmod) : std::nullopt;
        }
        const auto operation = slice_project_node_opcode(expression, oids);
        if (!operation.has_value()) {
            return batch_join_typmod_is_supported(descriptor_typmod) ? finish(descriptor_typmod)
                                                                     : std::nullopt;
        }
        if (*operation == slice_project_opcode::slice) {
            auto* subscript = reinterpret_cast<SubscriptingRef*>(expression);
            if (subscript->refcontainertype != oids.bioseq_seq_type_oid ||
                subscript->refrestype != oids.bioseq_seq_type_oid ||
                subscript->reflowerindexpr == NIL || subscript->refexpr == nullptr)
                return std::nullopt;
            auto source =
                per_row_validate_sequence_typmod(reinterpret_cast<Node*>(subscript->refexpr), oids);
            return source.has_value() ? finish(source->typmod) : std::nullopt;
        }
        if (*operation == slice_project_opcode::reverse ||
            *operation == slice_project_opcode::reverse_complement) {
            FuncExpr* function = try_cast_pg_node<FuncExpr>(expression);
            if (function == nullptr || function->funcresulttype != oids.bioseq_seq_type_oid ||
                !per_row_function_signature_is_exact(function)) {
                return std::nullopt;
            }
            if (list_length(function->args) != 1)
                return std::nullopt;
            auto source = per_row_validate_sequence_typmod(
                reinterpret_cast<Node*>(linitial(function->args)), oids);
            return source.has_value() ? finish(source->typmod) : std::nullopt;
        }
        if (*operation != slice_project_opcode::concat)
            return std::nullopt;
        List* arguments = NIL;
        if (FuncExpr* function = try_cast_pg_node<FuncExpr>(expression)) {
            if (function->funcresulttype != oids.bioseq_seq_type_oid ||
                !slice_project_concat_oid(function->funcid, oids) ||
                !per_row_function_signature_is_exact(function)) {
                return std::nullopt;
            }
            arguments = function->args;
        } else if (OpExpr* operator_expression = try_cast_pg_node<OpExpr>(expression)) {
            if (operator_expression->opresulttype != oids.bioseq_seq_type_oid ||
                !slice_project_concat_oid(operator_expression->opfuncid, oids) ||
                list_length(operator_expression->args) != 2 ||
                !per_row_operator_signature_is_exact(operator_expression)) {
                return std::nullopt;
            }
            arguments = operator_expression->args;
        } else {
            return std::nullopt;
        }
        std::optional<int32> common_typmod;
        for (Node* argument : postgres_list<Node>(arguments)) {
            if (exprType(argument) != oids.bioseq_seq_type_oid)
                continue;
            auto child = per_row_validate_sequence_typmod(argument, oids);
            if (!child.has_value() ||
                (common_typmod.has_value() && *common_typmod != child->typmod))
                return std::nullopt;
            common_typmod = child->typmod;
        }
        return common_typmod.has_value() ? finish(*common_typmod) : std::nullopt;
    }
}

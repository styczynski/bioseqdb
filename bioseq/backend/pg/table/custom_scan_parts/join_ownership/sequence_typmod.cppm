/**
 * @file
 * @brief Checks the sequence alphabets and storage widths used by planned mmsearch calls.
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
import :custom_scan_join_ownership_semantic_admission_contracts;
export import :custom_scan_join_ownership_sequence_typmod_validation_contracts;
export import :custom_scan_join_ownership_expression_signature_contract;

using namespace bioseq::postgres::support;

export {
    /**
     * @brief Infers a named conversion's uint64-packed destination from its constant name.
     *
     * PostgreSQL inserts default arguments before this analysis. Dynamic or invalid
     * names supply no type proof; an enclosing bound cast may provide that proof,
     * with the actual Datum checked when captured by the scan. Never infer the
     * output alphabet from the source expression or evaluate conversion here.
     */
    [[nodiscard]] std::optional<int32>
    named_conversion_result_typmod(Node* expression, const bioseq_stub_oids& oids) {
        FuncExpr* function = try_cast_pg_node<FuncExpr>(expression);
        if (function == nullptr || function->funcresulttype != oids.bioseq_seq_type_oid ||
            !OidIsValid(function->funcid) ||
            (function->funcid != oids.seq_translate_oid &&
             function->funcid != oids.seq_back_translate_oid) ||
            !per_row_function_signature_is_exact(function) || list_length(function->args) < 2)
            return std::nullopt;
        Const* destination =
            try_cast_pg_node<Const>(reinterpret_cast<Node*>(lsecond(function->args)));
        if (destination == nullptr || destination->constisnull || destination->consttype != TEXTOID)
            return std::nullopt;
        char* name = TextDatumGetCString(destination->constvalue);
        const auto alphabet_code = pgseq_supported_biological_alphabets::try_index_of_name(name);
        pfree(name);
        if (!alphabet_code)
            return std::nullopt;
        const compact_sequence_stdc_type_params result{
            .alphabet_code = *alphabet_code,
            .storage_unit_byte_width = sizeof(std::uint64_t),
        };
        return batch_join_typmod_is_supported(result.encode())
            ? std::optional<int32>{result.encode()} : std::nullopt;
    }

    /**
     * @brief Rejects unbound dynamic conversion destinations throughout an alignment operand.
     *
     * Check even when another CASE/COALESCE arm supplies a known result type.
     * Bound outer casts stop traversal; they remain authoritative subject to
     * capture-time validation.
     */
    void require_bound_conversion_operand(Node* expression, const bioseq_stub_oids& oids) {
        bool dynamic_destination = false;
        const auto visit = [&oids](pg_expression_walk_cursor, pg_node_view node, bool& found) {
            if (RelabelType* relabel = node.try_cast<RelabelType>(); relabel != nullptr &&
                relabel->resulttype == oids.bioseq_seq_type_oid &&
                batch_join_typmod_is_supported(relabel->resulttypmod))
                return pg_walk_step::skip_subtree;
            FuncExpr* function = node.try_cast<FuncExpr>();
            if (function == nullptr || function->funcresulttype != oids.bioseq_seq_type_oid)
                return pg_walk_step::descend;
            if (batch_join_typmod_is_supported(exprTypmod(node.get())))
                return pg_walk_step::skip_subtree;
            if (OidIsValid(function->funcid) &&
                (function->funcid == oids.seq_translate_oid ||
                 function->funcid == oids.seq_back_translate_oid) &&
                per_row_function_signature_is_exact(function) && list_length(function->args) >= 2 &&
                !IsA(reinterpret_cast<Node*>(lsecond(function->args)), Const)) {
                found = true;
                return pg_walk_step::stop;
            }
            return pg_walk_step::descend;
        };
        walk_pg_expression_including_root(expression, dynamic_destination, visit);
        if (dynamic_destination)
            ereport(ERROR,
                    (errcode(ERRCODE_INDETERMINATE_DATATYPE),
                     errmsg("mmsearch requires a bound conversion destination alphabet"),
                     errhint("Use a constant destination alphabet or cast the complete conversion "
                             "result, for example bioseq.translate(source, destination)"
                             "::bioseq.seq('aa27', 64).")));
    }

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
     * @brief Reuses whole-expression type resolution for joins and records catalog typmod erasure.
     *
     * Base scans and per-row joins share conversion, CASE/COALESCE, concatenation
     * and explicit-cast rules. The resulting Datum is checked again at capture.
     */
    [[nodiscard]] std::optional<per_row_sequence_typmod_validation>
    per_row_validate_sequence_typmod(Node* expression, const bioseq_stub_oids& oids) {
        const auto resolved = resolve_base_local_sequence_typmod(expression, oids);
        if (resolved.invalid || !batch_join_typmod_is_supported(resolved.typmod))
            return std::nullopt;
        return per_row_sequence_typmod_validation{
            .typmod = resolved.typmod,
            .descriptor_erased = exprTypmod(expression) == -1,
        };
    }
}

/**
 * @file
 * @brief Recognizes sequence operations that SliceProject can track.
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

#include <cstdint>
#include <optional>

export module bioseq.postgres.table:custom_scan_slice_path_expression_contract;

import bioseq.postgres.support;
import :custom_scan_oid_expression_support_oid_catalog_contracts;

using namespace bioseq::postgres::support;

export {
    /** @brief Operation recorded for a node in the SliceProject expression graph. */
    enum class slice_project_opcode : std::uint8_t {
        source_expr = 1,
        literal_expr = 2,
        element = 3,
        slice = 4,
        concat = 5,
        reverse = 6,
        reverse_complement = 7,
        length = 8,
        cardinality = 9,
        bridge = 10,
        materialize = 11,
    };

    /** @brief True when @p oid is a registered sequence concat function variant. */
    [[nodiscard]] bool slice_project_concat_oid(Oid oid,
                                                const bioseq_stub_oids& oids) noexcept {
        return oid == oids.seq_concat_oid || oid == oids.seq_array_cat_oid ||
               oid == oids.seq_concat_text_oid || oid == oids.text_concat_seq_oid ||
               oid == oids.seq_concat_varchar_oid || oid == oids.varchar_concat_seq_oid ||
               oid == oids.seq_concat_text_array_oid || oid == oids.text_array_concat_seq_oid ||
               oid == oids.seq_concat_letter_array_oid || oid == oids.letter_array_concat_seq_oid;
    }

    /** @brief Identify a supported sequence  function by its registered OID. */
    [[nodiscard]] std::optional<slice_project_opcode>
    slice_project_function_opcode(FuncExpr* function,
                                  const bioseq_stub_oids& oids) noexcept {
        if (function == nullptr)
            return std::nullopt;
        if (slice_project_concat_oid(function->funcid, oids)) {
            return slice_project_opcode::concat;
        }
        if (function->funcid == oids.seq_reverse_oid) {
            return slice_project_opcode::reverse;
        }
        if (function->funcid == oids.seq_reverse_complement_oid) {
            return slice_project_opcode::reverse_complement;
        }
        if (function->funcid == oids.seq_len_oid || function->funcid == oids.seq_array_length_oid) {
            return slice_project_opcode::length;
        }
        if (function->funcid == oids.seq_cardinality_oid) {
            return slice_project_opcode::cardinality;
        }
        if (function->funcid == oids.seq_to_text_oid ||
            function->funcid == oids.seq_to_varchar_oid ||
            function->funcid == oids.seq_to_array_oid) {
            return slice_project_opcode::materialize;
        }
        if (function->funcid == oids.seq_from_letter_array_infer_oid ||
            function->funcid == oids.seq_from_letter_array_named_oid) {
            return slice_project_opcode::bridge;
        }
        return std::nullopt;
    }

    /** @brief Identify a supported sequence, function, operator, or subscript expression. */
    [[nodiscard]] std::optional<slice_project_opcode>
    slice_project_node_opcode(Node* node, const bioseq_stub_oids& oids) noexcept {
        if (node == nullptr)
            return std::nullopt;
        if (IsA(node, SubscriptingRef)) {
            auto* subscript = reinterpret_cast<SubscriptingRef*>(node);
            if (subscript->refcontainertype != oids.bioseq_seq_type_oid) {
                return std::nullopt;
            }
            /* PG17 represents an omitted bound as NIL or a single null cell. Leave
             * lists with multiple bounds to the sequence subscript code, which reports
             * the unsupported shape */
            if ((subscript->reflowerindexpr != NIL &&
                 list_length(subscript->reflowerindexpr) != 1) ||
                (subscript->refupperindexpr != NIL &&
                 list_length(subscript->refupperindexpr) != 1)) {
                return std::nullopt;
            }
            return subscript->reflowerindexpr == NIL ? slice_project_opcode::element
                                                     : slice_project_opcode::slice;
        }
        if (FuncExpr* function = try_cast_pg_node<FuncExpr>(node)) {
            return slice_project_function_opcode(function, oids);
        }
        if (OpExpr* operation = try_cast_pg_node<OpExpr>(node)) {
            /* SQL concatenation uses OpExpr. Check its catalog operator/function pair,
             * result type, and two arguments before treating it as sequence concat. */
            if (OidIsValid(operation->opno) && OidIsValid(operation->opfuncid) &&
                get_opcode(operation->opno) == operation->opfuncid &&
                slice_project_concat_oid(operation->opfuncid, oids) &&
                operation->opresulttype == oids.bioseq_seq_type_oid && !operation->opretset &&
                list_length(operation->args) == 2) {
                return slice_project_opcode::concat;
            }
        }
        return std::nullopt;
    }

    /** @brief Returns FuncExpr or OpExpr argument lists, or NIL for other node kinds. */
    [[nodiscard]] List* slice_project_call_arguments(Node* node) noexcept {
        if (FuncExpr* function = try_cast_pg_node<FuncExpr>(node)) {
            return function->args;
        }
        if (OpExpr* operation = try_cast_pg_node<OpExpr>(node)) {
            return operation->args;
        }
        return NIL;
    }

    /* INSERT can wrap a sequence in an implicit typmod cast. With constant cast
     * arguments, trace the source expression through that wrapper  so the plan
     * records the lazy sequence work before the value is packed for storage.
     * Explicit casts and other function calls keep their own expression node. */
    [[nodiscard]] inline Node*
    slice_project_typmod_cast_source(Node* node, const bioseq_stub_oids& oids) noexcept {
        FuncExpr* function = try_cast_pg_node<FuncExpr>(node);
        if (function == nullptr || function->funcid != oids.seq_cast_oid ||
            function->funcresulttype != oids.bioseq_seq_type_oid || function->funcretset ||
            OidIsValid(function->funcvariadic) || function->funcformat != COERCE_IMPLICIT_CAST ||
            list_length(function->args) != 3)
            return nullptr;
        Node* source = reinterpret_cast<Node*>(linitial(function->args));
        Const* target_typmod =
            try_cast_pg_node<Const>(reinterpret_cast<Node*>(lsecond(function->args)));
        Const* explicit_cast =
            try_cast_pg_node<Const>(reinterpret_cast<Node*>(lthird(function->args)));
        if (source == nullptr || exprType(source) != oids.bioseq_seq_type_oid ||
            target_typmod == nullptr || target_typmod->constisnull ||
            target_typmod->consttype != INT4OID || DatumGetInt32(target_typmod->constvalue) < 0 ||
            explicit_cast == nullptr || explicit_cast->constisnull ||
            explicit_cast->consttype != BOOLOID || DatumGetBool(explicit_cast->constvalue))
            return nullptr;
        return source;
    }
}

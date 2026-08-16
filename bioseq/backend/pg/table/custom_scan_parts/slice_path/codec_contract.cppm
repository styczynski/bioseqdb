/**
 * @file
 * @brief Owns SliceProject opcodes, codec fields, and cost estimates.
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

export module bioseq.postgres.table:custom_scan_slice_path_codec_contract;

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

using namespace bioseq::postgres::support;

export {

    /* ========================================================================
     * Lazy sequence projection
     *
     * The executable expression remains PostgreSQL's targetlist expression.  The
     * codec below is deliberately symbolic: it records the lazy sequence algebra
     * for EXPLAIN and validation while core ExprState/ProjectionInfo preserves
     * CASE, NULL, STRICT, volatility and error semantics.  Every serialized value
     * is a PostgreSQL Node, so copy/equal/out/read and setrefs remain core-owned.
     * ====================================================================== */

    /** @brief Opcode tag for one node in the lazy SliceProject expression tree. */
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

    /** @brief Magic integer written to the first SliceProject plan header field (BSP1). */
    inline constexpr int k_bioseq_slice_project_magic = 0x42535031; /* BSP1 */
    /** @brief Serialized SliceProject codec version carried in the plan header. */
    inline constexpr int k_bioseq_slice_project_codec_version = 3;
    /** @brief Expected field count for the SliceProject plan header list. */
    inline constexpr int k_bioseq_slice_project_header_fields = 10;
    /** @brief Expected field count for each SliceProject producer node record. */
    inline constexpr int k_bioseq_slice_project_node_fields = 11;
    /** @brief Expected field count for each SliceProject materialization boundary record. */
    inline constexpr int k_bioseq_slice_project_record_fields = 9;

    /** @brief Field index within the serialized SliceProject plan header list. */
    enum slice_project_header_index : int {
        k_slice_header_magic = 0,
        k_slice_header_version = 1,
        k_slice_header_boundary_count = 2,
        k_slice_header_expression_count = 3,
        k_slice_header_piece_boundaries = 4,
        k_slice_header_source_pieces = 5,
        k_slice_header_planned_symbols = 6,
        k_slice_header_planned_bytes = 7,
        k_slice_header_producer_count = 8,
        k_slice_header_fallback_reason = 9,
    };

    /** @brief Field index within one SliceProject materialization boundary record. */
    enum slice_project_record_index : int {
        k_slice_record_boundary_id = 0,
        k_slice_record_producer_id = 1,
        k_slice_record_expression_index = 2,
        k_slice_record_dependencies = 3,
        k_slice_record_mechanism = 4,
        k_slice_record_reason = 5,
        k_slice_record_estimated_symbols = 6,
        k_slice_record_encoding = 7,
        k_slice_record_cache_scope = 8,
    };

    /** @brief Field index within one SliceProject producer node record. */
    enum slice_project_node_index : int {
        k_slice_node_stable_id = 0,
        k_slice_node_opcode = 1,
        k_slice_node_dependencies = 2,
        k_slice_node_expression_index = 3,
        k_slice_node_lower_expression_index = 4,
        k_slice_node_upper_expression_index = 5,
        k_slice_node_options_expression_index = 6,
        k_slice_node_transform = 7,
        k_slice_node_mechanism = 8,
        k_slice_node_reason = 9,
        k_slice_node_flags = 10,
    };

    /** @brief Bit flags attached to a SliceProject IR node. */
    enum slice_project_node_flags : int {
        /** @brief Node may change per evaluation and cannot be cached across calls. */
        k_slice_node_volatile = 1 << 0,
        /** @brief Expression shape is not represented in the lazy codec. */
        k_slice_node_opaque = 1 << 1,
    };

    /** @brief Builds a non-null INT8 Const for codec fields, clamping @p value to INT8 range. */
    [[nodiscard]] Const* slice_project_int8_const(std::uint64_t value) {
        const std::uint64_t clamped =
            std::min<std::uint64_t>(
                value,
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
        return makeConst(INT8OID,
                         -1,
                         InvalidOid,
                         sizeof(std::int64_t),
                         Int64GetDatum(static_cast<std::int64_t>(clamped)),
                         false,
                         true);
    }

    /** @brief Decodes a non-negative uint64 from an INT8 Const node and errors on corruption. */
    [[nodiscard]] std::uint64_t slice_project_int8_value(Node* node) {
        Const* value = try_cast_pg_node<Const>(node);
        if (value == nullptr || value->constisnull || value->consttype != INT8OID) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject uint64 codec value is invalid")));
        }
        const std::int64_t decoded = DatumGetInt64(value->constvalue);
        if (decoded < 0) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject uint64 codec value is negative")));
        }
        return static_cast<std::uint64_t>(decoded);
    }

    /** @brief Returns a stable EXPLAIN label for @p mechanism. */
    [[nodiscard]] const char*
    slice_project_mechanism_name(materialization_mechanism mechanism) noexcept {
        switch (mechanism) {
        case materialization_mechanism::backing_copy:
            return "backing copy";
        case materialization_mechanism::packed_full:
            return "packed full";
        case materialization_mechanism::packed_region:
            return "packed region";
        case materialization_mechanism::text:
            return "text";
        case materialization_mechanism::binary:
            return "binary";
        case materialization_mechanism::letter_array:
            return "letter array";
        }
        return "invalid";
    }

    /** @brief Returns a stable EXPLAIN label for @p reason. */
    [[nodiscard]] const char* slice_project_reason_name(materialization_reason reason) noexcept {
        switch (reason) {
        case materialization_reason::executor_copy:
            return "executor copy";
        case materialization_reason::heap_tuple:
            return "heap tuple";
        case materialization_reason::index_tuple:
            return "index tuple";
        case materialization_reason::mmsearch_target:
            return "mmsearch target";
        case materialization_reason::mmsearch_query:
            return "mmsearch query";
        case materialization_reason::resident_algorithm:
            return "resident algorithm";
        case materialization_reason::sql_conversion:
            return "SQL conversion";
        case materialization_reason::client_output:
            return "client output";
        }
        return "invalid";
    }

    /** @brief Returns the encoding family name shown in EXPLAIN for @p mechanism. */
    [[nodiscard]] const char*
    slice_project_encoding_name(materialization_mechanism mechanism) noexcept {
        switch (mechanism) {
        case materialization_mechanism::packed_full:
        case materialization_mechanism::packed_region:
            return "alphabet packed";
        case materialization_mechanism::text:
            return "database encoding text";
        case materialization_mechanism::binary:
            return "bioseq binary";
        case materialization_mechanism::letter_array:
            return "bioseq.letter[]";
        case materialization_mechanism::backing_copy:
            return "source encoding";
        }
        return "unknown";
    }

    /** @brief True when @p oid is a registered sequence concat function variant. */
    [[nodiscard]] bool slice_project_concat_oid(Oid oid, const bioseq_stub_oids& oids) noexcept {
        return oid == oids.seq_concat_oid || oid == oids.seq_array_cat_oid ||
               oid == oids.seq_concat_text_oid || oid == oids.text_concat_seq_oid ||
               oid == oids.seq_concat_varchar_oid || oid == oids.varchar_concat_seq_oid ||
               oid == oids.seq_concat_text_array_oid || oid == oids.text_array_concat_seq_oid ||
               oid == oids.seq_concat_letter_array_oid || oid == oids.letter_array_concat_seq_oid;
    }

    /** @brief Maps a FuncExpr to a slice opcode when it implements a known lazy operator. */
    [[nodiscard]] std::optional<slice_project_opcode>
    slice_project_function_opcode(FuncExpr* function, const bioseq_stub_oids& oids) noexcept {
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

    /** @brief Maps an expression node to a slice opcode when shape and OIDs match. */
    [[nodiscard]] std::optional<slice_project_opcode>
    slice_project_node_opcode(Node* node, const bioseq_stub_oids& oids) noexcept {
        if (node == nullptr)
            return std::nullopt;
        if (IsA(node, SubscriptingRef)) {
            auto* subscript = reinterpret_cast<SubscriptingRef*>(node);
            if (subscript->refcontainertype != oids.bioseq_seq_type_oid) {
                return std::nullopt;
            }
            /* A valid one-dimensional PG17 bound coordinate is NIL or exactly
             * one cell (whose payload may be nullptr for an omitted bound).
             * Leave wider/corrupt shapes opaque so the native subscript path
             * raises its normative unsupported-shape error. */
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
            /* PostgreSQL represents SQL operator syntax as OpExpr, even when the
             * implementation is one of the exact registered concat functions.  Do
             * not recognize an arbitrary operator merely because opfuncid happens
             * to be populated: validate the binary catalog operator/function identity
             * and the sequence result shape first. */
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

    /* PostgreSQL wraps INSERT projection values in the registered same-type
     * typmod cast.  The cast preserves logical length and the child lazy recipe;
     * the destination slot is still the one unavoidable heap boundary.  Admit
     * only the exact implicit, planner-constant cast shape.  Explicit calls and
     * unrelated sequence-returning functions remain opaque producer leaves. */
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

    /* PG17 represents an omitted slice bound as either NIL or a one-cell List
     * whose cell contains nullptr.  Both forms are canonical omission; no nullptr
     * may enter custom_exprs and deparse as an empty string. */
    [[nodiscard]] inline Node* slice_project_subscript_bound(List* expressions) {
        if (expressions == NIL)
            return nullptr;
        if (list_length(expressions) != 1) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject slice bound list is invalid"),
                     errdetail("bound_count=%d expected_count=1", list_length(expressions))));
        }
        return reinterpret_cast<Node*>(linitial(expressions));
    }

    /** @brief Estimates output symbol count for planner costing and returns 4096 when unknown. */
    [[nodiscard]] std::uint64_t slice_project_estimated_symbols(Node* node,
                                                                const bioseq_stub_oids& oids) {
        if (node == nullptr)
            return 0u;
        if (Node* source = slice_project_typmod_cast_source(node, oids)) {
            return slice_project_estimated_symbols(source, oids);
        }
        if (IsA(node, SubscriptingRef)) {
            auto* subscript = reinterpret_cast<SubscriptingRef*>(node);
            if (subscript->reflowerindexpr == NIL)
                return 1u;
            if (list_length(subscript->reflowerindexpr) != 1 ||
                (subscript->refupperindexpr != NIL &&
                 list_length(subscript->refupperindexpr) != 1)) {
                return 4096u;
            }
            Node* lower = slice_project_subscript_bound(subscript->reflowerindexpr);
            Node* upper = slice_project_subscript_bound(subscript->refupperindexpr);
            /** @brief Returns a compile-time INT8 bound when @p expression is a non-null constant. */
            auto constant_bound = [](Node* expression) -> std::optional<std::int64_t> {
                Const* constant = try_cast_pg_node<Const>(expression);
                if (constant == nullptr || constant->constisnull || constant->consttype != INT8OID)
                    return std::nullopt;
                return DatumGetInt64(constant->constvalue);
            };
            const auto lo = constant_bound(lower);
            const auto hi = constant_bound(upper);
            if (lower == nullptr && hi.has_value() && *hi >= 0) {
                return static_cast<std::uint64_t>(*hi);
            }
            if (upper == nullptr && lo.has_value() && *lo < 0) {
                return bioseq::storage::negative_magnitude(*lo);
            }
            if (lo.has_value() && hi.has_value() && *hi >= *lo) {
                if (*lo >= 0) {
                    return static_cast<std::uint64_t>(*hi - *lo);
                }
                if (*hi < 0) {
                    return bioseq::storage::negative_magnitude(*lo) -
                           bioseq::storage::negative_magnitude(*hi);
                }
                /* A negative lower with a nonnegative upper depends on the
                 * unknown source length; do not mislabel an upper bound as an
                 * exact selected-range estimate. */
                return 4096u;
            }
            return 4096u;
        }
        if (FuncExpr* function = try_cast_pg_node<FuncExpr>(node)) {
            const auto opcode = slice_project_function_opcode(function, oids);
            if (!opcode.has_value())
                return 4096u;
            if (*opcode == slice_project_opcode::concat) {
                std::uint64_t total = 0u;
                for (Node* argument : postgres_list<Node>(function->args)) {
                    total = bioseq::common::saturating_add(
                        total, slice_project_estimated_symbols(argument, oids));
                }
                return total;
            }
            if (*opcode == slice_project_opcode::length ||
                *opcode == slice_project_opcode::cardinality)
                return 1u;
            if (function->args != NIL) {
                return slice_project_estimated_symbols(
                    reinterpret_cast<Node*>(linitial(function->args)), oids);
            }
        }
        if (OpExpr* operation = try_cast_pg_node<OpExpr>(node)) {
            const auto opcode = slice_project_node_opcode(node, oids);
            if (opcode.has_value() && *opcode == slice_project_opcode::concat) {
                std::uint64_t total = 0u;
                for (Node* argument : postgres_list<Node>(operation->args)) {
                    total = bioseq::common::saturating_add(
                        total, slice_project_estimated_symbols(argument, oids));
                }
                return total;
            }
        }
        return 4096u;
    }

    /** @brief Startup and per-input-tuple planner costs for an owning mmsearch path. */
    struct owned_mmsearch_cost {
        Cost startup = 0;
        Cost per_input_tuple = 0;
    };

    /* Cost only executable operands, in SQL argument order.  In particular the
     * rendezvous FuncExpr itself (whose support function deliberately costs
     * 100000) must never enter an owning path's cost tree. */
    [[nodiscard]] List* mmsearch_operand_cost_exprs(FuncExpr const* expression,
                                                    bioseq_stub_oids const& oids) {
        if (expression == nullptr || !is_mmsearch_funcid(expression->funcid, oids) ||
            (expression->funcid == oids.mmsearch_oid && list_length(expression->args) != 2) ||
            (expression->funcid == oids.mmsearch_with_opts_oid &&
             list_length(expression->args) != 3)) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("owned mmsearch operand cost shape is invalid")));
        }
        List* operands = NIL;
        for (Node* argument : postgres_list<Node>(expression->args)) {
            operands = lappend(operands, argument);
        }
        return operands;
    }

    /** @brief Estimates symbol count for one mmsearch operand expression. */
    [[nodiscard]] std::uint64_t estimate_mmsearch_operand_symbols(Node* expression,
                                                                  bioseq_stub_oids const& oids) {
        if (expression == nullptr)
            return 4096u;
        if (Const* constant = try_cast_pg_node<Const>(expression)) {
            if (constant->constisnull)
                return 0u;
            if (constant->consttype == oids.bioseq_seq_type_oid) {
                return classify_and_describe_sequence_header(constant->constvalue).bp_length;
            }
        }
        const auto opcode = slice_project_node_opcode(expression, oids);
        if (opcode.has_value()) {
            return slice_project_estimated_symbols(expression, oids);
        }
        return 4096u;
    }

    /** @brief Computes planner startup and per-tuple costs from length, batch, and byte estimates. */
    [[nodiscard]] owned_mmsearch_cost
    estimate_owned_mmsearch_cost(std::uint64_t target_length_estimate,
                                 std::uint64_t query_length_estimate,
                                 double input_rows,
                                 bool index_read_requirement,
                                 std::uint32_t planned_batch_size,
                                 std::uint64_t planned_materialized_bytes) noexcept {
        constexpr Cost maximum = std::numeric_limits<Cost>::max() / 32.0;
        /** @brief Clamps a non-negative planner cost to a finite ceiling. */
        auto finite_cost = [](long double value) noexcept -> Cost {
            constexpr long double limit =
                static_cast<long double>(std::numeric_limits<Cost>::max() / 32.0);
            if (!(value >= 0.0L))
                return 0;
            if (value >= limit)
                return static_cast<Cost>(limit);
            return static_cast<Cost>(value);
        };
        const std::uint32_t batch_size = std::max<std::uint32_t>(1u, planned_batch_size);
        const long double target_units = static_cast<long double>(
            target_length_estimate / 64u + (target_length_estimate % 64u != 0u));
        const long double query_units = static_cast<long double>(
            query_length_estimate / 64u + (query_length_estimate % 64u != 0u));
        const long double byte_pages =
            static_cast<long double>(planned_materialized_bytes / BLCKSZ) +
            (planned_materialized_bytes % BLCKSZ != 0u);
        const long double rows = std::isfinite(input_rows) && input_rows > 0.0
                                     ? static_cast<long double>(input_rows)
                                     : 0.0L;
        const long double batch_count =
            rows == 0.0L ? 1.0L : std::ceil(rows / static_cast<long double>(batch_size));
        const long double startup =
            static_cast<long double>(cpu_operator_cost) * (8.0L + query_units + batch_count) +
            (index_read_requirement ? static_cast<long double>(random_page_cost) : 0.0L);
        const long double per_input =
            static_cast<long double>(cpu_tuple_cost) +
            static_cast<long double>(cpu_operator_cost) * (8.0L + target_units + query_units) +
            static_cast<long double>(seq_page_cost) * byte_pages /
                static_cast<long double>(batch_size);
        owned_mmsearch_cost result{finite_cost(startup), finite_cost(per_input)};
        if (!std::isfinite(result.startup) || result.startup < 0) {
            result.startup = maximum;
        }
        if (!std::isfinite(result.per_input_tuple) || result.per_input_tuple < 0) {
            result.per_input_tuple = maximum;
        }
        return result;
    }

    /** @brief Mutable accumulator for SliceProject IR nodes, records, and plan totals during planning. */
    struct slice_project_ir_builder {
        const bioseq_stub_oids* oids = nullptr;
        CmdType command_type = CMD_UNKNOWN;
        PlannerInfo* planner_root = nullptr;
        Index target_leaf_support_relid = 0;
        List* target_leaf_support_vars = NIL;
        List* expressions = NIL;
        List* nodes = NIL;
        List* records = NIL;
        List* output_bindings = NIL;
        std::vector<Node*> producer_expressions{};
        std::vector<std::vector<std::uint32_t>> producer_dependencies{};
        std::vector<std::pair<Node*, std::uint32_t>> source_bindings{};
        bool source_bindings_only = false;
        std::uint32_t next_boundary_id = 1u;
        std::uint64_t piece_boundaries = 0u;
        std::uint64_t source_pieces = 0u;
        std::uint64_t planned_symbols = 0u;
        std::uint64_t planned_bytes = 0u;
        bool found = false;
    };

} // export custom-scan partition declarations

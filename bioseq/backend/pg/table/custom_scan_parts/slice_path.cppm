/**
 * @file
 * @brief Builds and validates SliceProject planner IR and callback contracts.
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

export module bioseq.postgres.table:custom_scan_slice_path;

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

    inline constexpr int k_bioseq_slice_project_magic = 0x42535031; /* BSP1 */
    inline constexpr int k_bioseq_slice_project_codec_version = 3;
    inline constexpr int k_bioseq_slice_project_header_fields = 10;
    inline constexpr int k_bioseq_slice_project_node_fields = 11;
    inline constexpr int k_bioseq_slice_project_record_fields = 9;

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

    enum slice_project_node_flags : int {
        k_slice_node_volatile = 1 << 0,
        k_slice_node_opaque = 1 << 1,
    };

    [[nodiscard]] Const* slice_project_int8_const(std::uint64_t value) {
        const std::uint64_t clamped =
            std::min<std::uint64_t>(value, static_cast<std::uint64_t>(INT64_MAX));
        return makeConst(INT8OID,
                         -1,
                         InvalidOid,
                         sizeof(std::int64_t),
                         Int64GetDatum(static_cast<std::int64_t>(clamped)),
                         false,
                         true);
    }

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

    [[nodiscard]] bool slice_project_concat_oid(Oid oid, const bioseq_stub_oids& oids) noexcept {
        return oid == oids.seq_concat_oid || oid == oids.seq_array_cat_oid ||
               oid == oids.seq_concat_text_oid || oid == oids.text_concat_seq_oid ||
               oid == oids.seq_concat_varchar_oid || oid == oids.varchar_concat_seq_oid ||
               oid == oids.seq_concat_text_array_oid || oid == oids.text_array_concat_seq_oid ||
               oid == oids.seq_concat_letter_array_oid || oid == oids.letter_array_concat_seq_oid;
    }

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

    [[nodiscard]] owned_mmsearch_cost
    estimate_owned_mmsearch_cost(std::uint64_t target_length_estimate,
                                 std::uint64_t query_length_estimate,
                                 double input_rows,
                                 bool index_read_requirement,
                                 std::uint32_t planned_batch_size,
                                 std::uint64_t planned_materialized_bytes) noexcept {
        constexpr Cost maximum = std::numeric_limits<Cost>::max() / 32.0;
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

    [[nodiscard]] int slice_project_add_expression(slice_project_ir_builder& builder,
                                                   Node* expression) {
        int index = 0;
        for (Node* candidate : postgres_list<Node>(builder.expressions)) {
            if (equal(candidate, expression))
                return index;
            ++index;
        }
        index = list_length(builder.expressions);
        builder.expressions = lappend(builder.expressions, copyObject(expression));
        return index;
    }

    [[nodiscard]] std::uint32_t slice_project_add_producer(slice_project_ir_builder& builder,
                                                           Node* expression,
                                                           bool distinct_consumer = false,
                                                           bool force_value = false) {
        check_stack_depth();
        if (expression == nullptr)
            return 0u;
        if (Node* source = slice_project_typmod_cast_source(expression, *builder.oids)) {
            return slice_project_add_producer(builder, source, distinct_consumer, force_value);
        }
        const auto operation = slice_project_node_opcode(expression, *builder.oids);
        if (exprType(expression) != builder.oids->bioseq_seq_type_oid && !operation.has_value() &&
            !force_value)
            return 0u;
        /* CSE is descriptive only, but even descriptive CSE would hide distinct
         * occurrences of a volatile expression from EXPLAIN. */
        if (!distinct_consumer && !contain_volatile_functions(expression)) {
            for (std::size_t index = 0u; index < builder.producer_expressions.size(); ++index) {
                if (equal(builder.producer_expressions[index], expression)) {
                    return static_cast<std::uint32_t>(index + 1u);
                }
            }
        }
        /* Assign children first so stable ids are deterministic and topological. */
        std::vector<std::uint32_t> dependencies;
        auto add_dependency = [&builder, &dependencies](Node* child, bool force_child) {
            const std::uint32_t dependency =
                slice_project_add_producer(builder, child, false, force_child);
            if (dependency == 0u ||
                std::find(dependencies.begin(), dependencies.end(), dependency) !=
                    dependencies.end())
                return;
            dependencies.push_back(dependency);
        };
        /* Opaque source expressions are PostgreSQL-evaluated leaves in this
         * descriptive graph.  Only recognized lazy operations expose producer
         * dependencies; otherwise a sequence-returning FuncExpr would be encoded
         * as source_expr while still carrying children, contradicting the codec. */
        if (operation.has_value() && IsA(expression, SubscriptingRef)) {
            auto* subscript = reinterpret_cast<SubscriptingRef*>(expression);
            add_dependency(reinterpret_cast<Node*>(subscript->refexpr), false);
        } else if (operation.has_value()) {
            const bool all_arguments_are_values = *operation == slice_project_opcode::concat ||
                                                  *operation == slice_project_opcode::bridge;
            for (Node* argument : postgres_list<Node>(slice_project_call_arguments(expression))) {
                if (exprType(argument) == builder.oids->bioseq_seq_type_oid ||
                    slice_project_node_opcode(argument, *builder.oids).has_value() ||
                    all_arguments_are_values) {
                    add_dependency(argument, all_arguments_are_values);
                }
            }
        }
        if (builder.producer_expressions.size() >= static_cast<std::size_t>(INT_MAX - 1)) {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("BioseqSliceProject producer graph is too large")));
        }
        const std::uint32_t stable_id =
            static_cast<std::uint32_t>(builder.producer_expressions.size() + 1u);
        const int expression_index = slice_project_add_expression(builder, expression);
        const slice_project_opcode opcode =
            operation.has_value() ? *operation
                                  : (IsA(expression, Const) ? slice_project_opcode::literal_expr
                                                            : slice_project_opcode::source_expr);
        int lower_expression_index = -1;
        int upper_expression_index = -1;
        if ((opcode == slice_project_opcode::slice || opcode == slice_project_opcode::element) &&
            IsA(expression, SubscriptingRef)) {
            auto* subscript = reinterpret_cast<SubscriptingRef*>(expression);
            if (Node* lower = slice_project_subscript_bound(subscript->reflowerindexpr)) {
                lower_expression_index = slice_project_add_expression(builder, lower);
            }
            if (Node* upper = slice_project_subscript_bound(subscript->refupperindexpr)) {
                upper_expression_index = slice_project_add_expression(builder, upper);
            }
        }
        int transform = 0;
        if (opcode == slice_project_opcode::reverse)
            transform = 1;
        if (opcode == slice_project_opcode::reverse_complement)
            transform = 3;
        int mechanism = -1;
        int reason = -1;
        if (opcode == slice_project_opcode::materialize) {
            auto* function = reinterpret_cast<FuncExpr*>(expression);
            mechanism = static_cast<int>(function->funcid == builder.oids->seq_to_array_oid
                                             ? materialization_mechanism::letter_array
                                             : materialization_mechanism::text);
            reason = static_cast<int>(materialization_reason::sql_conversion);
        }
        int flags = 0;
        if (contain_volatile_functions(expression))
            flags |= k_slice_node_volatile;
        if (!operation.has_value() && !IsA(expression, Var) && !IsA(expression, Const))
            flags |= k_slice_node_opaque;
        List* dependency_codec = NIL;
        for (const std::uint32_t dependency : dependencies) {
            dependency_codec = lappend(dependency_codec, makeInteger(static_cast<int>(dependency)));
        }
        List* node = list_make5(makeInteger(static_cast<int>(stable_id)),
                                makeInteger(static_cast<int>(opcode)),
                                dependency_codec,
                                makeInteger(expression_index),
                                makeInteger(lower_expression_index));
        node = lappend(node, makeInteger(upper_expression_index));
        node = lappend(node, makeInteger(-1)); /* optional mmsearch options Expr */
        node = lappend(node, makeInteger(transform));
        node = lappend(node, makeInteger(mechanism));
        node = lappend(node, makeInteger(reason));
        node = lappend(node, makeInteger(flags));
        builder.nodes = lappend(builder.nodes, node);
        if (opcode == slice_project_opcode::source_expr ||
            opcode == slice_project_opcode::literal_expr) {
            builder.source_pieces = bioseq::common::saturating_add(builder.source_pieces, 1u);
        }
        builder.producer_expressions.push_back(reinterpret_cast<Node*>(copyObject(expression)));
        builder.producer_dependencies.push_back(std::move(dependencies));
        return stable_id;
    }

    void slice_project_note_source_binding(slice_project_ir_builder& builder,
                                           Node* sequence_expression,
                                           std::uint32_t producer_id) {
        if (sequence_expression == nullptr || producer_id == 0u)
            return;
        constexpr int pull_flags =
            PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
        List* sources = pull_var_clause(sequence_expression, pull_flags);
        for (Node* source : postgres_list<Node>(sources)) {
            Var* var = try_cast_pg_node<Var>(source);
            if (var == nullptr || var->vartype != builder.oids->bioseq_seq_type_oid) {
                continue;
            }
            bool seen = false;
            for (auto& binding : builder.source_bindings) {
                if (!equal(binding.first, source))
                    continue;
                /* Zero is the deliberate ambiguity marker.  A source Datum cannot
                 * truthfully carry two simultaneous pre-projection producer ids. */
                if (binding.second != producer_id)
                    binding.second = 0u;
                seen = true;
                break;
            }
            if (!seen) {
                builder.source_bindings.emplace_back(reinterpret_cast<Node*>(copyObject(source)),
                                                     producer_id);
            }
        }
    }

    [[nodiscard]] bool slice_project_producer_depends_on(slice_project_ir_builder const& builder,
                                                         std::uint32_t producer_id,
                                                         std::uint32_t possible_ancestor) {
        if (producer_id == 0u || possible_ancestor == 0u ||
            producer_id > builder.producer_dependencies.size() || possible_ancestor >= producer_id)
            return false;
        std::vector<std::uint8_t> visited(builder.producer_dependencies.size() + 1u, 0u);
        std::vector<std::uint32_t> pending(builder.producer_dependencies[producer_id - 1u].begin(),
                                           builder.producer_dependencies[producer_id - 1u].end());
        while (!pending.empty()) {
            const std::uint32_t candidate = pending.back();
            pending.pop_back();
            if (candidate == possible_ancestor)
                return true;
            if (candidate == 0u || candidate > builder.producer_dependencies.size() ||
                visited[candidate] != 0u)
                continue;
            visited[candidate] = 1u;
            auto const& dependencies = builder.producer_dependencies[candidate - 1u];
            pending.insert(pending.end(), dependencies.begin(), dependencies.end());
        }
        return false;
    }

    void slice_project_add_boundary(slice_project_ir_builder& builder,
                                    Node* sequence_expression,
                                    std::uint32_t producer_id,
                                    materialization_mechanism mechanism,
                                    materialization_reason reason,
                                    bool preprojection) {
        if (sequence_expression == nullptr || producer_id == 0u)
            return;
        const int expression_index = slice_project_add_expression(builder, sequence_expression);
        for (Node* raw : postgres_list<Node>(builder.records)) {
            List* record = reinterpret_cast<List*>(raw);
            if (intVal(list_nth(record, k_slice_record_producer_id)) ==
                    static_cast<int>(producer_id) &&
                intVal(list_nth(record, k_slice_record_expression_index)) == expression_index &&
                intVal(list_nth(record, k_slice_record_mechanism)) == static_cast<int>(mechanism) &&
                intVal(list_nth(record, k_slice_record_reason)) == static_cast<int>(reason))
                return;
        }
        if (builder.next_boundary_id == UINT32_MAX) {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("BioseqSliceProject materialization DAG is too large")));
        }
        const std::uint64_t estimated_symbols =
            slice_project_estimated_symbols(sequence_expression, *builder.oids);
        List* dependencies = NIL;
        for (Node* raw : postgres_list<Node>(builder.records)) {
            List* previous = reinterpret_cast<List*>(raw);
            const auto previous_producer =
                static_cast<std::uint32_t>(intVal(list_nth(previous, k_slice_record_producer_id)));
            if (!slice_project_producer_depends_on(builder, producer_id, previous_producer))
                continue;
            dependencies = lappend(
                dependencies, makeInteger(intVal(list_nth(previous, k_slice_record_boundary_id))));
        }
        List* record = list_make5(makeInteger(static_cast<int>(builder.next_boundary_id++)),
                                  makeInteger(static_cast<int>(producer_id)),
                                  makeInteger(expression_index),
                                  dependencies,
                                  makeInteger(static_cast<int>(mechanism)));
        record = lappend(record, makeInteger(static_cast<int>(reason)));
        record = lappend(record, slice_project_int8_const(estimated_symbols));
        record = lappend(record, makeString(pstrdup(slice_project_encoding_name(mechanism))));
        record =
            lappend(record, makeString(pstrdup(preprojection ? "projection row" : "result datum")));
        builder.records = lappend(builder.records, record);
        builder.planned_symbols =
            bioseq::common::saturating_add(builder.planned_symbols, estimated_symbols);
        builder.planned_bytes =
            bioseq::common::saturating_add(builder.planned_bytes, estimated_symbols);
        if (preprojection) {
            slice_project_note_source_binding(builder, sequence_expression, producer_id);
        }
    }

    [[nodiscard]] std::uint32_t slice_project_build_ir_node(slice_project_ir_builder& builder,
                                                            Node* expression) {
        check_stack_depth();
        if (expression == nullptr)
            return 0u;
        const auto operation = slice_project_node_opcode(expression, *builder.oids);
        if (!operation.has_value()) {
            return slice_project_add_producer(builder, expression);
        }
        builder.found = true;
        const slice_project_opcode opcode = *operation;
        if (opcode == slice_project_opcode::slice || opcode == slice_project_opcode::concat ||
            opcode == slice_project_opcode::reverse ||
            opcode == slice_project_opcode::reverse_complement) {
            builder.piece_boundaries = bioseq::common::saturating_add(builder.piece_boundaries, 1u);
        }
        if (List* arguments = slice_project_call_arguments(expression); arguments != NIL) {
            for (Node* argument : postgres_list<Node>(arguments)) {
                if (slice_project_node_opcode(argument, *builder.oids).has_value()) {
                    (void)slice_project_build_ir_node(builder, argument);
                } else if (exprType(argument) == builder.oids->bioseq_seq_type_oid) {
                    (void)slice_project_add_producer(builder, argument);
                }
            }
            FuncExpr* function = try_cast_pg_node<FuncExpr>(expression);
            if (opcode == slice_project_opcode::materialize && function != nullptr) {
                Node* source = reinterpret_cast<Node*>(linitial(arguments));
                const std::uint32_t producer_id = slice_project_add_producer(builder, source);
                const bool as_array = function->funcid == builder.oids->seq_to_array_oid;
                slice_project_add_boundary(builder,
                                           source,
                                           producer_id,
                                           as_array ? materialization_mechanism::letter_array
                                                    : materialization_mechanism::text,
                                           materialization_reason::sql_conversion,
                                           true);
            }
        } else if (IsA(expression, SubscriptingRef)) {
            auto* subscript = reinterpret_cast<SubscriptingRef*>(expression);
            Node* source = reinterpret_cast<Node*>(subscript->refexpr);
            if (slice_project_node_opcode(source, *builder.oids).has_value()) {
                (void)slice_project_build_ir_node(builder, source);
            } else {
                (void)slice_project_add_producer(builder, source);
            }
        }
        return slice_project_add_producer(builder, expression);
    }

    struct slice_project_discovery {
        slice_project_ir_builder* builder = nullptr;
        bool found = false;
    };

    struct slice_project_discovery_visitor {
        [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                              pg_node_view node,
                                              slice_project_discovery& discovery) const {
            if (slice_project_node_opcode(node.get(), *discovery.builder->oids).has_value()) {
                if (contain_volatile_functions(node.get())) {
                    return pg_walk_step::skip_subtree;
                }
                (void)slice_project_build_ir_node(*discovery.builder, node.get());
                discovery.found = true;
                return pg_walk_step::skip_subtree;
            }
            return pg_walk_step::descend;
        }
    };

    [[nodiscard]] bool slice_project_discover_target(PathTarget* target,
                                                     slice_project_ir_builder& builder) {
        if (target == nullptr)
            return false;
        slice_project_discovery_visitor visitor;
        int resno = 1;
        for (Node* expression : postgres_list<Node>(target->exprs)) {
            slice_project_discovery discovery{&builder};
            walk_pg_expression_including_root(expression, discovery, visitor);
            if (discovery.found && exprType(expression) == builder.oids->bioseq_seq_type_oid) {
                const std::uint32_t producer_id = slice_project_add_producer(builder, expression);
                builder.output_bindings = lappend(
                    builder.output_bindings,
                    list_make2(makeInteger(resno), makeInteger(static_cast<int>(producer_id))));
                if (builder.command_type == CMD_INSERT) {
                    slice_project_add_boundary(builder,
                                               expression,
                                               producer_id,
                                               materialization_mechanism::packed_full,
                                               materialization_reason::heap_tuple,
                                               false);
                } else if (builder.command_type == CMD_SELECT) {
                    slice_project_add_boundary(builder,
                                               expression,
                                               producer_id,
                                               materialization_mechanism::text,
                                               materialization_reason::client_output,
                                               false);
                    slice_project_add_boundary(builder,
                                               expression,
                                               producer_id,
                                               materialization_mechanism::binary,
                                               materialization_reason::client_output,
                                               false);
                }
            }
            ++resno;
        }
        return builder.found;
    }

    bool bioseq_expr_uses_lazy_sequence_projection(Node* node) {
        if (node == nullptr)
            return false;
        const bioseq_stub_oids* oids = try_get_cached_stub_oids();
        if (oids == nullptr)
            return false;
        slice_project_ir_builder builder{.oids = oids};
        slice_project_discovery discovery{&builder};
        slice_project_discovery_visitor visitor;
        walk_pg_expression_including_root(node, discovery, visitor);
        return discovery.found;
    }

    enum class slice_dsm_publish_state : std::uint8_t {
        never_launched = 0u,
        attached_running = 1u,
        published_complete = 2u,
        published_incomplete = 3u,
    };

    struct slice_dsm_header {
        std::uint32_t magic;
        std::uint16_t codec_version;
        std::uint16_t header_bytes;
        std::uint32_t recipe_count;
        std::uint32_t slot_count;
        std::uint64_t cycle;
        std::uint64_t slot_stride;
    };

    struct slice_dsm_worker_header {
        std::uint64_t cycle;
        slice_dsm_publish_state publish_state;
        std::uint8_t overflowed;
        std::array<std::byte, 6> reserved;
    };

    static_assert(sizeof(slice_dsm_header) == 32u);
    static_assert(sizeof(slice_dsm_worker_header) == 16u);
    static_assert(std::is_trivially_copyable_v<slice_dsm_header>);
    static_assert(std::is_trivially_copyable_v<slice_dsm_worker_header>);
    static_assert(std::is_standard_layout_v<slice_dsm_header>);
    static_assert(std::is_standard_layout_v<slice_dsm_worker_header>);

    inline constexpr std::uint32_t k_bioseq_slice_dsm_magic = 0x42534432u; /* BSD2 */
    inline constexpr std::uint16_t k_bioseq_slice_dsm_version = 1u;

    struct slice_dsm_detach_state {
        std::uint32_t refcount;
        bool callback_armed;
        std::array<std::byte, 3> reserved;
        ParallelContext* pcxt;
        slice_dsm_header* mapped_header;
        slice_trace_attachment* attachment;
        std::uint64_t expected_cycle;
    };
    static_assert(std::is_standard_layout_v<slice_dsm_detach_state>);

    struct slice_project_output_binding {
        AttrNumber resno;
        std::uint32_t producer_value_id;
    };

    struct slice_project_dsm_merge_audit;

    struct slice_project_runtime {
        std::uint64_t rows_published = 0u;
        std::uint64_t rescans = 0u;
        std::uint64_t planned_symbols = 0u;
        std::uint64_t planned_bytes = 0u;
        std::uint32_t boundary_count = 0u;
        std::uint32_t output_binding_count = 0u;
        slice_project_output_binding* output_bindings = nullptr;
        slice_materialization_boundary* boundaries = nullptr;
        slice_dsm_counter* counters = nullptr;
        slice_materialization_runtime trace_runtime{};
        slice_trace_attachment* attachment = nullptr;
        slice_async_owner_guard owner_guard{};
        slice_async_job_slot* async_jobs = nullptr;
        slice_dsm_counter* async_job_counters = nullptr;
        bioseq::storage::pg_sequence_source_route_set source_routes{};
        bool owner_initialized = false;
        bool source_routes_acquired = false;
        MemoryContextCallback source_routes_callback{};
        bool source_routes_callback_registered = false;
        slice_dsm_header* mapped_header = nullptr;
        slice_dsm_worker_header* mapped_worker = nullptr;
        std::uint64_t mapped_cycle = 0u;
        slice_dsm_detach_state* detach_state = nullptr;
        bool trace_initialization_attempted = false;
        bool collect_actual = false;
        bool timing = false;
#ifdef BIOSEQ_TEST_INDEXES
        bool dsm_test_fixture_applied = false;
        bool dsm_test_fixture_validated = false;
        bool dsm_test_async_validated = false;
        int dsm_test_fixture_mode = k_slice_dsm_test_none;
        std::uint32_t dsm_test_checks = 0u;
        std::uint32_t dsm_test_slot_headers_read = 0u;
        std::uint32_t dsm_test_counter_records_read = 0u;
        std::uint32_t dsm_test_cleanup_callbacks = 0u;
        slice_project_dsm_merge_audit* dsm_test_reinitialize_audit = nullptr;
#endif
    };

    /* slice_async_max_live_jobs is the one normative concurrency bound. Released
     * slots advance their generation and are reused across arbitrarily many
     * batches without multiplying every trace boundary by dispatch capacity. */

    struct slice_project_state {
        CustomScanState custom_scan_state;
        slice_project_runtime runtime;
    };
    static_assert(std::is_standard_layout_v<slice_project_state>);
    static_assert(offsetof(slice_project_state, custom_scan_state) == 0);

    [[nodiscard]] bool
    slice_project_rearm_worker_for_rescan_noexcept(slice_project_state* state) noexcept;

    [[nodiscard]] CustomExecMethods const& bioseq_slice_project_exec_methods() noexcept;
    [[nodiscard]] CustomScanMethods const& bioseq_slice_project_scan_methods() noexcept;
    /* A nested SliceProject must initialize its child's trace before it can bind
     * the parent projector, while child initialization calls back through the
     * parent-aware binding path. This forward declaration closes that genuine
     * initialization cycle without moving either executor stage out of order. */
    void slice_project_ensure_trace_initialized(CustomScanState* custom);
    void slice_project_reinitialize_dsm(CustomScanState* custom,
                                        ParallelContext* pcxt,
                                        void* coordinate);

    [[nodiscard]] List* slice_project_header(const slice_project_ir_builder& builder) {
        List* header = list_make5(makeInteger(k_bioseq_slice_project_magic),
                                  makeInteger(k_bioseq_slice_project_codec_version),
                                  makeInteger(list_length(builder.records)),
                                  makeInteger(list_length(builder.expressions)),
                                  slice_project_int8_const(builder.piece_boundaries));
        header = lappend(header, slice_project_int8_const(builder.source_pieces));
        header = lappend(header, slice_project_int8_const(builder.planned_symbols));
        header = lappend(header, slice_project_int8_const(builder.planned_bytes));
        header = lappend(header, makeInteger(list_length(builder.nodes)));
        header = lappend(header, makeString(pstrdup("none")));
        return header;
    }

    [[nodiscard]] List* slice_project_plan_header(CustomScan* scan) {
        if (scan == nullptr || list_length(scan->custom_private) != 4)
            return NIL;
        Node* header = reinterpret_cast<Node*>(linitial(scan->custom_private));
        return header != nullptr && IsA(header, List) ? reinterpret_cast<List*>(header) : NIL;
    }

    [[nodiscard]] List* slice_project_plan_nodes(CustomScan* scan) {
        if (scan == nullptr || list_length(scan->custom_private) != 4)
            return NIL;
        Node* nodes = reinterpret_cast<Node*>(lsecond(scan->custom_private));
        return nodes != nullptr && IsA(nodes, List) ? reinterpret_cast<List*>(nodes) : NIL;
    }

    [[nodiscard]] List* slice_project_plan_records(CustomScan* scan) {
        if (scan == nullptr || list_length(scan->custom_private) != 4)
            return NIL;
        Node* records = reinterpret_cast<Node*>(lthird(scan->custom_private));
        return records != nullptr && IsA(records, List) ? reinterpret_cast<List*>(records) : NIL;
    }

    [[nodiscard]] List* slice_project_plan_output_bindings(CustomScan* scan) {
        if (scan == nullptr || list_length(scan->custom_private) != 4)
            return NIL;
        Node* bindings = reinterpret_cast<Node*>(lfourth(scan->custom_private));
        return bindings != nullptr && IsA(bindings, List) ? reinterpret_cast<List*>(bindings) : NIL;
    }

    void slice_project_validate_codec(CustomScan* scan) {
        List* header = slice_project_plan_header(scan);
        List* nodes = slice_project_plan_nodes(scan);
        List* records = slice_project_plan_records(scan);
        List* output_bindings = slice_project_plan_output_bindings(scan);
        if (header == NIL || list_length(header) != k_bioseq_slice_project_header_fields ||
            !IsA(reinterpret_cast<Node*>(list_nth(header, k_slice_header_magic)), Integer) ||
            !IsA(reinterpret_cast<Node*>(list_nth(header, k_slice_header_version)), Integer) ||
            !IsA(reinterpret_cast<Node*>(list_nth(header, k_slice_header_boundary_count)),
                 Integer) ||
            !IsA(reinterpret_cast<Node*>(list_nth(header, k_slice_header_expression_count)),
                 Integer) ||
            !IsA(reinterpret_cast<Node*>(list_nth(header, k_slice_header_producer_count)),
                 Integer) ||
            !IsA(reinterpret_cast<Node*>(list_nth(header, k_slice_header_fallback_reason)),
                 String) ||
            intVal(list_nth(header, k_slice_header_magic)) != k_bioseq_slice_project_magic ||
            intVal(list_nth(header, k_slice_header_version)) !=
                k_bioseq_slice_project_codec_version) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject plan codec header is invalid"),
                     errdetail("header_fields=%d expected_fields=%d",
                               list_length(header),
                               k_bioseq_slice_project_header_fields)));
        }
        const int boundary_count = intVal(list_nth(header, k_slice_header_boundary_count));
        const int expression_count = intVal(list_nth(header, k_slice_header_expression_count));
        const int producer_count = intVal(list_nth(header, k_slice_header_producer_count));
        if (boundary_count < 0 || boundary_count != list_length(records) || expression_count < 0 ||
            expression_count != list_length(scan->custom_exprs) || producer_count < 1 ||
            producer_count != list_length(nodes)) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject plan codec cardinality is invalid"),
                     errdetail("boundary_count=%d record_count=%d "
                               "expression_count=%d custom_expression_count=%d "
                               "producer_count=%d node_count=%d",
                               boundary_count,
                               list_length(records),
                               expression_count,
                               list_length(scan->custom_exprs),
                               producer_count,
                               list_length(nodes))));
        }
        (void)slice_project_int8_value(
            reinterpret_cast<Node*>(list_nth(header, k_slice_header_piece_boundaries)));
        (void)slice_project_int8_value(
            reinterpret_cast<Node*>(list_nth(header, k_slice_header_source_pieces)));
        (void)slice_project_int8_value(
            reinterpret_cast<Node*>(list_nth(header, k_slice_header_planned_symbols)));
        (void)slice_project_int8_value(
            reinterpret_cast<Node*>(list_nth(header, k_slice_header_planned_bytes)));

        int expected_stable_id = 1;
        for (Node* raw_node : postgres_list<Node>(nodes)) {
            List* node = raw_node != nullptr && IsA(raw_node, List)
                             ? reinterpret_cast<List*>(raw_node)
                             : NIL;
            if (node == NIL || list_length(node) != k_bioseq_slice_project_node_fields) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqSliceProject IR node is invalid"),
                         errdetail("stable_id=%d node_fields=%d expected_fields=%d",
                                   expected_stable_id,
                                   list_length(node),
                                   k_bioseq_slice_project_node_fields)));
            }
            constexpr std::array<int, 9> integer_fields{k_slice_node_stable_id,
                                                        k_slice_node_opcode,
                                                        k_slice_node_expression_index,
                                                        k_slice_node_lower_expression_index,
                                                        k_slice_node_upper_expression_index,
                                                        k_slice_node_options_expression_index,
                                                        k_slice_node_transform,
                                                        k_slice_node_mechanism,
                                                        k_slice_node_reason};
            for (const int field : integer_fields) {
                Node* value = reinterpret_cast<Node*>(list_nth(node, field));
                if (value == nullptr || !IsA(value, Integer)) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("BioseqSliceProject IR integer field is invalid"),
                             errdetail("stable_id=%d field=%d node_tag=%d",
                                       expected_stable_id,
                                       field,
                                       value == nullptr ? 0 : static_cast<int>(nodeTag(value)))));
                }
            }
            Node* flags_node = reinterpret_cast<Node*>(list_nth(node, k_slice_node_flags));
            Node* dependencies_node =
                reinterpret_cast<Node*>(list_nth(node, k_slice_node_dependencies));
            if (flags_node == nullptr || !IsA(flags_node, Integer) ||
                (dependencies_node != nullptr && !IsA(dependencies_node, List))) {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject IR topology field is invalid"),
                     errdetail("stable_id=%d flags_tag=%d dependencies_tag=%d",
                               expected_stable_id,
                               flags_node == nullptr ? 0 : static_cast<int>(nodeTag(flags_node)),
                               dependencies_node == nullptr
                                   ? 0
                                   : static_cast<int>(nodeTag(dependencies_node)))));
            }
            List* dependencies =
                dependencies_node == nullptr ? NIL : reinterpret_cast<List*>(dependencies_node);
            const int stable_id = intVal(list_nth(node, k_slice_node_stable_id));
            const int opcode = intVal(list_nth(node, k_slice_node_opcode));
            const int expression_index = intVal(list_nth(node, k_slice_node_expression_index));
            const int lower_expression_index =
                intVal(list_nth(node, k_slice_node_lower_expression_index));
            const int upper_expression_index =
                intVal(list_nth(node, k_slice_node_upper_expression_index));
            const int options_expression_index =
                intVal(list_nth(node, k_slice_node_options_expression_index));
            const int transform = intVal(list_nth(node, k_slice_node_transform));
            const int mechanism = intVal(list_nth(node, k_slice_node_mechanism));
            const int reason = intVal(list_nth(node, k_slice_node_reason));
            const int flags = intVal(flags_node);
            auto valid_optional_expression_index = [expression_count](int index) {
                return index == -1 || (index >= 0 && index < expression_count);
            };
            if (stable_id != expected_stable_id ||
                opcode < static_cast<int>(slice_project_opcode::source_expr) ||
                opcode > static_cast<int>(slice_project_opcode::materialize) ||
                expression_index < 0 || expression_index >= expression_count ||
                !valid_optional_expression_index(lower_expression_index) ||
                !valid_optional_expression_index(upper_expression_index) ||
                !valid_optional_expression_index(options_expression_index) || transform < 0 ||
                transform > 3 || flags < 0 ||
                (flags & ~(k_slice_node_volatile | k_slice_node_opaque)) != 0) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqSliceProject IR node metadata is invalid"),
                         errdetail("stable_id=%d expected_id=%d opcode=%d "
                                   "expression=%d lower=%d upper=%d options=%d "
                                   "transform=%d flags=%d",
                                   stable_id,
                                   expected_stable_id,
                                   opcode,
                                   expression_index,
                                   lower_expression_index,
                                   upper_expression_index,
                                   options_expression_index,
                                   transform,
                                   flags)));
            }
            Bitmapset* seen_dependencies = nullptr;
            for (Node* dependency : postgres_list<Node>(dependencies)) {
                if (dependency == nullptr || !IsA(dependency, Integer) || intVal(dependency) <= 0 ||
                    intVal(dependency) >= stable_id ||
                    bms_is_member(intVal(dependency), seen_dependencies)) {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqSliceProject IR dependency is not topological"),
                         errdetail(
                             "stable_id=%d dependency_id=%d dependency_tag=%d",
                             stable_id,
                             dependency != nullptr && IsA(dependency, Integer) ? intVal(dependency)
                                                                               : 0,
                             dependency == nullptr ? 0 : static_cast<int>(nodeTag(dependency)))));
                }
                seen_dependencies = bms_add_member(seen_dependencies, intVal(dependency));
            }
            if (seen_dependencies != nullptr)
                bms_free(seen_dependencies);
            const auto typed_opcode = static_cast<slice_project_opcode>(opcode);
            const int dependency_count = list_length(dependencies);
            const bool leaf = typed_opcode == slice_project_opcode::source_expr ||
                              typed_opcode == slice_project_opcode::literal_expr;
            const bool unary = typed_opcode == slice_project_opcode::element ||
                               typed_opcode == slice_project_opcode::slice ||
                               typed_opcode == slice_project_opcode::reverse ||
                               typed_opcode == slice_project_opcode::reverse_complement ||
                               typed_opcode == slice_project_opcode::length ||
                               typed_opcode == slice_project_opcode::cardinality ||
                               typed_opcode == slice_project_opcode::materialize;
            const bool materialize = typed_opcode == slice_project_opcode::materialize;
            const bool bounds_valid =
                (typed_opcode == slice_project_opcode::slice ||
                 typed_opcode == slice_project_opcode::element)
                    ? (typed_opcode == slice_project_opcode::element
                           ? lower_expression_index == -1 && upper_expression_index >= 0
                           : true)
                    : lower_expression_index == -1 && upper_expression_index == -1;
            const int expected_transform =
                typed_opcode == slice_project_opcode::reverse
                    ? 1
                    : (typed_opcode == slice_project_opcode::reverse_complement ? 3 : 0);
            const bool boundary_metadata_valid =
                materialize
                    ? mechanism >= static_cast<int>(materialization_mechanism::backing_copy) &&
                          mechanism <= static_cast<int>(materialization_mechanism::letter_array) &&
                          reason == static_cast<int>(materialization_reason::sql_conversion)
                    : mechanism == -1 && reason == -1;
            if ((leaf && dependency_count != 0) || (unary && dependency_count != 1) ||
                (typed_opcode == slice_project_opcode::concat && dependency_count < 1) ||
                (typed_opcode == slice_project_opcode::bridge && dependency_count < 1) ||
                !bounds_valid || transform != expected_transform || !boundary_metadata_valid ||
                options_expression_index != -1) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqSliceProject IR opcode contract is invalid"),
                         errdetail("stable_id=%d opcode=%d dependencies=%d "
                                   "lower=%d upper=%d options=%d transform=%d "
                                   "mechanism=%d reason=%d",
                                   stable_id,
                                   opcode,
                                   dependency_count,
                                   lower_expression_index,
                                   upper_expression_index,
                                   options_expression_index,
                                   transform,
                                   mechanism,
                                   reason)));
            }
            ++expected_stable_id;
        }

        std::uint32_t expected_boundary_id = 1u;
        int record_index = 0;
        for (Node* raw_record : postgres_list<Node>(records)) {
            List* record = raw_record != nullptr && IsA(raw_record, List)
                               ? reinterpret_cast<List*>(raw_record)
                               : NIL;
            if (record == NIL || list_length(record) != k_bioseq_slice_project_record_fields) {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject plan codec record is invalid"),
                     errdetail("record_index=%d record_node_tag=%d "
                               "record_fields=%d expected_fields=%d",
                               record_index,
                               raw_record == nullptr ? 0 : static_cast<int>(nodeTag(raw_record)),
                               list_length(record),
                               k_bioseq_slice_project_record_fields)));
            }
            Node* boundary_id_node =
                reinterpret_cast<Node*>(list_nth(record, k_slice_record_boundary_id));
            Node* producer_id_node =
                reinterpret_cast<Node*>(list_nth(record, k_slice_record_producer_id));
            Node* expression_index_node =
                reinterpret_cast<Node*>(list_nth(record, k_slice_record_expression_index));
            Node* mechanism_node =
                reinterpret_cast<Node*>(list_nth(record, k_slice_record_mechanism));
            Node* reason_node = reinterpret_cast<Node*>(list_nth(record, k_slice_record_reason));
            if (boundary_id_node == nullptr || !IsA(boundary_id_node, Integer) ||
                producer_id_node == nullptr || !IsA(producer_id_node, Integer) ||
                expression_index_node == nullptr || !IsA(expression_index_node, Integer) ||
                mechanism_node == nullptr || !IsA(mechanism_node, Integer) ||
                reason_node == nullptr || !IsA(reason_node, Integer)) {
                auto node_tag_or_zero = [](Node* node) noexcept {
                    return node == nullptr ? 0 : static_cast<int>(nodeTag(node));
                };
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqSliceProject plan codec topology is invalid"),
                         errdetail("record_index=%d integer_field_tags: "
                                   "boundary_id=%d producer_id=%d expression_index=%d "
                                   "mechanism=%d reason=%d",
                                   record_index,
                                   node_tag_or_zero(boundary_id_node),
                                   node_tag_or_zero(producer_id_node),
                                   node_tag_or_zero(expression_index_node),
                                   node_tag_or_zero(mechanism_node),
                                   node_tag_or_zero(reason_node))));
            }
            const int boundary_id = intVal(boundary_id_node);
            const int producer_id = intVal(producer_id_node);
            const int expression_index = intVal(expression_index_node);
            const int mechanism = intVal(mechanism_node);
            const int reason = intVal(reason_node);
            Node* dependency_node =
                reinterpret_cast<Node*>(list_nth(record, k_slice_record_dependencies));
            const bool dependency_shape_valid =
                dependency_node == nullptr || IsA(dependency_node, List);
            List* dependencies =
                dependency_node == nullptr
                    ? NIL
                    : (dependency_shape_valid ? reinterpret_cast<List*>(dependency_node) : NIL);
            if (boundary_id != static_cast<int>(expected_boundary_id) || producer_id <= 0 ||
                producer_id > producer_count || expression_index < 0 ||
                expression_index >= expression_count ||
                mechanism < static_cast<int>(materialization_mechanism::backing_copy) ||
                mechanism > static_cast<int>(materialization_mechanism::letter_array) ||
                reason < static_cast<int>(materialization_reason::executor_copy) ||
                reason > static_cast<int>(materialization_reason::client_output) ||
                !dependency_shape_valid) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqSliceProject plan codec topology is invalid"),
                         errdetail("record_index=%d boundary_id=%d "
                                   "expected_boundary_id=%u producer_id=%d "
                                   "producer_count=%d mechanism=%d reason=%d "
                                   "expression_index=%d expression_count=%d "
                                   "dependency_node_tag=%d",
                                   record_index,
                                   boundary_id,
                                   expected_boundary_id,
                                   producer_id,
                                   producer_count,
                                   mechanism,
                                   reason,
                                   expression_index,
                                   expression_count,
                                   dependency_node == nullptr
                                       ? 0
                                       : static_cast<int>(nodeTag(dependency_node)))));
            }
            int dependency_index = 0;
            for (Node* dependency : postgres_list<Node>(dependencies)) {
                if (dependency == nullptr || !IsA(dependency, Integer) || intVal(dependency) <= 0 ||
                    intVal(dependency) >= boundary_id) {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqSliceProject dependency is not topological"),
                         errdetail(
                             "record_index=%d dependency_index=%d "
                             "boundary_id=%d dependency_id=%d dependency_node_tag=%d",
                             record_index,
                             dependency_index,
                             boundary_id,
                             dependency != nullptr && IsA(dependency, Integer) ? intVal(dependency)
                                                                               : 0,
                             dependency == nullptr ? 0 : static_cast<int>(nodeTag(dependency)))));
                }
                ++dependency_index;
            }
            (void)slice_project_int8_value(
                reinterpret_cast<Node*>(list_nth(record, k_slice_record_estimated_symbols)));
            Node* encoding = reinterpret_cast<Node*>(list_nth(record, k_slice_record_encoding));
            Node* cache_scope =
                reinterpret_cast<Node*>(list_nth(record, k_slice_record_cache_scope));
            if (encoding == nullptr || !IsA(encoding, String) || cache_scope == nullptr ||
                !IsA(cache_scope, String)) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqSliceProject boundary metadata is invalid"),
                         errdetail(
                             "record_index=%d encoding_node_tag=%d "
                             "cache_scope_node_tag=%d",
                             record_index,
                             encoding == nullptr ? 0 : static_cast<int>(nodeTag(encoding)),
                             cache_scope == nullptr ? 0 : static_cast<int>(nodeTag(cache_scope)))));
            }
            ++expected_boundary_id;
            ++record_index;
        }
        Bitmapset* seen_resnos = nullptr;
        int binding_index = 0;
        for (Node* raw_binding : postgres_list<Node>(output_bindings)) {
            List* binding = raw_binding != nullptr && IsA(raw_binding, List)
                                ? reinterpret_cast<List*>(raw_binding)
                                : NIL;
            if (list_length(binding) != 2 ||
                !IsA(reinterpret_cast<Node*>(linitial(binding)), Integer) ||
                !IsA(reinterpret_cast<Node*>(lsecond(binding)), Integer)) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqSliceProject output binding is invalid"),
                         errdetail("binding_index=%d", binding_index)));
            }
            const int resno = intVal(linitial(binding));
            const int producer_id = intVal(lsecond(binding));
            if (resno <= 0 || resno > list_length(scan->scan.plan.targetlist) || producer_id <= 0 ||
                producer_id > producer_count || bms_is_member(resno, seen_resnos)) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqSliceProject output binding is out of range"),
                         errdetail("binding_index=%d resno=%d producer_id=%d",
                                   binding_index,
                                   resno,
                                   producer_id)));
            }
            seen_resnos = bms_add_member(seen_resnos, resno);
            ++binding_index;
        }
        if (seen_resnos != nullptr)
            bms_free(seen_resnos);
    }

    [[nodiscard]] Plan* bioseq_slice_project_plan_custom_path(PlannerInfo*,
                                                              RelOptInfo* rel,
                                                              CustomPath* best_path,
                                                              List* tlist,
                                                              List* clauses,
                                                              List* custom_plans) {
        if (rel == nullptr || best_path == nullptr || list_length(custom_plans) != 1 ||
            list_length(best_path->custom_private) != 5) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject path shape is invalid")));
        }
        List* header = reinterpret_cast<List*>(list_nth(best_path->custom_private, 0));
        List* expressions = reinterpret_cast<List*>(list_nth(best_path->custom_private, 1));
        List* nodes = reinterpret_cast<List*>(list_nth(best_path->custom_private, 2));
        List* records = reinterpret_cast<List*>(list_nth(best_path->custom_private, 3));
        List* output_bindings = reinterpret_cast<List*>(list_nth(best_path->custom_private, 4));
        if (header == nullptr || !IsA(header, List) ||
            (expressions != NIL && !IsA(expressions, List)) ||
            (nodes != NIL && !IsA(nodes, List)) || (records != NIL && !IsA(records, List)) ||
            (output_bindings != NIL && !IsA(output_bindings, List))) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject path codec is invalid")));
        }
        auto* scan = makeNode(CustomScan);
        scan->methods = &bioseq_slice_project_scan_methods();
        scan->scan.scanrelid = 0;
        scan->scan.plan.targetlist = tlist;
        /* SliceProject is a projection owner, never a restriction owner.  This is
         * essential when the lowest valid placement is directly above a base
         * BioseqScan: create_plan has already given the child the base clauses, and
         * copying them onto this scanrelid-zero wrapper would both execute them
         * twice and ask setrefs to resolve base Vars against the projected output. */
        (void)clauses;
        scan->scan.plan.qual = NIL;
        scan->custom_plans = custom_plans;
        auto* child_plan = reinterpret_cast<Plan*>(linitial(custom_plans));
        scan->custom_scan_tlist = static_cast<List*>(copyObject(child_plan->targetlist));
        scan->custom_exprs = static_cast<List*>(copyObject(expressions));
        scan->custom_private = list_make4(copyObject(header),
                                          copyObject(nodes),
                                          copyObject(records),
                                          copyObject(output_bindings));
        scan->custom_relids = bms_copy(rel->relids);
        scan->flags = best_path->flags;
        return reinterpret_cast<Plan*>(scan);
    }

    [[nodiscard]] List* bioseq_slice_project_reparameterize_custom_path_by_child(
        PlannerInfo*, List* custom_private, RelOptInfo*) {
        return static_cast<List*>(copyObject(custom_private));
    }

    [[nodiscard]] Node* bioseq_slice_project_create_custom_scan_state(CustomScan*) {
        auto* state = static_cast<slice_project_state*>(palloc0(sizeof(slice_project_state)));
        NodeSetTag(&state->custom_scan_state, T_CustomScanState);
        state->custom_scan_state.methods = &bioseq_slice_project_exec_methods();
        return reinterpret_cast<Node*>(state);
    }

    [[nodiscard]] CustomScanMethods const& bioseq_slice_project_scan_methods() noexcept {
        static const CustomScanMethods methods{
            .CustomName = "BioseqSliceProject",
            .CreateCustomScanState = bioseq_slice_project_create_custom_scan_state,
        };
        return methods;
    }

    [[nodiscard]] CustomPathMethods const& bioseq_slice_project_path_methods() noexcept {
        static const CustomPathMethods methods{
            .CustomName = "BioseqSliceProject",
            .PlanCustomPath = bioseq_slice_project_plan_custom_path,
            .ReparameterizeCustomPathByChild =
                bioseq_slice_project_reparameterize_custom_path_by_child,
        };
        return methods;
    }

} // export custom-scan partition declarations

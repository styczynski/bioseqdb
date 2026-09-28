/**
 * @file
 * @brief Defines SliceProject plan records and estimates sequence-work costs.
 */
module;
extern "C" {
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "nodes/value.h"
#include "optimizer/optimizer.h"
#include "utils/errcodes.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

export module bioseq.postgres.table:custom_scan_slice_path_codec_contract;

import bioseq.common.checked_arithmetic;
import bioseq.postgres.support;
import bioseq.postgres.types.compact_sequence.header_probe;
import bioseq.postgres.types.compact_sequence.materialization_contract;
import bioseq.storage.coordinates;

import :custom_scan_oid_expression_support_oid_catalog_contracts;
import :custom_scan_oid_expression_support_expression_analysis_contracts;
export import :custom_scan_slice_path_expression_contract;
export import :custom_scan_slice_path_cost_contracts;

using namespace bioseq::postgres::support;

export {

    /* PostgreSQL evaluates the original target expressions, preserving CASE,
     * NULL handling, volatility and errors. These records describe sequence
     * operations for EXPLAIN and validation They use PostgreSQL nodes so
     * normal plan copying, serialization and reference rewriting can handle them. */

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

    /** @brief Flags describing an expression in the SliceProject graph. */
    enum slice_project_node_flags : int {
        /** @brief The expression contains a  volatile function. */
        k_slice_node_volatile = 1 << 0,
        /** @brief The expression is kept as a whole source in the graph. */
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

    /* PG17 uses NIL or a single null cell for an omitted slice bound.
     * Treat both as absent so custom_exprs never gets a null expression
     * that EXPLAIN would print as an empty string. */
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
            /** @brief Reads a non-NULL bigint constant used as a slice bound. */
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
                /* Crossing from a negative lower bound to a nonnegative upper bound
                 * needs the source length. Keep the unknown-length estimate here. */
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

    /* Cost mmsearch arguments in SQL order. Its scalar support cost  is a
     * planner penalty, so, the owning custom scan estimates alignment work
     * separately instead of inheriting that penalty. */
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

    /** @brief Estimates mmsearch operand length, using a sequence constant's stored length when available. */
    [[nodiscard]] std::uint64_t estimate_mmsearch_operand_symbols(Node* expression,
                                                                  bioseq_stub_oids const& oids) {
        if (expression == nullptr)
            return 4096u;
        if (Const* constant = try_cast_pg_node<Const>(expression)) {
            if (constant->constisnull)
                return 0u;
            if (constant->consttype == oids.bioseq_seq_type_oid) {
                return bioseq::postgres::compact_sequence_header_bp_length(
                    constant->constvalue);
            }
        }
        const auto opcode = slice_project_node_opcode(expression, oids);
        if (opcode.has_value()) {
            return slice_project_estimated_symbols(expression, oids);
        }
        return 4096u;
    }

    /**
     * @brief Estimates startup and per-row costs for a cjstom scan that runs mmsearch.
     *
     * Accounts for operand lengths, batch count and sequence data that must be
     * materialized, with an index-read startup allowance when applicable.
     * Returns finite, nonnegative costs for PostgreSQL to compare with other paths.
     */
    [[nodiscard]] owned_mmsearch_cost
    estimate_owned_mmsearch_cost(std::uint64_t target_length_estimate,
                                 std::uint64_t query_length_estimate,
                                 double input_rows,
                                 bool index_read_requirement,
                                 std::uint32_t planned_batch_size,
                                 std::uint64_t planned_materialized_bytes) noexcept {
        constexpr Cost maximum = std::numeric_limits<Cost>::max() / 32.0;
        /** @brief Keeps a cost within the nonnegative, finite range used by  the planner */
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

    /** @brief Collects sequence expressions, producer dependencies and size estimates while planning. */
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

    /** @brief Writes plan counts and size estimates from the completed SliceProject builder. */
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

} // export custom-scan partition declarations

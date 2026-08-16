/**
 * @file
 * @brief Owns SliceProject EXPLAIN, shutdown, and stable executor methods.
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

export module bioseq.postgres.table:custom_scan_slice_execution_explain_callbacks;

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

import :custom_scan_slice_path;

import :custom_scan_slice_execution_dsm_protocol;
import :custom_scan_slice_execution_resource_lifecycle;
import :custom_scan_slice_execution_tuple_production;

using namespace bioseq::postgres::support;

export {
    /** Deparses the custom scan expression at @p expression_index into EXPLAIN text. Returns a
     *  pstrdup'd string or `<omitted>` when the index is out of range. */
    [[nodiscard]] char*
    slice_project_deparse_expression(CustomScan* scan, int expression_index, ExplainState* es) {
        if (expression_index < 0 || expression_index >= list_length(scan->custom_exprs)) {
            return pstrdup("<omitted>");
        }
        Node* expression = reinterpret_cast<Node*>(list_nth(scan->custom_exprs, expression_index));
        return deparse_expression(expression, es->deparse_cxt, true, false);
    }

    /** Maps @p opcode to the human-readable label used in EXPLAIN output. Returns `"invalid"` for
     *  unknown values. */
    [[nodiscard]] const char* slice_project_opcode_name(slice_project_opcode opcode) noexcept {
        switch (opcode) {
        case slice_project_opcode::source_expr:
            return "source";
        case slice_project_opcode::literal_expr:
            return "literal";
        case slice_project_opcode::element:
            return "element";
        case slice_project_opcode::slice:
            return "slice";
        case slice_project_opcode::concat:
            return "concat";
        case slice_project_opcode::reverse:
            return "reverse";
        case slice_project_opcode::reverse_complement:
            return "reverse complement";
        case slice_project_opcode::length:
            return "length";
        case slice_project_opcode::cardinality:
            return "cardinality";
        case slice_project_opcode::bridge:
            return "literal bridge";
        case slice_project_opcode::materialize:
            return "materialize";
        }
        return "invalid";
    }

    /** Builds a Postgres list of `value_{id}` names from the dependency field of a serialized
     *  recipe @p node. Returns NIL when the node has no dependencies. */
    [[nodiscard]] List* slice_project_node_dependency_names(List* node) {
        List* names = NIL;
        Node* dependency_node = reinterpret_cast<Node*>(list_nth(node, k_slice_node_dependencies));
        if (dependency_node == nullptr)
            return names;
        for (Node* dependency : postgres_list<Node>(reinterpret_cast<List*>(dependency_node))) {
            const std::string value = fmt::format("value_{}", intVal(dependency));
            names = lappend(names, pstrdup(value.c_str()));
        }
        return names;
    }

    /** Builds a Postgres list of `var_{id}` names from the dependency field of a serialized
     *  materialization @p record. Returns NIL when the record has no dependencies. */
    [[nodiscard]] List* slice_project_dependency_names(List* record) {
        List* names = NIL;
        Node* dependency_node =
            reinterpret_cast<Node*>(list_nth(record, k_slice_record_dependencies));
        if (dependency_node == nullptr)
            return names;
        for (Node* dependency : postgres_list<Node>(reinterpret_cast<List*>(dependency_node))) {
            std::array<char, 32> variable{};
            snprintf(variable.data(), variable.size(), "var_%d", intVal(dependency));
            names = lappend(names, pstrdup(variable.data()));
        }
        return names;
    }

#ifdef BIOSEQ_TEST_INDEXES
    /** Maps a DSM test fixture mode code to its EXPLAIN label. Returns `"none"` for unrecognized
     *  modes. */
    [[nodiscard]] const char* slice_project_dsm_test_fixture_name(int mode) noexcept {
        switch (mode) {
        case k_slice_dsm_test_incomplete:
            return "incomplete";
        case k_slice_dsm_test_malformed:
            return "malformed";
        default:
            return "none";
        }
    }

    /** Appends DSM test fixture diagnostics to @p es when @p state applied a fixture during
     *  ANALYZE. No-op when @p state or @p es is null or no fixture ran. */
    void slice_project_explain_dsm_test_fixture(slice_project_state const* state,
                                                ExplainState* es) {
        if (state == nullptr || es == nullptr || !state->runtime.dsm_test_fixture_applied)
            return;
        auto const& runtime = state->runtime;
        const char* mode = slice_project_dsm_test_fixture_name(runtime.dsm_test_fixture_mode);
        if (es->format == EXPLAIN_FORMAT_TEXT) {
            /** Writes one indented name/value line into the text EXPLAIN buffer. */
            auto property = [es](const char* name, const char* value) {
                appendStringInfoSpaces(es->str, es->indent * 2);
                appendStringInfo(es->str, "%s: %s\n", name, value);
            };
            property("DSM Test Fixture", mode);
            property("DSM Fixture Validated",
                     runtime.dsm_test_fixture_validated ? "true" : "false");
            property("DSM Async Lifecycle Validated",
                     runtime.dsm_test_async_validated ? "true" : "false");
            /** Writes one indented unsigned integer property line into the text EXPLAIN buffer. */
            const auto integer_property = [es](const char* name, std::uint32_t value) {
                appendStringInfoSpaces(es->str, es->indent * 2);
                appendStringInfo(es->str, "%s: %u\n", name, value);
            };
            integer_property("DSM Fixture Checks", runtime.dsm_test_checks);
            integer_property("DSM Fixture Slot Headers Read", runtime.dsm_test_slot_headers_read);
            integer_property("DSM Fixture Counter Records Read",
                             runtime.dsm_test_counter_records_read);
            integer_property("DSM Async Cleanup Callbacks", runtime.dsm_test_cleanup_callbacks);
            return;
        }
        ExplainPropertyText("DSM Test Fixture", mode, es);
        ExplainPropertyBool("DSM Fixture Validated", runtime.dsm_test_fixture_validated, es);
        ExplainPropertyBool("DSM Async Lifecycle Validated", runtime.dsm_test_async_validated, es);
        ExplainPropertyUInteger("DSM Fixture Checks", nullptr, runtime.dsm_test_checks, es);
        ExplainPropertyUInteger(
            "DSM Fixture Slot Headers Read", nullptr, runtime.dsm_test_slot_headers_read, es);
        ExplainPropertyUInteger(
            "DSM Fixture Counter Records Read", nullptr, runtime.dsm_test_counter_records_read, es);
        ExplainPropertyUInteger(
            "DSM Async Cleanup Callbacks", nullptr, runtime.dsm_test_cleanup_callbacks, es);
    }
#endif

    /** Returns the ANALYZE-time DSM counter for materialization boundary @p index. Returns a static
     *  zero counter when @p state is null, tracing is off, or @p index is out of range. */
    [[nodiscard]] slice_dsm_counter const&
    slice_project_explain_counter(slice_project_state const* state, std::uint32_t index) noexcept {
        static const slice_dsm_counter zero{};
        if (state == nullptr || !state->runtime.collect_actual ||
            state->runtime.counters == nullptr || index >= state->runtime.boundary_count)
            return zero;
        return state->runtime.counters[index];
    }

    /** Renders the slice recipe and materialization plan as indented text EXPLAIN output in @p es.
     *  Uses @p scan for expression deparsing and @p state for ANALYZE counters when enabled. */
    void slice_project_explain_text(slice_project_state* state,
                                    CustomScan* scan,
                                    List* header,
                                    List* nodes,
                                    List* records,
                                    ExplainState* es) {
        appendStringInfoSpaces(es->str, es->indent * 2);
        appendStringInfoString(es->str, "Slice Recipe:\n");
        for (Node* raw_node : postgres_list<Node>(nodes)) {
            List* node = reinterpret_cast<List*>(raw_node);
            const int stable_id = intVal(list_nth(node, k_slice_node_stable_id));
            const auto opcode =
                static_cast<slice_project_opcode>(intVal(list_nth(node, k_slice_node_opcode)));
            const int expression_index = intVal(list_nth(node, k_slice_node_expression_index));
            appendStringInfoSpaces(es->str, (es->indent + 1) * 2);
            appendStringInfo(es->str,
                             "value_%d [%s] = %s\n",
                             stable_id,
                             slice_project_opcode_name(opcode),
                             slice_project_deparse_expression(scan, expression_index, es));
            List* dependencies = slice_project_node_dependency_names(node);
            appendStringInfoSpaces(es->str, (es->indent + 2) * 2);
            appendStringInfoString(es->str, "Depends On: ");
            if (dependencies == NIL) {
                appendStringInfoString(es->str, "none");
            } else {
                bool first = true;
                for (char* name : postgres_list<char>(dependencies)) {
                    if (!first)
                        appendStringInfoString(es->str, ", ");
                    appendStringInfoString(es->str, name);
                    first = false;
                }
            }
            appendStringInfoChar(es->str, '\n');
            if (opcode == slice_project_opcode::slice || opcode == slice_project_opcode::element) {
                const int lower = intVal(list_nth(node, k_slice_node_lower_expression_index));
                const int upper = intVal(list_nth(node, k_slice_node_upper_expression_index));
                appendStringInfoSpaces(es->str, (es->indent + 2) * 2);
                appendStringInfo(
                    es->str,
                    "Bounds: lower=%s upper=%s\n",
                    lower < 0 ? "omitted" : slice_project_deparse_expression(scan, lower, es),
                    upper < 0 ? "omitted" : slice_project_deparse_expression(scan, upper, es));
            }
        }
        appendStringInfoSpaces(es->str, es->indent * 2);
        appendStringInfoString(es->str, "Slice Materialization:\n");
        if (es->analyze && state != nullptr &&
            state->runtime.trace_runtime.incomplete_workers != 0u) {
            appendStringInfoSpaces(es->str, (es->indent + 1) * 2);
            appendStringInfo(es->str,
                             "Incomplete Workers: %u\n",
                             state->runtime.trace_runtime.incomplete_workers);
        }
        std::uint32_t counter_index = 0u;
        for (Node* raw_record : postgres_list<Node>(records)) {
            List* record = reinterpret_cast<List*>(raw_record);
            const int boundary_id = intVal(list_nth(record, k_slice_record_boundary_id));
            const int producer_id = intVal(list_nth(record, k_slice_record_producer_id));
            const int expression_index = intVal(list_nth(record, k_slice_record_expression_index));
            const auto mechanism = static_cast<materialization_mechanism>(
                intVal(list_nth(record, k_slice_record_mechanism)));
            const auto reason = static_cast<materialization_reason>(
                intVal(list_nth(record, k_slice_record_reason)));
            char* expression = slice_project_deparse_expression(scan, expression_index, es);
            appendStringInfoSpaces(es->str, (es->indent + 1) * 2);
            appendStringInfo(es->str, "var_%d = %s\n", boundary_id, expression);
            /** Writes one indented nested property line under the current materialization record. */
            auto property = [es](const char* name, const char* value) {
                appendStringInfoSpaces(es->str, (es->indent + 2) * 2);
                appendStringInfo(es->str, "%s: %s\n", name, value);
            };
            property("Mechanism", slice_project_mechanism_name(mechanism));
            property("Reason", slice_project_reason_name(reason));
            List* dependencies = slice_project_dependency_names(record);
            StringInfoData dependency_text;
            initStringInfo(&dependency_text);
            if (dependencies == NIL) {
                appendStringInfoString(&dependency_text, "none");
            } else {
                bool first = true;
                for (char* name : postgres_list<char>(dependencies)) {
                    if (!first)
                        appendStringInfoString(&dependency_text, ", ");
                    appendStringInfoString(&dependency_text, name);
                    first = false;
                }
            }
            property("Depends On", dependency_text.data);
            property("Encoding", strVal(list_nth(record, k_slice_record_encoding)));
            property("Cache Scope", strVal(list_nth(record, k_slice_record_cache_scope)));
            if (es->verbose) {
                std::array<char, 32> producer{};
                snprintf(producer.data(), producer.size(), "%d", producer_id);
                property("Producer Value", producer.data());
                property("Source Provenance", expression);
            }
            if (es->costs) {
                const auto estimate = slice_project_int8_value(
                    reinterpret_cast<Node*>(list_nth(record, k_slice_record_estimated_symbols)));
                appendStringInfoSpaces(es->str, (es->indent + 2) * 2);
                appendStringInfo(es->str, "Estimated Symbols: " UINT64_FORMAT "\n", estimate);
                appendStringInfoSpaces(es->str, (es->indent + 2) * 2);
                appendStringInfo(es->str, "Estimated Bytes: " UINT64_FORMAT " bytes\n", estimate);
            }
            if (es->analyze) {
                auto const& counter = slice_project_explain_counter(state, counter_index);
                /** Writes one indented ANALYZE counter line, optionally suffixing @p unit. */
                auto actual =
                    [es](const char* name, std::uint64_t value, const char* unit = nullptr) {
                        appendStringInfoSpaces(es->str, (es->indent + 2) * 2);
                        appendStringInfo(es->str,
                                         "%s: " UINT64_FORMAT "%s%s\n",
                                         name,
                                         value,
                                         unit == nullptr ? "" : " ",
                                         unit == nullptr ? "" : unit);
                    };
                actual("Actual Calls", counter.calls);
                actual("Actual Input Symbols", counter.input_symbols);
                actual("Actual Output Symbols", counter.output_symbols);
                actual("Actual Bytes", counter.bytes, "bytes");
                actual("Reused Calls", counter.reused_calls);
                actual("Peak Retained Bytes", counter.peak_retained_bytes, "bytes");
                const bool overflow =
                    counter.overflowed != 0u ||
                    (state != nullptr && (state->runtime.trace_runtime.invalid ||
                                          state->runtime.trace_runtime.overflowed));
                property("Counter Overflow", overflow ? "true" : "false");
                if (es->timing && state != nullptr && state->runtime.timing) {
                    appendStringInfoSpaces(es->str, (es->indent + 2) * 2);
                    appendStringInfo(es->str,
                                     "Actual Time: %.3f ms\n",
                                     static_cast<double>(counter.elapsed_ns) / 1.0e6);
                }
            }
            ++counter_index;
        }
        if (records == NIL) {
            appendStringInfoSpaces(es->str, (es->indent + 1) * 2);
            appendStringInfoString(es->str, "none\n");
        }
        if (es->costs) {
            appendStringInfoSpaces(es->str, es->indent * 2);
            appendStringInfo(es->str,
                             "Planned Materialized Symbols: " UINT64_FORMAT "\n",
                             slice_project_int8_value(reinterpret_cast<Node*>(
                                 list_nth(header, k_slice_header_planned_symbols))));
            appendStringInfoSpaces(es->str, es->indent * 2);
            appendStringInfo(es->str,
                             "Planned Materialized Bytes: " UINT64_FORMAT " bytes\n",
                             slice_project_int8_value(reinterpret_cast<Node*>(
                                 list_nth(header, k_slice_header_planned_bytes))));
        }
    }

    /** Renders the slice recipe and materialization plan as structured EXPLAIN groups in @p es.
     *  Uses @p scan for expression deparsing and @p state for ANALYZE counters when enabled. */
    void slice_project_explain_structured(slice_project_state* state,
                                          CustomScan* scan,
                                          List* nodes,
                                          List* records,
                                          ExplainState* es) {
        ExplainOpenGroup("Slice Recipe", "Slice Recipe", false, es);
        for (Node* raw_node : postgres_list<Node>(nodes)) {
            List* node = reinterpret_cast<List*>(raw_node);
            const int stable_id = intVal(list_nth(node, k_slice_node_stable_id));
            const auto opcode =
                static_cast<slice_project_opcode>(intVal(list_nth(node, k_slice_node_opcode)));
            const int expression_index = intVal(list_nth(node, k_slice_node_expression_index));
            ExplainOpenGroup("Recipe Node", nullptr, true, es);
            ExplainPropertyUInteger(
                "Stable ID", nullptr, static_cast<std::uint64_t>(stable_id), es);
            ExplainPropertyText("Opcode", slice_project_opcode_name(opcode), es);
            ExplainPropertyText(
                "Expression", slice_project_deparse_expression(scan, expression_index, es), es);
            ExplainPropertyList("Depends On", slice_project_node_dependency_names(node), es);
            const int lower = intVal(list_nth(node, k_slice_node_lower_expression_index));
            const int upper = intVal(list_nth(node, k_slice_node_upper_expression_index));
            if (lower >= 0) {
                ExplainPropertyText(
                    "Lower Bound", slice_project_deparse_expression(scan, lower, es), es);
            }
            if (upper >= 0) {
                ExplainPropertyText(
                    "Upper Bound", slice_project_deparse_expression(scan, upper, es), es);
            }
            ExplainCloseGroup("Recipe Node", nullptr, true, es);
        }
        ExplainCloseGroup("Slice Recipe", "Slice Recipe", false, es);
        ExplainOpenGroup("Slice Materialization", "Slice Materialization", false, es);
        if (es->analyze && state != nullptr &&
            state->runtime.trace_runtime.incomplete_workers != 0u) {
            ExplainPropertyUInteger(
                "Incomplete Workers", nullptr, state->runtime.trace_runtime.incomplete_workers, es);
        }
        std::uint32_t counter_index = 0u;
        for (Node* raw_record : postgres_list<Node>(records)) {
            List* record = reinterpret_cast<List*>(raw_record);
            const int boundary_id = intVal(list_nth(record, k_slice_record_boundary_id));
            const int producer_id = intVal(list_nth(record, k_slice_record_producer_id));
            const int expression_index = intVal(list_nth(record, k_slice_record_expression_index));
            const auto mechanism = static_cast<materialization_mechanism>(
                intVal(list_nth(record, k_slice_record_mechanism)));
            const auto reason = static_cast<materialization_reason>(
                intVal(list_nth(record, k_slice_record_reason)));
            std::array<char, 32> variable{};
            snprintf(variable.data(), variable.size(), "var_%d", boundary_id);
            ExplainOpenGroup("Materialization", nullptr, true, es);
            ExplainPropertyText("Variable", variable.data(), es);
            ExplainPropertyText(
                "Expression", slice_project_deparse_expression(scan, expression_index, es), es);
            ExplainPropertyText("Mechanism", slice_project_mechanism_name(mechanism), es);
            ExplainPropertyText("Reason", slice_project_reason_name(reason), es);
            ExplainPropertyList("Depends On", slice_project_dependency_names(record), es);
            ExplainPropertyText("Encoding", strVal(list_nth(record, k_slice_record_encoding)), es);
            ExplainPropertyText(
                "Cache Scope", strVal(list_nth(record, k_slice_record_cache_scope)), es);
            if (es->verbose) {
                ExplainPropertyUInteger(
                    "Producer Value", nullptr, static_cast<std::uint64_t>(producer_id), es);
                ExplainPropertyText("Source Provenance",
                                    slice_project_deparse_expression(scan, expression_index, es),
                                    es);
            }
            if (es->costs) {
                const auto estimate = slice_project_int8_value(
                    reinterpret_cast<Node*>(list_nth(record, k_slice_record_estimated_symbols)));
                ExplainPropertyUInteger("Estimated Symbols", nullptr, estimate, es);
                ExplainPropertyUInteger("Estimated Bytes", "bytes", estimate, es);
            }
            if (es->analyze) {
                auto const& counter = slice_project_explain_counter(state, counter_index);
                ExplainPropertyUInteger("Actual Calls", nullptr, counter.calls, es);
                ExplainPropertyUInteger("Actual Input Symbols", nullptr, counter.input_symbols, es);
                ExplainPropertyUInteger(
                    "Actual Output Symbols", nullptr, counter.output_symbols, es);
                ExplainPropertyUInteger("Actual Bytes", "bytes", counter.bytes, es);
                ExplainPropertyUInteger("Reused Calls", nullptr, counter.reused_calls, es);
                ExplainPropertyUInteger(
                    "Peak Retained Bytes", "bytes", counter.peak_retained_bytes, es);
                ExplainPropertyBool(
                    "Counter Overflow",
                    counter.overflowed != 0u ||
                        (state != nullptr && (state->runtime.trace_runtime.invalid ||
                                              state->runtime.trace_runtime.overflowed)),
                    es);
                if (es->timing && state != nullptr && state->runtime.timing) {
                    ExplainPropertyFloat("Actual Time",
                                         "ms",
                                         static_cast<double>(counter.elapsed_ns) / 1.0e6,
                                         3,
                                         es);
                }
            }
            ExplainCloseGroup("Materialization", nullptr, true, es);
            ++counter_index;
        }
        ExplainCloseGroup("Slice Materialization", "Slice Materialization", false, es);
    }

    /** CustomScan EXPLAIN callback for SliceProject. Initializes trace state for ANALYZE, then
     *  emits text or structured output from the serialized plan in @p custom. */
    void slice_project_explain(CustomScanState* custom, List*, ExplainState* es) {
        if (es->analyze)
            slice_project_ensure_trace_initialized(custom);
#ifdef BIOSEQ_TEST_INDEXES
        if (es->analyze) {
            slice_project_apply_dsm_test_fixture(reinterpret_cast<slice_project_state*>(custom));
        }
#endif
        auto* scan = reinterpret_cast<CustomScan*>(custom->ss.ps.plan);
        List* header = slice_project_plan_header(scan);
        List* nodes = slice_project_plan_nodes(scan);
        List* records = slice_project_plan_records(scan);
        auto* state = reinterpret_cast<slice_project_state*>(custom);
#ifdef BIOSEQ_TEST_INDEXES
        slice_project_explain_dsm_test_fixture(state, es);
#endif
        if (es->format == EXPLAIN_FORMAT_TEXT) {
            slice_project_explain_text(state, scan, header, nodes, records, es);
            return;
        }
        ExplainPropertyText("Projection Mode", "source-backed lazy", es);
        ExplainPropertyUInteger("Source Pieces",
                                nullptr,
                                slice_project_int8_value(reinterpret_cast<Node*>(
                                    list_nth(header, k_slice_header_source_pieces))),
                                es);
        ExplainPropertyUInteger("Estimated Piece Boundaries",
                                nullptr,
                                slice_project_int8_value(reinterpret_cast<Node*>(
                                    list_nth(header, k_slice_header_piece_boundaries))),
                                es);
        ExplainPropertyText(
            "Fallback Reason", strVal(list_nth(header, k_slice_header_fallback_reason)), es);
        slice_project_explain_structured(state, scan, nodes, records, es);
    }

    /** CustomScan shutdown hook that quiesces the slice async owner when parallel runtime
     *  initialization completed. */
    void slice_project_shutdown(CustomScanState* custom) {
        auto* state = reinterpret_cast<slice_project_state*>(custom);
        if (state->runtime.owner_initialized) {
            quiesce_slice_async_owner_noexcept(&state->runtime.owner_guard,
                                               slice_shutdown_disposition::complete);
        }
    }

    /** Returns the static CustomExecMethods vtable wired for BioseqSliceProject scans. */
    [[nodiscard]] CustomExecMethods const& bioseq_slice_project_exec_methods() noexcept {
        static_assert(std::is_same_v<decltype(&slice_project_estimate_dsm),
                                     Size (*)(CustomScanState*, ParallelContext*)>);
        static_assert(std::is_same_v<decltype(&slice_project_initialize_dsm),
                                     void (*)(CustomScanState*, ParallelContext*, void*)>);
        static_assert(std::is_same_v<decltype(&slice_project_reinitialize_dsm),
                                     void (*)(CustomScanState*, ParallelContext*, void*)>);
        static_assert(std::is_same_v<decltype(&slice_project_initialize_worker),
                                     void (*)(CustomScanState*, shm_toc*, void*)>);
        static_assert(
            std::is_same_v<decltype(&slice_project_shutdown), void (*)(CustomScanState*)>);
        static_assert(std::is_same_v<decltype(&slice_project_explain),
                                     void (*)(CustomScanState*, List*, ExplainState*)>);
        static const CustomExecMethods methods{
            .CustomName = "BioseqSliceProject",
            .BeginCustomScan = slice_project_begin,
            .ExecCustomScan = slice_project_exec,
            .EndCustomScan = slice_project_end,
            .ReScanCustomScan = slice_project_rescan,
            .MarkPosCustomScan = slice_project_mark_pos,
            .RestrPosCustomScan = slice_project_restr_pos,
            .EstimateDSMCustomScan = slice_project_estimate_dsm,
            .InitializeDSMCustomScan = slice_project_initialize_dsm,
            .ReInitializeDSMCustomScan = slice_project_reinitialize_dsm,
            .InitializeWorkerCustomScan = slice_project_initialize_worker,
            .ShutdownCustomScan = slice_project_shutdown,
            .ExplainCustomScan = slice_project_explain,
        };
        return methods;
    }

} // export custom-scan partition declarations

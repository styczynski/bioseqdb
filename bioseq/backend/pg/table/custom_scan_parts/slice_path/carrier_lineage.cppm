/**
 * @file
 * @brief Records sequence producers and dependencies for SliceProject plans.
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

export module bioseq.postgres.table:custom_scan_slice_path_carrier_lineage;

import std;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.support;

import :custom_scan_oid_expression_support_oid_catalog;
import :custom_scan_slice_path_codec_contract;
export import :custom_scan_slice_path_expression_detection_contracts;
export import :custom_scan_slice_path_carrier_contracts;

using namespace bioseq::postgres::support;

export {
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
                                                           bool distinct_consumer,
                                                           bool force_value) {
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
        /* Reusing graph entries affects the description only. Keep volatile
         * occurrences separate so EXPLAIN still shows each evaluation site. */
        if (!distinct_consumer && !contain_volatile_functions(expression)) {
            for (std::size_t index = 0u; index < builder.producer_expressions.size(); ++index) {
                if (equal(builder.producer_expressions[index], expression)) {
                    return static_cast<std::uint32_t>(index + 1u);
                }
            }
        }
        /* Number dependencies first, so every node refers to earlier IDs. */
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
        /* PostgreSQL evaluates unrecognized  source expressions as whole leaves.
         * Only known sequence operations expose dependencies in this graph. */
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
        if (builder.producer_expressions.size() >=
            static_cast<std::size_t>(std::numeric_limits<int>::max() - 1)) {
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
        node = lappend(node, makeInteger(-1)); /* reserved options-expression position */
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
                /* Zero marks a source used by diffekent producers. A value can carry
                 * only one producer ID before projection. */
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
        if (builder.next_boundary_id == std::numeric_limits<std::uint32_t>::max()) {
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

} // export custom-scan partition declarations

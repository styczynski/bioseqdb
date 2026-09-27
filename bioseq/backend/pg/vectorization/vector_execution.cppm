/**
 * @file
 * Evaluate supported SQL filters over  a batch of rows. Comparisons use one
 * column and one constant, with vector routines found in generated registries.
 * AND and OR combine the results into a mask of rows that pass.
 */


module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "commands/extension.h"
#include "parser/parse_func.h"
#include "utils/syscache.h"
}

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>

export module bioseq.postgres.vectorization.execution;

import bioseq.postgres.support;
import bioseq.postgres.vectorization.types;
import bioseq.column.vector_compare;
#if defined(BIOSEQ_TEST_INDEXES)
import bioseq.postgres.sql.generated.test_indexes.vector_aggregate_registry;
import bioseq.postgres.sql.generated.test_indexes.vector_comparison_registry;
#else
import bioseq.postgres.sql.generated.production.vector_aggregate_registry;
import bioseq.postgres.sql.generated.production.vector_comparison_registry;
#endif

namespace {

using bioseq::postgres::support::allocate_zeroed_array;
using bioseq::postgres::support::allocate_zeroed_object;
using bioseq::postgres::support::allocate_zeroed_storage;
using bioseq::postgres::support::is_pg_const_var_binary_args;
using bioseq::postgres::support::pg_node_view;
using bioseq::postgres::support::postgres_list;
using bioseq::postgres::support::try_cast_pg_operator_expression;

/** Catalog name, schema and types used to find a matching vector routine */
struct routine_signature final {
    std::array<char, NAMEDATALEN> name{};
    Oid namespace_oid = InvalidOid;
    std::array<Oid, 2> argument_types{InvalidOid, InvalidOid};
    Oid result_type = InvalidOid;
    std::int16_t argument_count = 0;
    char procedure_kind = PROKIND_FUNCTION;
};

/** Select a vector funstion call or an AND/OR group of child filters. */
enum class vector_qualifier_node_kind : int {
    boolean_expression,
    expression,
};

/**
 * A compiled filter with reusable function-call arguments, or a Boolean group
 * of child filters. PostgreSQL memory contexts own the allocated records.
 */
struct vector_qualifier_node final {
    vector_qualifier_node_kind kind;
    union payload_type {
        struct expression_type {
            FmgrInfo* function_info;
            FunctionCallInfo call_info;
            vector_operand_storage* arguments;
        } expression;
        struct boolean_expression_type {
            BoolExprType kind;
            List* expressions;
        } boolean_expression;
    } payload;
};

/** Read a routine's catalog signature. Return no match for over two arguments. */
[[nodiscard]] std::optional<routine_signature> read_routine_signature(Oid procedure_oid) {
    HeapTuple procedure_tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(procedure_oid));
    if (!HeapTupleIsValid(procedure_tuple))
        return std::nullopt;

    Form_pg_proc procedure_form = reinterpret_cast<Form_pg_proc>(GETSTRUCT(procedure_tuple));
    if (procedure_form->pronargs > 2) {
        ReleaseSysCache(procedure_tuple);
        return std::nullopt;
    }

    routine_signature signature{
        .namespace_oid = procedure_form->pronamespace,
        .argument_types = {procedure_form->pronargs >= 1 ? procedure_form->proargtypes.values[0]
                                                         : InvalidOid,
                           procedure_form->pronargs >= 2 ? procedure_form->proargtypes.values[1]
                                                         : InvalidOid},
        .result_type = procedure_form->prorettype,
        .argument_count = procedure_form->pronargs,
        .procedure_kind = procedure_form->prokind,
    };
    strlcpy(signature.name.data(), NameStr(procedure_form->proname), signature.name.size());
    ReleaseSysCache(procedure_tuple);
    return signature;
}

/** Find the schema of the installed bioseq extension, if present. */
[[nodiscard]] std::optional<Oid> find_extension_namespace_oid() {
    const Oid extension_oid = get_extension_oid("bioseq", true);
    if (!OidIsValid(extension_oid))
        return std::nullopt;

    const Oid namespace_oid = get_extension_schema(extension_oid);
    if (!OidIsValid(namespace_oid))
        return std::nullopt;
    return namespace_oid;
}

/** Find a function by schema, name and exact  argument types. */
[[nodiscard]] std::optional<Oid> lookup_installed_routine(Oid namespace_oid,
                                                          std::string_view routine_name,
                                                          std::span<const Oid> argument_types) {
    const Oid routine_oid =
        bioseq::postgres::support::invoke_pg_as_cpp([namespace_oid, routine_name, argument_types] {
            char* namespace_name = get_namespace_name(namespace_oid);
            if (namespace_name == nullptr)
                return InvalidOid;
            List* qualified_name =
                list_make2(makeString(namespace_name),
                           makeString(pnstrdup(routine_name.data(), routine_name.size())));
            std::array<Oid, 2> exact_argument_types{};
            std::ranges::copy(argument_types, exact_argument_types.begin());
            const Oid resolved_oid =
                LookupFuncName(qualified_name,
                               static_cast<int>(argument_types.size()),
                               argument_types.empty() ? nullptr : exact_argument_types.data(),
                               true);
            list_free_deep(qualified_name);
            return resolved_oid;
        });
    return OidIsValid(routine_oid) ? std::optional<Oid>{routine_oid} : std::nullopt;
}

/** Find the registered vector comparison and check its return type. */
[[nodiscard]] std::optional<Oid>
lookup_vector_comparison_owner(Oid namespace_oid,
                               std::string_view vector_name,
                               const std::array<Oid, 2>& argument_types,
                               Oid expected_result_type) {
    const std::optional<Oid> vector_oid =
        lookup_installed_routine(namespace_oid, vector_name, argument_types);
    if (!vector_oid || get_func_rettype(*vector_oid) != expected_result_type)
        return std::nullopt;
    return vector_oid;
}

/**
 * Check a routine's schema and name. An empty expected name matches an absent
 * routine, as needed for aggregates without, a final function.
 */
[[nodiscard]] bool
routine_matches(Oid procedure_oid, Oid namespace_oid, std::string_view expected_name) {
    if (!OidIsValid(procedure_oid))
        return expected_name.empty();
    if (expected_name.empty())
        return false;

    HeapTuple procedure_tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(procedure_oid));
    if (!HeapTupleIsValid(procedure_tuple))
        return false;
    Form_pg_proc procedure_form = reinterpret_cast<Form_pg_proc>(GETSTRUCT(procedure_tuple));
    const bool matches = procedure_form->pronamespace == namespace_oid &&
                         expected_name == NameStr(procedure_form->proname);
    ReleaseSysCache(procedure_tuple);
    return matches;
}

/**
 * Find a vector aggregate whose input, result and state types match the registry.
 * Also check its transition and final functions, so an unrelated installed
 * aggregate cannot be selected just because it has the expected name.
 */
[[nodiscard]] std::optional<Oid> lookup_vector_aggregate_owner(
    Oid namespace_oid,
    const bioseq::sql::generated::vector_aggregate_registry::association& association) {
    const std::array<Oid, 1> input_types{association.input_type};
    const std::span<const Oid> exact_input_types = OidIsValid(association.input_type)
                                                       ? std::span<const Oid>{input_types}
                                                       : std::span<const Oid>{};
    const std::optional<Oid> vector_oid =
        lookup_installed_routine(namespace_oid, association.vector_name, exact_input_types);
    if (!vector_oid)
        return std::nullopt;

    const std::optional<routine_signature> vector_signature = read_routine_signature(*vector_oid);
    if (!vector_signature || vector_signature->procedure_kind != PROKIND_AGGREGATE ||
        vector_signature->argument_count != static_cast<std::int16_t>(exact_input_types.size()) ||
        vector_signature->argument_types[0] != association.input_type ||
        vector_signature->result_type != association.result_type)
        return std::nullopt;

    HeapTuple aggregate_tuple = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(*vector_oid));
    if (!HeapTupleIsValid(aggregate_tuple))
        return std::nullopt;
    Form_pg_aggregate aggregate_form =
        reinterpret_cast<Form_pg_aggregate>(GETSTRUCT(aggregate_tuple));
    const bool matches =
        aggregate_form->aggtranstype == association.state_type &&
        routine_matches(aggregate_form->aggtransfn, namespace_oid, association.transition_name) &&
        routine_matches(aggregate_form->aggfinalfn, namespace_oid, association.final_name);
    ReleaseSysCache(aggregate_tuple);
    if (!matches)
        return std::nullopt;
    return vector_oid;
}

/** Find NULL constants, which  this rewriter leaves for scalar evaluation. */
[[nodiscard]] bool contains_null_constant(List* arguments) noexcept {
    for (Expr* argument : postgres_list<Expr>{arguments}) {
        if (Const* constant = pg_node_view{reinterpret_cast<Node*>(argument)}.try_cast<Const>();
            constant != nullptr && constant->constisnull)
            return true;
    }
    return false;
}

} // namespace

/**
 * Find the installed vector version of a scalar comparison or agmregate.
 * Match the scalar signature against the generated registry, then validate
 * the vector routine in the bioseq extension's schema. Return no match when
 * either side is missing or has a different signature
 */
export [[nodiscard]] std::optional<Oid> lookup_vectorized_routine_oid(Oid procedure_oid) {
    const std::optional<routine_signature> signature = read_routine_signature(procedure_oid);
    if (!signature)
        return std::nullopt;

    if (signature->argument_count == 2 && signature->procedure_kind != PROKIND_AGGREGATE) {
        using namespace bioseq::sql::generated::vector_comparison_registry;
        const auto association = std::ranges::find_if(all(), [signature](const auto& candidate) {
            return candidate.scalar_namespace == signature->namespace_oid &&
                   candidate.scalar_name == signature->name.data() &&
                   candidate.left_type == signature->argument_types[0] &&
                   candidate.right_type == signature->argument_types[1] &&
                   candidate.result_type == signature->result_type;
        });
        if (association != all().end()) {
            const std::optional<Oid> namespace_oid = find_extension_namespace_oid();
            if (!namespace_oid)
                return std::nullopt;
            return lookup_vector_comparison_owner(*namespace_oid,
                                                  association->vector_name,
                                                  signature->argument_types,
                                                  association->result_type);
        }
    }

    if (signature->procedure_kind != PROKIND_AGGREGATE || signature->argument_count > 1)
        return std::nullopt;
    using namespace bioseq::sql::generated::vector_aggregate_registry;
    const Oid input_type =
        signature->argument_count == 0 ? InvalidOid : signature->argument_types[0];
    const auto association =
        std::ranges::find_if(all(), [signature, input_type](const auto& candidate) {
            return candidate.scalar_namespace == signature->namespace_oid &&
                   candidate.scalar_name == signature->name.data() &&
                   candidate.input_type == input_type &&
                   candidate.result_type == signature->result_type;
        });
    if (association == all().end())
        return std::nullopt;
    const std::optional<Oid> namespace_oid = find_extension_namespace_oid();
    if (!namespace_oid)
        return std::nullopt;
    return lookup_vector_aggregate_owner(*namespace_oid, *association);
}

/**
 * Copy supported comparisons and point them at their vector functions.
 * A comparison needs one column and one  non-NULL constant. Rebuild an AND/OR
 * group only when every child can be rewritten. Keep the original pointers
 * for other expressions so the planner can leave them in scalar evaluation.
 */
export List* rewrite_vectorized_expression_list(List* expression_list) {
    if (expression_list == nullptr)
        return expression_list;

    check_stack_depth();

    List* rewritten_expressions = NIL;

    for (Node* expression_node : postgres_list<Node>{expression_list}) {
        if (expression_node == nullptr)
            return nullptr;

        /*
         * OpExpr, DistinctExpr and NullIfExpr share these fields. The helper
         * accepts their different PostgreSQL node tags.
         */
        if (OpExpr* op_expr_node = try_cast_pg_operator_expression(expression_node)) {
            if (list_length(op_expr_node->args) != 2) {
                rewritten_expressions = lappend(rewritten_expressions, op_expr_node);
                continue;
            }

            if (!is_pg_const_var_binary_args(op_expr_node->args)) {
                rewritten_expressions = lappend(rewritten_expressions, op_expr_node);
                continue;
            }
            if (contains_null_constant(op_expr_node->args)) {
                rewritten_expressions = lappend(rewritten_expressions, op_expr_node);
                continue;
            }

            HeapTuple operator_tuple =
                SearchSysCache1(OPEROID, ObjectIdGetDatum(op_expr_node->opno));
            if (!HeapTupleIsValid(operator_tuple)) {
                rewritten_expressions = lappend(rewritten_expressions, op_expr_node);
                continue;
            }
            Form_pg_operator operator_form =
                reinterpret_cast<Form_pg_operator>(GETSTRUCT(operator_tuple));
            const Oid procedure_oid = operator_form->oprcode;
            ReleaseSysCache(operator_tuple);

            const std::optional<Oid> vectorized_oid = lookup_vectorized_routine_oid(procedure_oid);
            if (!vectorized_oid) {
                rewritten_expressions = lappend(rewritten_expressions, op_expr_node);
                continue;
            }

            OpExpr* vectorized_op_expr_node = bioseq::postgres::support::copy_pg_node(op_expr_node);
            vectorized_op_expr_node->opfuncid = *vectorized_oid;
            rewritten_expressions = lappend(rewritten_expressions, vectorized_op_expr_node);

            continue;
        }

        if (BoolExpr* boolean_expression = pg_node_view{expression_node}.try_cast<BoolExpr>()) {
            List* rewritten_bool_expr_args =
                rewrite_vectorized_expression_list(boolean_expression->args);
            List* unchanged_arguments =
                list_intersection(rewritten_bool_expr_args, boolean_expression->args);
            const bool all_arguments_rewritten = unchanged_arguments == NIL;
            list_free(unchanged_arguments);

            if (all_arguments_rewritten &&
                (boolean_expression->boolop == AND_EXPR || boolean_expression->boolop == OR_EXPR)) {
                Expr* boolean_clause = nullptr;

                if (boolean_expression->boolop == AND_EXPR) {
                    boolean_clause = make_andclause(rewritten_bool_expr_args);
                } else if (boolean_expression->boolop == OR_EXPR) {
                    boolean_clause = make_orclause(rewritten_bool_expr_args);
                }

                rewritten_expressions = lappend(rewritten_expressions, boolean_clause);
            } else {
                rewritten_expressions = lappend(rewritten_expressions, boolean_expression);
            }

            continue;
        }

        rewritten_expressions = lappend(rewritten_expressions, expression_node);
    }

    return rewritten_expressions;
}

/**
 * Prepare vector filter calls for this batch slot. Build function-call records
 * once and keep references to the slot's column storage for later batches.
 * The input list must contain the supported expressions selected  by rewriting.
 */
export List* compile_vector_qualifier_list(TupleTableSlot* slot, List* vectorized_qual) {
    vector_tuple_table_slot_storage* vector_slot =
        reinterpret_cast<vector_tuple_table_slot_storage*>(slot);

    List* compiled_qualifiers = NIL;
    for (Node* expression_node : postgres_list<Node>{vectorized_qual}) {
        if (OpExpr* op_expr_node = try_cast_pg_operator_expression(expression_node)) {
            int argument_index = 0;
            const int argument_count = list_length(op_expr_node->args);

            vector_qualifier_node* compiled_qualifier =
                allocate_zeroed_object<vector_qualifier_node>();
            compiled_qualifier->kind = vector_qualifier_node_kind::expression;

            compiled_qualifier->payload.expression.function_info =
                allocate_zeroed_object<FmgrInfo>();
            compiled_qualifier->payload.expression.call_info =
                allocate_zeroed_storage<FunctionCallInfoBaseData>(
                    SizeForFunctionCallInfo(argument_count));
            compiled_qualifier->payload.expression.arguments =
                allocate_zeroed_array<vector_operand_storage>(argument_count);

            fmgr_info(op_expr_node->opfuncid, compiled_qualifier->payload.expression.function_info);
            fmgr_info_set_expr(expression_node,
                               compiled_qualifier->payload.expression.function_info);

            InitFunctionCallInfoData(*(compiled_qualifier->payload.expression.call_info),
                                     compiled_qualifier->payload.expression.function_info,
                                     argument_count,
                                     op_expr_node->inputcollid,
                                     nullptr,
                                     nullptr);

            for (Expr* argument : postgres_list<Expr>{op_expr_node->args}) {
                vector_operand_storage* argument_storage =
                    compiled_qualifier->payload.expression.arguments + argument_index;

                if (Const* constant =
                        pg_node_view{reinterpret_cast<Node*>(argument)}.try_cast<Const>()) {
                    argument_storage->kind = vector_operand_storage_kind::constant;
                    argument_storage->datum = constant->constvalue;

                    compiled_qualifier->payload.expression.call_info->args[argument_index].value =
                        reinterpret_cast<Datum>(argument_storage);
                    compiled_qualifier->payload.expression.call_info->args[argument_index].isnull =
                        constant->constisnull;
                } else {
                    Var* variable = pg_node_view{reinterpret_cast<Node*>(argument)}.try_cast<Var>();
                    Assert(variable != nullptr);
                    const int column_index = variable->varattno - 1;

                    argument_storage->kind = vector_operand_storage_kind::column;
                    argument_storage->datum =
                        vector_slot->virtual_slot.base.tts_values[column_index];

                    compiled_qualifier->payload.expression.call_info->args[argument_index].value =
                        reinterpret_cast<Datum>(argument_storage);
                    compiled_qualifier->payload.expression.call_info->args[argument_index].isnull =
                        false;
                }

                ++argument_index;
            }

            compiled_qualifiers = lappend(compiled_qualifiers, compiled_qualifier);
            continue;
        }

        if (BoolExpr* boolean_expression = pg_node_view{expression_node}.try_cast<BoolExpr>()) {
            vector_qualifier_node* compiled_qualifier =
                allocate_zeroed_object<vector_qualifier_node>();
            compiled_qualifier->kind = vector_qualifier_node_kind::boolean_expression;

            compiled_qualifier->payload.boolean_expression.kind = boolean_expression->boolop;
            compiled_qualifier->payload.boolean_expression.expressions =
                compile_vector_qualifier_list(slot, boolean_expression->args);

            compiled_qualifiers = lappend(compiled_qualifiers, compiled_qualifier);

            continue;
        }
    }

    return compiled_qualifiers;
}

/** Run one filter and return its row mask from the per-tuple memory context. */
static bool* evaluate_vectorized_expression(vector_qualifier_node* compiled_qualifier,
                                            ExprContext* expression_context) {
    vector_column_storage* evaluation_result;
    {
        bioseq::postgres::support::memory_context_scope memory_scope{
            expression_context->ecxt_per_tuple_memory};
        evaluation_result = reinterpret_cast<vector_column_storage*>(
            DatumGetPointer(FunctionCallInvoke(compiled_qualifier->payload.expression.call_info)));
    }

    return reinterpret_cast<bool*>(evaluation_result->data);
}

/**
 * Evaluate a filter list and combine its row masks with AND or OR.
 * Reuse, the first result mask for the combined result, and eturn nullptr
 * for an empty list. The mask has one entry for each populated batch row.
 */
export bool* evaluate_vectorized_qualifiers(TupleTableSlot* slot,
                                            List* vectorized_qual_list,
                                            BoolExprType bool_type,
                                            ExprContext* expression_context) {
    vector_tuple_table_slot_storage* vector_slot =
        reinterpret_cast<vector_tuple_table_slot_storage*>(slot);

    bool* combined_result = nullptr;

    for (vector_qualifier_node* compiled_qualifier :
         postgres_list<vector_qualifier_node>{vectorized_qual_list}) {
        bool* qualifier_result = nullptr;

        switch (compiled_qualifier->kind) {
        case vector_qualifier_node_kind::expression: {
            qualifier_result =
                evaluate_vectorized_expression(compiled_qualifier, expression_context);
            break;
        }
        case vector_qualifier_node_kind::boolean_expression: {
            const BoolExprType nested_kind = compiled_qualifier->payload.boolean_expression.kind;
            if (nested_kind == AND_EXPR || nested_kind == OR_EXPR) {
                qualifier_result = evaluate_vectorized_qualifiers(
                    slot,
                    compiled_qualifier->payload.boolean_expression.expressions,
                    nested_kind,
                    expression_context);
            }
            break;
        }
        }

        if (combined_result == nullptr) {
            combined_result = qualifier_result;
        } else {
            const std::uint32_t row_count = vector_slot->row_count;
            if (bool_type == AND_EXPR)
                bioseq::column::vec::mask_and(combined_result, qualifier_result, row_count);
            else if (bool_type == OR_EXPR)
                bioseq::column::vec::mask_or(combined_result, qualifier_result, row_count);
        }
    }

    return combined_result;
}

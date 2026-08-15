/**
 * @file
 * Rewrites, compiles, and evaluates PostgreSQL vector predicates.
 */

/*-------------------------------------------------------------------------
 *
 * vectorization/vector_execution.cppm
 *
 * Walks query expression trees to build vectorised-friendly variants:
 *   - is_pg_const_var_binary_args gates OpExprs to
 *     the (one Const + one Var) shape the vectorised evaluator supports.
 *   - lookup_vectorized_routine_oid looks up the "v"-prefixed vectorised
 *     implementation of a scalar procedure.
 *   - rewrite_vectorized_expression_list / compile_vector_qualifier_list convert
 *     qual/expr trees into vector_qualifier_node tree structures for the executor.
 *   - evaluate_vectorized_qualifiers runs the compiled vector_qualifier_node tree over a
 *     vector_tuple_table_slot_storage and returns the surviving-rows mask.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "commands/extension.h"
#include "parser/parse_func.h"
#include "utils/syscache.h"

#include <algorithm>
#include <array>
#include <bioseq/pg_compat.h>
#include <cstdint>
#include <cstring>
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

/** Captures the catalog identity needed to validate a vector routine owner. */
struct routine_signature final {
    std::array<char, NAMEDATALEN> name{};
    Oid namespace_oid = InvalidOid;
    std::array<Oid, 2> argument_types{InvalidOid, InvalidOid};
    Oid result_type = InvalidOid;
    std::int16_t argument_count = 0;
    char procedure_kind = PROKIND_FUNCTION;
};

/** Distinguishes routine calls from Boolean composition nodes. */
enum class vector_qualifier_node_kind : int {
    boolean_expression,
    expression,
};

/** Stores one compiled vector predicate node in PostgreSQL memory. */
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

/** Reads a bounded routine signature from PostgreSQL's procedure cache. */
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

/** Returns the schema that owns the installed Bioseq extension. */
[[nodiscard]] std::optional<Oid> find_extension_namespace_oid() {
    const Oid extension_oid = get_extension_oid("bioseq", true);
    if (!OidIsValid(extension_oid))
        return std::nullopt;

    const Oid namespace_oid = get_extension_schema(extension_oid);
    if (!OidIsValid(namespace_oid))
        return std::nullopt;
    return namespace_oid;
}

/** Resolves an exact installed routine without implicit argument coercion. */
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
            const Oid result =
                LookupFuncName(qualified_name,
                               static_cast<int>(argument_types.size()),
                               argument_types.empty() ? nullptr : exact_argument_types.data(),
                               true);
            list_free_deep(qualified_name);
            return result;
        });
    return OidIsValid(routine_oid) ? std::optional<Oid>{routine_oid} : std::nullopt;
}

/** Validates the installed owner of a generated vector comparison entry. */
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

/** Tests whether a procedure has the required schema and catalog name. */
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

/** Validates the installed owner of a generated vector aggregate entry. */
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

/** Detects null constants that must retain scalar PostgreSQL evaluation. */
[[nodiscard]] bool contains_null_constant(List* arguments) noexcept {
    for (Expr* argument : postgres_list<Expr>{arguments}) {
        if (Const* constant = pg_node_view{reinterpret_cast<Node*>(argument)}.try_cast<Const>();
            constant != nullptr && constant->constisnull)
            return true;
    }
    return false;
}

} // namespace

/** Resolve a scalar routine through the generated vector-owner registries. */
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
 * Copy vectorizable operator expressions and bind them to vector routines.
 * Unsupported nodes retain their original PostgreSQL-owned pointer.
 */
export List* rewrite_vectorized_expression_list(List* expression_list) {
    if (expression_list == nullptr)
        return expression_list;

    check_stack_depth();

    List* new_qual_list = NIL;

    for (Node* node : postgres_list<Node>{expression_list}) {
        if (node == nullptr)
            return nullptr;

        /*
         * OpExpr / DistinctExpr / NullIfExpr share OpExpr storage but carry
         * distinct tags; try_cast_pg_operator_expression groups all three.
         */
        if (OpExpr* op_expr_node = try_cast_pg_operator_expression(node)) {
            if (list_length(op_expr_node->args) != 2) {
                new_qual_list = lappend(new_qual_list, op_expr_node);
                continue;
            }

            if (!is_pg_const_var_binary_args(op_expr_node->args)) {
                new_qual_list = lappend(new_qual_list, op_expr_node);
                continue;
            }
            if (contains_null_constant(op_expr_node->args)) {
                new_qual_list = lappend(new_qual_list, op_expr_node);
                continue;
            }

            HeapTuple operator_tuple =
                SearchSysCache1(OPEROID, ObjectIdGetDatum(op_expr_node->opno));
            if (!HeapTupleIsValid(operator_tuple)) {
                new_qual_list = lappend(new_qual_list, op_expr_node);
                continue;
            }
            Form_pg_operator operator_form =
                reinterpret_cast<Form_pg_operator>(GETSTRUCT(operator_tuple));
            const Oid procedure_oid = operator_form->oprcode;
            ReleaseSysCache(operator_tuple);

            const std::optional<Oid> vectorized_oid = lookup_vectorized_routine_oid(procedure_oid);
            if (!vectorized_oid) {
                new_qual_list = lappend(new_qual_list, op_expr_node);
                continue;
            }

            OpExpr* vectorized_op_expr_node = bioseq::postgres::support::copy_pg_node(op_expr_node);
            vectorized_op_expr_node->opfuncid = *vectorized_oid;
            new_qual_list = lappend(new_qual_list, vectorized_op_expr_node);

            continue;
        }

        if (BoolExpr* bool_expr = pg_node_view{node}.try_cast<BoolExpr>()) {
            List* rewritten_bool_expr_args = rewrite_vectorized_expression_list(bool_expr->args);
            List* unchanged_arguments =
                list_intersection(rewritten_bool_expr_args, bool_expr->args);
            const bool all_arguments_rewritten = unchanged_arguments == NIL;
            list_free(unchanged_arguments);

            if (all_arguments_rewritten &&
                (bool_expr->boolop == AND_EXPR || bool_expr->boolop == OR_EXPR)) {
                Expr* boolean_clause = nullptr;

                if (bool_expr->boolop == AND_EXPR) {
                    boolean_clause = make_andclause(rewritten_bool_expr_args);
                } else if (bool_expr->boolop == OR_EXPR) {
                    boolean_clause = make_orclause(rewritten_bool_expr_args);
                }

                new_qual_list = lappend(new_qual_list, boolean_clause);
            } else {
                new_qual_list = lappend(new_qual_list, bool_expr);
            }

            continue;
        }

        new_qual_list = lappend(new_qual_list, node);
    }

    return new_qual_list;
}

/**
 * Compile vector expressions against one batch slot.
 * Function-call records are initialized once and reused for every batch.
 */
export List* compile_vector_qualifier_list(TupleTableSlot* slot, List* vectorized_qual) {
    vector_tuple_table_slot_storage* vector_slot =
        reinterpret_cast<vector_tuple_table_slot_storage*>(slot);

    List* vector_qual_list = NIL;
    for (Node* node : postgres_list<Node>{vectorized_qual}) {
        if (OpExpr* op_expr_node = try_cast_pg_operator_expression(node)) {
            int argument_index = 0;
            const int argument_count = list_length(op_expr_node->args);

            vector_qualifier_node* new_vector_qual =
                allocate_zeroed_object<vector_qualifier_node>();
            new_vector_qual->kind = vector_qualifier_node_kind::expression;

            new_vector_qual->payload.expression.function_info = allocate_zeroed_object<FmgrInfo>();
            new_vector_qual->payload.expression.call_info =
                allocate_zeroed_storage<FunctionCallInfoBaseData>(
                    SizeForFunctionCallInfo(argument_count));
            new_vector_qual->payload.expression.arguments =
                allocate_zeroed_array<vector_operand_storage>(argument_count);

            fmgr_info(op_expr_node->opfuncid, new_vector_qual->payload.expression.function_info);
            fmgr_info_set_expr(node, new_vector_qual->payload.expression.function_info);

            InitFunctionCallInfoData(*(new_vector_qual->payload.expression.call_info),
                                     new_vector_qual->payload.expression.function_info,
                                     argument_count,
                                     op_expr_node->inputcollid,
                                     nullptr,
                                     nullptr);

            for (Expr* argument : postgres_list<Expr>{op_expr_node->args}) {
                vector_operand_storage* argument_storage =
                    new_vector_qual->payload.expression.arguments + argument_index;

                if (Const* constant =
                        pg_node_view{reinterpret_cast<Node*>(argument)}.try_cast<Const>()) {
                    argument_storage->kind = vector_operand_storage_kind::constant;
                    argument_storage->datum = constant->constvalue;

                    new_vector_qual->payload.expression.call_info->args[argument_index].value =
                        reinterpret_cast<Datum>(argument_storage);
                    new_vector_qual->payload.expression.call_info->args[argument_index].isnull =
                        constant->constisnull;
                } else {
                    Var* variable = pg_node_view{reinterpret_cast<Node*>(argument)}.try_cast<Var>();
                    Assert(variable != nullptr);
                    const int column_index = variable->varattno - 1;

                    argument_storage->kind = vector_operand_storage_kind::column;
                    argument_storage->datum =
                        vector_slot->virtual_slot.base.tts_values[column_index];

                    new_vector_qual->payload.expression.call_info->args[argument_index].value =
                        reinterpret_cast<Datum>(argument_storage);
                    new_vector_qual->payload.expression.call_info->args[argument_index].isnull =
                        false;
                }

                ++argument_index;
            }

            vector_qual_list = lappend(vector_qual_list, new_vector_qual);
            continue;
        }

        if (BoolExpr* bool_expr = pg_node_view{node}.try_cast<BoolExpr>()) {
            vector_qualifier_node* new_vector_qual =
                allocate_zeroed_object<vector_qualifier_node>();
            new_vector_qual->kind = vector_qualifier_node_kind::boolean_expression;

            new_vector_qual->payload.boolean_expression.kind = bool_expr->boolop;
            new_vector_qual->payload.boolean_expression.expressions =
                compile_vector_qualifier_list(slot, bool_expr->args);

            vector_qual_list = lappend(vector_qual_list, new_vector_qual);

            continue;
        }
    }

    return vector_qual_list;
}

/** Invoke one compiled vector expression in the per-tuple memory context. */
static bool* evaluate_vectorized_expression(vector_qualifier_node* vector_qual,
                                            ExprContext* expression_context) {
    vector_column_storage* evaluation_result;
    {
        bioseq::postgres::support::memory_context_scope memory_scope{
            expression_context->ecxt_per_tuple_memory};
        evaluation_result = reinterpret_cast<vector_column_storage*>(
            DatumGetPointer(FunctionCallInvoke(vector_qual->payload.expression.call_info)));
    }

    return reinterpret_cast<bool*>(evaluation_result->data);
}

/** Combine compiled vector predicate masks with the requested Boolean kind. */
export bool* evaluate_vectorized_qualifiers(TupleTableSlot* slot,
                                            List* vectorized_qual_list,
                                            BoolExprType bool_type,
                                            ExprContext* expression_context) {
    vector_tuple_table_slot_storage* vector_slot =
        reinterpret_cast<vector_tuple_table_slot_storage*>(slot);

    bool* combined_result = nullptr;

    for (vector_qualifier_node* vector_qual :
         postgres_list<vector_qualifier_node>{vectorized_qual_list}) {
        bool* qual_result = nullptr;

        switch (vector_qual->kind) {
        case vector_qualifier_node_kind::expression: {
            qual_result = evaluate_vectorized_expression(vector_qual, expression_context);
            break;
        }
        case vector_qualifier_node_kind::boolean_expression: {
            const BoolExprType nested_kind = vector_qual->payload.boolean_expression.kind;
            if (nested_kind == AND_EXPR || nested_kind == OR_EXPR) {
                qual_result = evaluate_vectorized_qualifiers(
                    slot,
                    vector_qual->payload.boolean_expression.expressions,
                    nested_kind,
                    expression_context);
            }
            break;
        }
        }

        if (combined_result == nullptr) {
            combined_result = qual_result;
        } else {
            const std::uint32_t row_count = vector_slot->row_count;
            if (bool_type == AND_EXPR)
                bioseq::column::vec::mask_and(combined_result, qual_result, row_count);
            else if (bool_type == OR_EXPR)
                bioseq::column::vec::mask_or(combined_result, qual_result, row_count);
        }
    }

    return combined_result;
}

/*-------------------------------------------------------------------------
 *
 * vectorization/columnar_vector_execution.cppm
 *
 * Walks query expression trees to build vectorised-friendly variants:
 *   - pg_is_const_var_binary_args (bioseq.pg_utils.nodes) gates OpExprs to
 *     the (one Const + one Var) shape the vectorised evaluator supports.
 *   - GetVectorizedProcedureOid looks up the "v"-prefixed vectorised
 *     implementation of a scalar procedure.
 *   - CreateVectorizedExprList / ConstructVectorizedQualList convert
 *     qual/expr trees into VectorQual tree structures for the executor.
 *   - ExecuteVectorizedQual runs the compiled VectorQual tree over a
 *     VectorTupleTableSlot and returns the surviving-rows mask.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include "commands/extension.h"
#include "parser/parse_func.h"
#include "utils/syscache.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

export module bioseq.vector.execution;

import bioseq.pg_utils;
import bioseq.vector.types;
import bioseq.column.vector_compare;
import bioseq.pg_utils.nodes;          // DOC0333 — pg_node_try_as_op_expr_family, pg_node_try_as, pg_is_const_var_binary_args
#if defined(BIOSEQ_TEST_INDEXES)
import bioseq.sql.generated.test_indexes.vector_aggregate_registry;
import bioseq.sql.generated.test_indexes.vector_comparison_registry;
#else
import bioseq.sql.generated.production.vector_aggregate_registry;
import bioseq.sql.generated.production.vector_comparison_registry;
#endif

using namespace bioseq::pg_utils;      // DOC0333 — node view helpers live in bioseq::pg_utils

namespace {

struct scalar_procedure_signature final
{
	std::array<char, NAMEDATALEN> name{};
	Oid namespace_oid = InvalidOid;
	std::array<Oid, 2> argument_types{InvalidOid, InvalidOid};
	Oid result_type = InvalidOid;
	std::int16_t argument_count = 0;
	char procedure_kind = PROKIND_FUNCTION;
};

[[nodiscard]] std::optional<scalar_procedure_signature>
read_scalar_procedure_signature(Oid procedure_oid)
{
	HeapTuple procedure_tuple =
		SearchSysCache1(PROCOID, ObjectIdGetDatum(procedure_oid));
	if (!HeapTupleIsValid(procedure_tuple))
		return std::nullopt;

	Form_pg_proc procedure_form =
		reinterpret_cast<Form_pg_proc>(GETSTRUCT(procedure_tuple));
	if (procedure_form->pronargs > 2)
	{
		ReleaseSysCache(procedure_tuple);
		return std::nullopt;
	}

	scalar_procedure_signature signature{
		.namespace_oid = procedure_form->pronamespace,
		.argument_types = {
			procedure_form->pronargs >= 1
				? procedure_form->proargtypes.values[0]
				: InvalidOid,
			procedure_form->pronargs >= 2
				? procedure_form->proargtypes.values[1]
				: InvalidOid},
		.result_type = procedure_form->prorettype,
		.argument_count = procedure_form->pronargs,
		.procedure_kind = procedure_form->prokind,
	};
	strlcpy(
		signature.name.data(),
		NameStr(procedure_form->proname),
		signature.name.size());
	ReleaseSysCache(procedure_tuple);
	return signature;
}

[[nodiscard]] std::optional<Oid>
installed_extension_namespace()
{
	const Oid extension_oid = get_extension_oid("bioseq", true);
	if (!OidIsValid(extension_oid))
		return std::nullopt;

	const Oid namespace_oid = get_extension_schema(extension_oid);
	if (!OidIsValid(namespace_oid))
		return std::nullopt;
	return namespace_oid;
}

[[nodiscard]] std::optional<Oid>
lookup_vector_owner(
	Oid namespace_oid,
	std::string_view vector_name,
	const std::array<Oid, 2>& argument_types,
	Oid expected_result_type)
{
	const char* const vector_name_data = vector_name.data();
	const Oid left_type = argument_types[0];
	const Oid right_type = argument_types[1];
	const Oid vector_oid = bioseq::pg_utils::invoke_pg_as_cpp(
		[namespace_oid, vector_name_data, left_type, right_type]
		{
			char* namespace_name = get_namespace_name(namespace_oid);
			if (namespace_name == nullptr)
				return InvalidOid;
			List* qualified_name = list_make2(
				makeString(namespace_name),
				makeString(pstrdup(vector_name_data)));
			Oid exact_argument_types[2]{left_type, right_type};
			const Oid result = LookupFuncName(
				qualified_name,
				2,
				exact_argument_types,
				true);
			list_free_deep(qualified_name);
			return result;
		});
	if (!OidIsValid(vector_oid)
		|| get_func_rettype(vector_oid) != expected_result_type)
		return std::nullopt;
	return vector_oid;
}

[[nodiscard]] bool
routine_matches(
	Oid procedure_oid,
	Oid namespace_oid,
	std::string_view expected_name)
{
	if (!OidIsValid(procedure_oid))
		return expected_name.empty();
	if (expected_name.empty())
		return false;

	HeapTuple procedure_tuple =
		SearchSysCache1(PROCOID, ObjectIdGetDatum(procedure_oid));
	if (!HeapTupleIsValid(procedure_tuple))
		return false;
	Form_pg_proc procedure_form =
		reinterpret_cast<Form_pg_proc>(GETSTRUCT(procedure_tuple));
	const bool matches =
		procedure_form->pronamespace == namespace_oid
		&& expected_name == NameStr(procedure_form->proname);
	ReleaseSysCache(procedure_tuple);
	return matches;
}

[[nodiscard]] std::optional<Oid>
lookup_vector_aggregate_owner(
	Oid namespace_oid,
	const bioseq::sql::generated::vector_aggregate_registry::
		association& association)
{
	const char* const vector_name_data = association.vector_name.data();
	const int argument_count =
		OidIsValid(association.input_type) ? 1 : 0;
	const Oid input_type = association.input_type;
	const Oid vector_oid = bioseq::pg_utils::invoke_pg_as_cpp(
		[namespace_oid, vector_name_data, argument_count, input_type]
		{
			char* namespace_name = get_namespace_name(namespace_oid);
			if (namespace_name == nullptr)
				return InvalidOid;
			List* qualified_name = list_make2(
				makeString(namespace_name),
				makeString(pstrdup(vector_name_data)));
			Oid exact_argument_types[1]{input_type};
			const Oid result = LookupFuncName(
				qualified_name,
				argument_count,
				argument_count == 0 ? nullptr : exact_argument_types,
				true);
			list_free_deep(qualified_name);
			return result;
		});
	if (!OidIsValid(vector_oid))
		return std::nullopt;

	const std::optional<scalar_procedure_signature> vector_signature =
		read_scalar_procedure_signature(vector_oid);
	if (!vector_signature
		|| vector_signature->procedure_kind != PROKIND_AGGREGATE
		|| vector_signature->argument_count != argument_count
		|| vector_signature->argument_types[0]
			!= association.input_type
		|| vector_signature->result_type != association.result_type)
		return std::nullopt;

	HeapTuple aggregate_tuple =
		SearchSysCache1(AGGFNOID, ObjectIdGetDatum(vector_oid));
	if (!HeapTupleIsValid(aggregate_tuple))
		return std::nullopt;
	Form_pg_aggregate aggregate_form =
		reinterpret_cast<Form_pg_aggregate>(GETSTRUCT(aggregate_tuple));
	const bool matches =
		aggregate_form->aggtranstype == association.state_type
		&& routine_matches(
			aggregate_form->aggtransfn,
			namespace_oid,
			association.transition_name)
		&& routine_matches(
			aggregate_form->aggfinalfn,
			namespace_oid,
			association.final_name);
	ReleaseSysCache(aggregate_tuple);
	if (!matches)
		return std::nullopt;
	return vector_oid;
}

[[nodiscard]] bool
contains_null_constant(List* arguments) noexcept
{
	for (Expr* argument : PList<Expr>{arguments})
	{
		if (Const* constant =
				pg_node_try_as<Const>(pg_as_node(argument));
			constant != nullptr && constant->constisnull)
			return true;
	}
	return false;
}

}  // namespace

/*
 * Resolve a built-in scalar comparison through the generated closed
 * scalar/vector association table and the extension's installed namespace.
 */
export [[nodiscard]] std::optional<Oid>
GetVectorizedProcedureOid(Oid procedure_oid)
{
	const std::optional<scalar_procedure_signature> signature =
		read_scalar_procedure_signature(procedure_oid);
	if (!signature)
		return std::nullopt;

	if (signature->argument_count == 2
		&& signature->procedure_kind != PROKIND_AGGREGATE)
	{
		using namespace
			bioseq::sql::generated::vector_comparison_registry;
		const auto association = std::ranges::find_if(
			all(),
			[signature](const auto& candidate)
			{
				return candidate.scalar_namespace
						== signature->namespace_oid
					&& candidate.scalar_name
						== signature->name.data()
					&& candidate.left_type
						== signature->argument_types[0]
					&& candidate.right_type
						== signature->argument_types[1]
					&& candidate.result_type
						== signature->result_type;
			});
		if (association != all().end())
		{
			const std::optional<Oid> namespace_oid =
				installed_extension_namespace();
			if (!namespace_oid)
				return std::nullopt;
			return lookup_vector_owner(
				*namespace_oid,
				association->vector_name,
				signature->argument_types,
				association->result_type);
		}
	}

	if (signature->procedure_kind != PROKIND_AGGREGATE
		|| signature->argument_count > 1)
		return std::nullopt;
	using namespace
		bioseq::sql::generated::vector_aggregate_registry;
	const Oid input_type =
		signature->argument_count == 0
			? InvalidOid
			: signature->argument_types[0];
	const auto association = std::ranges::find_if(
		all(),
		[signature, input_type](const auto& candidate)
		{
			return candidate.scalar_namespace
					== signature->namespace_oid
				&& candidate.scalar_name == signature->name.data()
				&& candidate.input_type == input_type
				&& candidate.result_type == signature->result_type;
		});
	if (association == all().end())
		return std::nullopt;
	const std::optional<Oid> namespace_oid =
		installed_extension_namespace();
	if (!namespace_oid)
		return std::nullopt;
	return lookup_vector_aggregate_owner(
		*namespace_oid, *association);
}

/*
 * CreateVectorizedExprList rewrites an expression list by swapping each
 * OpExpr-like node for a copy whose opfuncid points at the vectorised
 * implementation. BoolExprs are rebuilt from their rewritten children; any
 * node that can't be vectorised is passed through unchanged.
 */
export List*
CreateVectorizedExprList(List* expression_list)
{
	if (expression_list == NULL)
		return expression_list;

	check_stack_depth();

	List* new_qual_list = NIL;

	for (auto node : PList<Node>(expression_list))
	{
		if (node == NULL)
			return NULL;

		/*
		 * OpExpr / DistinctExpr / NullIfExpr share OpExpr storage but carry
		 * distinct tags; pg_node_try_as_op_expr_family groups all three.
		 */
		if (OpExpr* op_expr_node = pg_node_try_as_op_expr_family(node))
		{
			if (list_length(op_expr_node->args) != 2)
			{
				new_qual_list = lappend(new_qual_list, op_expr_node);
				continue;
			}

			if (!pg_is_const_var_binary_args(op_expr_node->args))
			{
				new_qual_list = lappend(new_qual_list, op_expr_node);
				continue;
			}
			if (contains_null_constant(op_expr_node->args))
			{
				new_qual_list = lappend(new_qual_list, op_expr_node);
				continue;
			}

			HeapTuple operator_tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(op_expr_node->opno));
			Form_pg_operator operator_form = reinterpret_cast<Form_pg_operator>(GETSTRUCT(operator_tuple));
			Oid procedure_oid = operator_form->oprcode;
			ReleaseSysCache(operator_tuple);

			const std::optional<Oid> vectorized_oid =
				GetVectorizedProcedureOid(procedure_oid);
			if (!vectorized_oid)
			{
				new_qual_list = lappend(new_qual_list, op_expr_node);
				continue;
			}

			OpExpr* vectorized_op_expr_node = reinterpret_cast<OpExpr*>(copyObject(op_expr_node));
			vectorized_op_expr_node->opfuncid = *vectorized_oid;
			new_qual_list = lappend(new_qual_list, vectorized_op_expr_node);

			continue;
		}

		if (BoolExpr* bool_expr = pg_node_try_as<BoolExpr>(node))
		{
			List* rewritten_bool_expr_args =
				CreateVectorizedExprList(bool_expr->args);

			if (list_length(list_intersection(rewritten_bool_expr_args, bool_expr->args)) == 0)
			{
				Expr* boolean_clause = NULL;

				if (bool_expr->boolop == AND_EXPR)
				{
					boolean_clause = make_andclause(rewritten_bool_expr_args);
				}
				else if (bool_expr->boolop == OR_EXPR)
				{
					boolean_clause = make_orclause(rewritten_bool_expr_args);
				}

				new_qual_list = lappend(new_qual_list, boolean_clause);
			}
			else
			{
				new_qual_list = lappend(new_qual_list, bool_expr);
			}

			continue;
		}

		new_qual_list = lappend(new_qual_list, node);
	}

	return new_qual_list;
}


/*
 * ConstructVectorizedQualList compiles a vectorised expression list into
 * a VectorQual tree bound to the columns of the supplied VectorTupleTableSlot.
 * Each OpExpr becomes a VectorQual with its FmgrInfo/FunctionCallInfoBaseData
 * pre-initialised so execution avoids per-row setup.
 */
export List*
ConstructVectorizedQualList(TupleTableSlot* slot, List* vectorized_qual)
{
	VectorTupleTableSlot* vector_slot = reinterpret_cast<VectorTupleTableSlot*>(slot);

	List* vector_qual_list = NIL;
	for (auto node : PList<Node>(vectorized_qual))
	{
		if (OpExpr* op_expr_node = pg_node_try_as_op_expr_family(node))
		{
			{
				int argument_index = 0;
				int argument_count = list_length(op_expr_node->args);

				VectorQual* new_vector_qual = make0<VectorQual>();
				new_vector_qual->vectorQualType = VECTOR_QUAL_EXPR;

				new_vector_qual->u.expr.fmgrInfo = make0<FmgrInfo>();
				new_vector_qual->u.expr.fcInfo = make1<FunctionCallInfoBaseData>(SizeForFunctionCallInfo(argument_count));
				new_vector_qual->u.expr.vectorFnArguments =
					make_array<VectorFnArgument>(argument_count);


				fmgr_info(op_expr_node->opfuncid, new_vector_qual->u.expr.fmgrInfo);
				fmgr_info_set_expr(reinterpret_cast<Node*>(node), new_vector_qual->u.expr.fmgrInfo);

				InitFunctionCallInfoData(*(new_vector_qual->u.expr.fcInfo),
										 new_vector_qual->u.expr.fmgrInfo,
										 argument_count, op_expr_node->inputcollid, NULL, NULL);

				for (auto arg : PList<Expr>(op_expr_node->args))
				{
					VectorFnArgument* vector_fn_argument =
						new_vector_qual->u.expr.vectorFnArguments + argument_index;

					if (Const* const_expr = pg_node_try_as<Const>(pg_as_node(arg)))
					{
						vector_fn_argument->type = VECTOR_FN_ARG_CONSTANT;
						vector_fn_argument->arg = const_expr->constvalue;

						new_vector_qual->u.expr.fcInfo->args[argument_index].value = reinterpret_cast<Datum>(vector_fn_argument);
						new_vector_qual->u.expr.fcInfo->args[argument_index].isnull = const_expr->constisnull;
					}
					else if (Var* variable_expr = pg_node_try_as<Var>(pg_as_node(arg)))
					{
						int column_index = variable_expr->varattno - 1;

						vector_fn_argument->type = VECTOR_FN_ARG_VAR;
						vector_fn_argument->arg = vector_slot->tts.tts_values[column_index];

						new_vector_qual->u.expr.fcInfo->args[argument_index].value = reinterpret_cast<Datum>(vector_fn_argument);
						new_vector_qual->u.expr.fcInfo->args[argument_index].isnull = false;
					}

					argument_index++;
				}

				vector_qual_list = lappend(vector_qual_list, new_vector_qual);
				continue;
			}
		}

		if (BoolExpr* bool_expr = pg_node_try_as<BoolExpr>(node))
		{
			VectorQual* new_vector_qual = make0<VectorQual>();
			new_vector_qual->vectorQualType = VECTOR_QUAL_BOOL_EXPR;

			List* new_qual_expression_args =
				ConstructVectorizedQualList(slot, bool_expr->args);

			new_vector_qual->u.boolExpr.boolExprType = bool_expr->boolop;

			for (auto qual_child : PList<VectorQual>(new_qual_expression_args))
			{
				new_vector_qual->u.boolExpr.vectorQualExprList =
					lappend(new_vector_qual->u.boolExpr.vectorQualExprList, qual_child);
			}

			vector_qual_list = lappend(vector_qual_list, new_vector_qual);

			continue;
		}
	}

	return vector_qual_list;
}

/*
 * executeVectorizedExpr invokes the compiled fmgrInfo function using the
 * per-tuple memory context. The return is a VectorColumn whose 'value'
 * array we reinterpret as a bool row mask.
 */
static bool*
executeVectorizedExpr(VectorQual* vector_qual, ExprContext* expression_context)
{
	VectorColumn* evaluation_result;
	{
		MemoryContextScope ctx_guard{expression_context->ecxt_per_tuple_memory};
		evaluation_result =
			reinterpret_cast<VectorColumn*>(vector_qual->u.expr.fmgrInfo->fn_addr(vector_qual->u.expr.fcInfo));
	}

	return reinterpret_cast<bool*>(evaluation_result->value);
}

/*
 * ExecuteVectorizedQual evaluates each VectorQual in vectorizedQualList and
 * combines the resulting bool masks according to boolType. The first mask
 * is returned as-is; subsequent masks are AND/ORed into it in place.
 */
export bool*
ExecuteVectorizedQual(TupleTableSlot* slot, List* vectorized_qual_list,
			BoolExprType bool_type, ExprContext* expression_context)
{
	VectorTupleTableSlot* vector_slot = reinterpret_cast<VectorTupleTableSlot*>(slot);

	bool* combined_result = NULL;

	for (auto vector_qual : PList<VectorQual>(vectorized_qual_list))
	{
		bool* qual_result = NULL;

		switch (vector_qual->vectorQualType)
		{
			case VECTOR_QUAL_EXPR:
			{
				qual_result = executeVectorizedExpr(vector_qual, expression_context);
				break;
			}
			case VECTOR_QUAL_BOOL_EXPR:
			{
				if (vector_qual->u.boolExpr.boolExprType == AND_EXPR)
				{
					qual_result = ExecuteVectorizedQual(slot,
										vector_qual->u.boolExpr.vectorQualExprList,
										AND_EXPR, expression_context);
				}
				else if (vector_qual->u.boolExpr.boolExprType == OR_EXPR)
				{
					qual_result = ExecuteVectorizedQual(slot,
										vector_qual->u.boolExpr.vectorQualExprList,
										OR_EXPR, expression_context);
				}
				break;
			}
		}

		if (combined_result == NULL)
		{
			combined_result = qual_result;
		}
		else
		{
			const std::uint32_t n = vector_slot->dimension;
			if (bool_type == AND_EXPR)
				bioseq::column::vec::mask_and(
					combined_result, qual_result, n);
			else if (bool_type == OR_EXPR)
				bioseq::column::vec::mask_or(
					combined_result, qual_result, n);
		}
	}

	return combined_result;
}

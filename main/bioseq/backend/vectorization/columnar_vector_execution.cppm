/*-------------------------------------------------------------------------
 *
 * vectorization/columnar_vector_execution.cppm
 *
 * Walks query expression trees to build vectorised-friendly variants:
 *   - CheckOpExprArgumentRules rejects OpExprs that do not match the
 *     (one Const + one Var) shape the vectorised evaluator supports.
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

#include "columnar/vectorization/columnar_vector_types.h"

export module bioseq_vector_execution;

import pg_utils;

extern "C++" {

/*
 * CheckOpExprArgumentRules validates that a vectorised OpExpr receives
 * exactly one Const and one Var argument. Any other combination (two
 * Consts, two Vars, non-Const-non-Var nodes) is rejected. Returns true
 * when the combination is invalid.
 */
bool
CheckOpExprArgumentRules(List* args)
{
	bool invalid_argument = false;

	bool seen_const_argument = false;
	bool seen_var_argument = false;

	for (auto arg : PList<Expr>(args))
	{
		if (invalid_argument)
			break;

		if (IsA(arg, Const))
		{
			if (seen_const_argument)
			{
				invalid_argument = true;
				break;
			}
			seen_const_argument = true;
		}
		else if (IsA(arg, Var))
		{
			if (seen_var_argument)
			{
				invalid_argument = true;
				break;
			}
			seen_var_argument = true;
		}
		else
		{
			invalid_argument = true;
			break;
		}
	}

	return invalid_argument;
}

/*
 * GetVectorizedProcedureOid resolves the vectorised implementation of
 * procedureOid by prepending 'v' to its name and looking up a match with
 * identical argument types. Returns true when a unique matching function
 * is found and writes the Oid to vectorizedProcedureOid.
 */
bool
GetVectorizedProcedureOid(Oid procedure_oid, Oid* vectorized_procedure_oid)
{
	HeapTuple procedure_tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(procedure_oid));
	Form_pg_proc procedure_form = reinterpret_cast<Form_pg_proc>(GETSTRUCT(procedure_tuple));

	const int original_procedure_name_length = strlen(NameStr(procedure_form->proname));

	char* vectorized_procedure_name = make_array<char>(original_procedure_name_length + 2);

	vectorized_procedure_name[0] = 'v';

	memcpy(vectorized_procedure_name + 1,
		NameStr(procedure_form->proname),
		original_procedure_name_length);

	ReleaseSysCache(procedure_tuple);

	auto function_names = PList<void>::of(makeString(vectorized_procedure_name));

	Oid* argument_types = make_array<Oid>(procedure_form->pronargs);

	for (int argument_index = 0; argument_index < procedure_form->pronargs; argument_index++)
		argument_types[argument_index] = procedure_form->proargtypes.values[argument_index];

	bool returns_set;
	Oid return_type;
	int variadic_args_count;
	Oid variadic_array_type;
	Oid* resolved_argument_types;

	FuncDetailCode func_detail_result = func_get_detail(function_names, NIL, NIL,
								procedure_form->pronargs, argument_types,
								false, true, false,
								vectorized_procedure_oid,
								&return_type, &returns_set,
								&variadic_args_count, &variadic_array_type,
								&resolved_argument_types, NULL);

	if ((func_detail_result == FUNCDETAIL_NOTFOUND || func_detail_result == FUNCDETAIL_MULTIPLE) ||
		!OidIsValid(*vectorized_procedure_oid) ||
		(procedure_form->pronargs != 0 &&
		 memcmp(argument_types, resolved_argument_types, procedure_form->pronargs * sizeof(Oid)) != 0))
	{
		return false;
	}

	return true;
}

/*
 * CreateVectorizedExprList rewrites an expression list by swapping each
 * OpExpr-like node for a copy whose opfuncid points at the vectorised
 * implementation. BoolExprs are rebuilt from their rewritten children; any
 * node that can't be vectorised is passed through unchanged.
 */
List*
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

		switch (nodeTag(node))
		{
			case T_OpExpr:
			case T_DistinctExpr:	/* struct-equivalent to OpExpr */
			case T_NullIfExpr:		/* struct-equivalent to OpExpr */
			{
				OpExpr* op_expr_node = reinterpret_cast<OpExpr*>(node);

				if (list_length(op_expr_node->args) != 2)
				{
					new_qual_list = lappend(new_qual_list, op_expr_node);
					break;
				}

				if (CheckOpExprArgumentRules(op_expr_node->args))
				{
					new_qual_list = lappend(new_qual_list, op_expr_node);
					break;
				}

				HeapTuple operator_tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(op_expr_node->opno));
				Form_pg_operator operator_form = reinterpret_cast<Form_pg_operator>(GETSTRUCT(operator_tuple));
				Oid procedure_oid = operator_form->oprcode;
				ReleaseSysCache(operator_tuple);

				Oid vectorized_oid;
				if (!GetVectorizedProcedureOid(procedure_oid, &vectorized_oid))
				{
					new_qual_list = lappend(new_qual_list, op_expr_node);
					break;
				}

				OpExpr* vectorized_op_expr_node = reinterpret_cast<OpExpr*>(copyObject(op_expr_node));
				vectorized_op_expr_node->opfuncid = vectorized_oid;
				new_qual_list = lappend(new_qual_list, vectorized_op_expr_node);

				break;
			}

			case T_BoolExpr:
			{
				BoolExpr* bool_expr = castNode(BoolExpr, node);

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

				break;
			}
			default:
			{
				new_qual_list = lappend(new_qual_list, node);
				break;
			}
		}
	}

	return new_qual_list;
}


/*
 * ConstructVectorizedQualList compiles a vectorised expression list into
 * a VectorQual tree bound to the columns of the supplied VectorTupleTableSlot.
 * Each OpExpr becomes a VectorQual with its FmgrInfo/FunctionCallInfoBaseData
 * pre-initialised so execution avoids per-row setup.
 */
List*
ConstructVectorizedQualList(TupleTableSlot* slot, List* vectorized_qual)
{
	VectorTupleTableSlot* vector_slot = reinterpret_cast<VectorTupleTableSlot*>(slot);

	List* vector_qual_list = NIL;
	for (auto node : PList<Node>(vectorized_qual))
	{
		switch (nodeTag(node))
		{
			case T_OpExpr:
			case T_DistinctExpr:
			case T_NullIfExpr:
			{
				OpExpr* op_expr_node = reinterpret_cast<OpExpr*>(node);

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

					if (IsA(arg, Const))
					{
						Const* const_expr = reinterpret_cast<Const*>(arg);

						vector_fn_argument->type = VECTOR_FN_ARG_CONSTANT;
						vector_fn_argument->arg = const_expr->constvalue;

						new_vector_qual->u.expr.fcInfo->args[argument_index].value = reinterpret_cast<Datum>(vector_fn_argument);
						new_vector_qual->u.expr.fcInfo->args[argument_index].isnull = const_expr->constisnull;
					}
					else if (IsA(arg, Var))
					{
						Var* variable_expr = reinterpret_cast<Var*>(arg);
						int column_index = variable_expr->varattno - 1;

						vector_fn_argument->type = VECTOR_FN_ARG_VAR;
						vector_fn_argument->arg = vector_slot->tts.tts_values[column_index];

						new_vector_qual->u.expr.fcInfo->args[argument_index].value = reinterpret_cast<Datum>(vector_fn_argument);
						new_vector_qual->u.expr.fcInfo->args[argument_index].isnull = false;
					}

					argument_index++;
				}

				vector_qual_list = lappend(vector_qual_list, new_vector_qual);
				break;
			}

			case T_BoolExpr:
			{
				BoolExpr* bool_expr = castNode(BoolExpr, node);

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

				break;
			}

			default:
				break;
		}
	}

	return vector_qual_list;
}

/*
 * vectorizedAnd / vectorizedOr perform element-wise boolean combination of
 * two bool arrays sized by dimension. Written as plain loops so the
 * compiler can auto-vectorise; keep them out of hot paths without further
 * tuning.
 */
static void
vectorizedAnd(bool* left, bool* right, int dimension)
{
	for (int element_index = 0; element_index < dimension; element_index++)
	{
		left[element_index] &= right[element_index];
	}
}

static void
vectorizedOr(bool* left, bool* right, int dimension)
{
	for (int element_index = 0; element_index < dimension; element_index++)
	{
		left[element_index] |= right[element_index];
	}
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
bool*
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
			if (bool_type == AND_EXPR)
				vectorizedAnd(combined_result, qual_result, vector_slot->dimension);
			else if (bool_type == OR_EXPR)
				vectorizedOr(combined_result, qual_result, vector_slot->dimension);
		}
	}

	return combined_result;
}

}

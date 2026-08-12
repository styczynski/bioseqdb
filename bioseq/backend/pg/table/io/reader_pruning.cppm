/**
 * @file
 * @brief Predicate-driven chunk-group pruning for Bioseq table reads.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table.io:pruning;

import bioseq.common.logging;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.support;
import bioseq.postgres.types.compact_sequence;

using namespace bioseq::postgres::support;

static Oid
get_operator_for_type(Oid type_id, Oid access_method_id, int16_t strategy_number)
{
	/* Get default operator class from pg_opclass */
	Oid operator_class_id = GetDefaultOpClass(type_id, access_method_id);

	Oid operator_family = get_opclass_family(operator_class_id);

	Oid operator_id = get_opfamily_member(operator_family, type_id, type_id, strategy_number);

	return operator_id;
}

static OpExpr *
make_operator_expression(Var *variable, int16_t strategy_number)
{
	Oid type_id = variable->vartype;
	Oid type_mod_id = variable->vartypmod;
	Oid collation_id = variable->varcollid;

	Oid access_method_id = BTREE_AM_OID;

	/* Load the operator from system catalogs */
	Oid operator_id = get_operator_for_type(type_id, access_method_id, strategy_number);

	/* No operator for typeId, search for operator class type */
	if (operator_id == InvalidOid)
	{
		Oid operator_class_id = GetDefaultOpClass(type_id, access_method_id);
		Oid opcintype = get_opclass_input_type(operator_class_id);
		operator_id = get_operator_for_type(opcintype, access_method_id, strategy_number);
	}

	Const *constant_value = makeNullConst(type_id, type_mod_id, collation_id);

	/* Now make the expression with the given variable and a null constant */
	OpExpr *expression = reinterpret_cast<OpExpr *>(make_opclause(operator_id,
												  InvalidOid, /* no result type yet */
												  false, /* no return set */
												  reinterpret_cast<Expr *>(variable),
												  reinterpret_cast<Expr *>(constant_value),
												  InvalidOid, collation_id));

	/* Set implementing function id and result type */
	expression->opfuncid = get_opcode(operator_id);
	expression->opresulttype = get_func_rettype(expression->opfuncid);

	return expression;
}

static Node *
build_chunk_constraint(Var *variable)
{
	OpExpr *less_than_expr = make_operator_expression(variable, BTLessEqualStrategyNumber);
	OpExpr *greater_than_expr = make_operator_expression(variable, BTGreaterEqualStrategyNumber);

	Node *base_constraint = make_and_qual(reinterpret_cast<Node *>(less_than_expr),
										 reinterpret_cast<Node *>(greater_than_expr));

	return base_constraint;
}

static void
update_chunk_constraint_bounds(Node *base_constraint, Datum min_value, Datum max_value)
{
	BoolExpr *and_expr = reinterpret_cast<BoolExpr *>(base_constraint);
	bioseq::postgres::support::postgres_list<Node> arg_list{and_expr->args};
	Node *less_than_expr = arg_list.get_first();
	Node *greater_than_expr = arg_list.get_second();

	Node *min_node = get_rightop(reinterpret_cast<Expr *>(greater_than_expr));
	Node *max_node = get_rightop(reinterpret_cast<Expr *>(less_than_expr));

	Const *min_constant = expect_pg_node<Const>(min_node);
	Const *max_constant = expect_pg_node<Const>(max_node);

	min_constant->constvalue = min_value;
	max_constant->constvalue = max_value;

	min_constant->constisnull = false;
	max_constant->constisnull = false;

	min_constant->constbyval = true;
	max_constant->constbyval = true;
}

export List *
collect_clause_variables(List *filter_clauses, int natts)
{
	/*
	 * We don't recurse into or include aggregates, window functions, or
	 * PHVs. We don't expect any PHVs during execution; and Vars found inside
	 * an aggregate or window function aren't going to be useful in forming
	 * constraints that can be refuted.
	 */
	int flags = 0;
	List *vars = pull_var_clause(reinterpret_cast<Node *>(filter_clauses), flags);
	Var **deduplicate = bioseq::postgres::support::allocate_zeroed_array<Var *>(natts);

	for (auto node : bioseq::postgres::support::postgres_list<Node>(vars))
	{
		Var *var = expect_pg_node<Var>(node);
		int idx = var->varattno - 1;

		if (deduplicate[idx] != NULL)
		{
			/* if they have the same varattno, the rest should be identical */
			Assert(equal(var, deduplicate[idx]));
		}

		deduplicate[idx] = var;
	}

	bioseq::postgres::support::postgres_list<Var> filter_variables;
	for (int i = 0; i < natts; i++)
	{
		Var *var = deduplicate[i];
		if (var != NULL)
		{
			filter_variables.append(var);
		}
	}

	pfree(deduplicate);

	return filter_variables.release();
}

export bool *
select_chunk_mask(stripe_skip_list *skip_list, List *filter_clauses,
				  List *filter_variables, int64 *filtered_chunk_groups)
{
	uint32_t chunk_index = 0;

	bool *selected_chunk_mask = bioseq::postgres::support::allocate_zeroed_array<bool>(skip_list->chunk_count);
	memset(selected_chunk_mask, true, skip_list->chunk_count * sizeof(bool));
	const Oid sequence_type_id = bioseq_seq_type_oid_missing_ok();

	for (auto column : bioseq::postgres::support::postgres_list<Var>(filter_variables))
	{
		if (column->varattno <= 0 ||
			static_cast<uint32_t>(column->varattno) > skip_list->column_count)
		{
			bioseq_log<logging_level::error, logging_target::postgres>("restriction column {} is outside stripe column count {}",
				   column->varattno, skip_list->column_count);
		}

		uint32_t column_index = column->varattno - 1;

		/* Sequence extrema are never a sound proxy for logical rank-stream
		 * order, so claimed min/max metadata is ignored conservatively. */
		if (OidIsValid(sequence_type_id)
			&& column->vartype == sequence_type_id)
		{
			continue;
		}

		/* if this column's data type doesn't have a comparator, skip it */
		FmgrInfo *comparison_function = find_index_support_function(column->vartype,
															 BTREE_AM_OID,
															 BTORDER_PROC);
		if (comparison_function == NULL)
		{
			continue;
		}

		Node *base_constraint = build_chunk_constraint(column);
		for (chunk_index = 0; chunk_index < skip_list->chunk_count; chunk_index++)
		{
			chunk_skip_node *chunk_skip_nodes =
				skip_list->chunk_skip_nodes[column_index];
			chunk_skip_node *chunk_skip_node_entry = &chunk_skip_nodes[chunk_index];

			/*
			 * A column chunk with comparable data type can miss min/max values
			 * if all values in the chunk are NULL.
			 */
			if (!chunk_skip_node_entry->has_min_max)
			{
				continue;
			}

			update_chunk_constraint_bounds(base_constraint, chunk_skip_node_entry->minimum_value,
							 chunk_skip_node_entry->maximum_value);

			auto constraint_list =
				bioseq::postgres::support::postgres_list<Node>::make_singleton(base_constraint);
			bool predicate_refuted =
				predicate_refuted_by(constraint_list.get(), filter_clauses, false);
			if (predicate_refuted && selected_chunk_mask[chunk_index])
			{
				selected_chunk_mask[chunk_index] = false;
				*filtered_chunk_groups += 1;
			}
		}
	}

	return selected_chunk_mask;
}

export stripe_skip_list *
select_chunk_skip_list(stripe_skip_list *skip_list, bool *projected_column_mask,
					  bool *selected_chunk_mask)
{
	uint32_t selected_chunk_count = 0;
	uint32_t chunk_index = 0;
	uint32_t column_index = 0;
	uint32_t column_count = skip_list->column_count;
	uint32_t selected_chunk_index = 0;

	for (chunk_index = 0; chunk_index < skip_list->chunk_count; chunk_index++)
	{
		if (selected_chunk_mask[chunk_index])
		{
			selected_chunk_count++;
		}
	}

	if (selected_chunk_count == skip_list->chunk_count)
	{
		return skip_list;
	}

	chunk_skip_node **selected_chunk_skip_node_array =
		bioseq::postgres::support::allocate_zeroed_array<chunk_skip_node*>(column_count);

	for (column_index = 0; column_index < column_count; column_index++)
	{
		bool first_column = column_index == 0;
		selected_chunk_index = 0;

		/* first column's chunk skip node is always read */
		if (!projected_column_mask[column_index] && !first_column)
		{
			selected_chunk_skip_node_array[column_index] = NULL;
			continue;
		}

		Assert(skip_list->chunk_skip_nodes[column_index] != NULL);

		selected_chunk_skip_node_array[column_index] = bioseq::postgres::support::allocate_zeroed_array<chunk_skip_node>(selected_chunk_count);

		for (chunk_index = 0; chunk_index < skip_list->chunk_count; chunk_index++)
		{
			if (selected_chunk_mask[chunk_index])
			{
				selected_chunk_skip_node_array[column_index][selected_chunk_index] =
					skip_list->chunk_skip_nodes[column_index][chunk_index];
				selected_chunk_index++;
			}
		}
	}

	selected_chunk_index = 0;
	uint32_t *chunk_group_row_counts = bioseq::postgres::support::allocate_zeroed_array<uint32_t>(selected_chunk_count);
	uint32_t *chunk_group_row_offsets = bioseq::postgres::support::allocate_zeroed_array<uint32_t>(selected_chunk_count);
	uint32_t *chunk_group_deleted_rows = bioseq::postgres::support::allocate_zeroed_array<uint32_t>(selected_chunk_count);

	for (chunk_index = 0; chunk_index < skip_list->chunk_count; chunk_index++)
	{
		if (selected_chunk_mask[chunk_index])
		{
			chunk_group_row_counts[selected_chunk_index] =
				skip_list->chunk_group_row_counts[chunk_index];
			chunk_group_row_offsets[selected_chunk_index] =
				skip_list->chunk_group_row_offsets[chunk_index];
			chunk_group_deleted_rows[selected_chunk_index] =
				skip_list->chunk_group_deleted_rows[chunk_index];
			selected_chunk_index++;
		}
	}

	stripe_skip_list *selected_chunk_skip_list = bioseq::postgres::support::allocate_zeroed_object<stripe_skip_list>();
	selected_chunk_skip_list->chunk_skip_nodes = selected_chunk_skip_node_array;
	selected_chunk_skip_list->chunk_count = selected_chunk_count;
	selected_chunk_skip_list->column_count = skip_list->column_count;
	selected_chunk_skip_list->chunk_group_row_counts = chunk_group_row_counts;
	selected_chunk_skip_list->chunk_group_row_offsets = chunk_group_row_offsets;
	selected_chunk_skip_list->chunk_group_deleted_rows = chunk_group_deleted_rows;

	return selected_chunk_skip_list;
}

export uint32_t
count_stripe_skip_rows(stripe_skip_list *skip_list)
{
	uint32_t stripe_skip_list_row_count = 0;
	uint32_t chunk_index = 0;
	uint32_t *chunk_group_row_counts = skip_list->chunk_group_row_counts;

	for (chunk_index = 0; chunk_index < skip_list->chunk_count; chunk_index++)
	{
		uint32_t chunk_group_row_count = chunk_group_row_counts[chunk_index];
		stripe_skip_list_row_count += chunk_group_row_count;
	}

	return stripe_skip_list_row_count;
}

export bool *
build_projected_column_mask(uint32_t column_count, List *projected_columns)
{
	bool *projected_column_mask = bioseq::postgres::support::allocate_zeroed_array<bool>(column_count);

	for (auto attno : bioseq::postgres::support::postgres_integer_list{projected_columns})
	{
		if (attno <= 0 || static_cast<uint32_t>(attno) > column_count)
		{
			bioseq_log<logging_level::error, logging_target::postgres>("projected column {} is outside column count {}",
				   attno, column_count);
		}

		/* attno is 1-indexed; projectedColumnMask is 0-indexed */
		int column_index = attno - 1;
		projected_column_mask[column_index] = true;
	}

	return projected_column_mask;
}

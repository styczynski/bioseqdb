/*-------------------------------------------------------------------------
 *
 * table/io/reader_pruning.cppm
 *
 * Chunk-group pruning helpers for the bioseq table reader.
 *
 *-------------------------------------------------------------------------
 */
module;
#include "common.h"

export module bioseq.table.io:pruning;

import bioseq.common.mlogging;
import bioseq.storage.metadata;
import bioseq.pg_utils;
import bioseq.pg_utils.nodes;   // DOC0333 — pg_node_expect<Const>/<Var>

using namespace bioseq::pg_utils;   // DOC0333 — node view helpers live in bioseq::pg_utils

static Oid
GetOperatorByType(Oid typeId, Oid accessMethodId, int16_t strategyNumber)
{
	/* Get default operator class from pg_opclass */
	Oid operatorClassId = GetDefaultOpClass(typeId, accessMethodId);

	Oid operatorFamily = get_opclass_family(operatorClassId);

	Oid operatorId = get_opfamily_member(operatorFamily, typeId, typeId, strategyNumber);

	return operatorId;
}

static OpExpr *
MakeOpExpression(Var *variable, int16_t strategyNumber)
{
	Oid typeId = variable->vartype;
	Oid typeModId = variable->vartypmod;
	Oid collationId = variable->varcollid;

	Oid accessMethodId = BTREE_AM_OID;

	/* Load the operator from system catalogs */
	Oid operatorId = GetOperatorByType(typeId, accessMethodId, strategyNumber);

	/* No operator for typeId, search for operator class type */
	if (operatorId == InvalidOid)
	{
		Oid operatorClassId = GetDefaultOpClass(typeId, accessMethodId);
		Oid opcintype = get_opclass_input_type(operatorClassId);
		operatorId = GetOperatorByType(opcintype, accessMethodId, strategyNumber);
	}

	Const *constantValue = makeNullConst(typeId, typeModId, collationId);

	/* Now make the expression with the given variable and a null constant */
	OpExpr *expression = reinterpret_cast<OpExpr *>(make_opclause(operatorId,
												  InvalidOid, /* no result type yet */
												  false, /* no return set */
												  reinterpret_cast<Expr *>(variable),
												  reinterpret_cast<Expr *>(constantValue),
												  InvalidOid, collationId));

	/* Set implementing function id and result type */
	expression->opfuncid = get_opcode(operatorId);
	expression->opresulttype = get_func_rettype(expression->opfuncid);

	return expression;
}

static Node *
BuildBaseConstraint(Var *variable)
{
	OpExpr *lessThanExpr = MakeOpExpression(variable, BTLessEqualStrategyNumber);
	OpExpr *greaterThanExpr = MakeOpExpression(variable, BTGreaterEqualStrategyNumber);

	Node *baseConstraint = make_and_qual(reinterpret_cast<Node *>(lessThanExpr),
										 reinterpret_cast<Node *>(greaterThanExpr));

	return baseConstraint;
}

static void
UpdateConstraint(Node *baseConstraint, Datum minValue, Datum maxValue)
{
	BoolExpr *andExpr = reinterpret_cast<BoolExpr *>(baseConstraint);
	PList<Node> argList{andExpr->args};
	Node *lessThanExpr = argList.first();
	Node *greaterThanExpr = argList.second();

	Node *minNode = get_rightop(reinterpret_cast<Expr *>(greaterThanExpr));
	Node *maxNode = get_rightop(reinterpret_cast<Expr *>(lessThanExpr));

	Const *minConstant = pg_node_expect<Const>(minNode);
	Const *maxConstant = pg_node_expect<Const>(maxNode);

	minConstant->constvalue = minValue;
	maxConstant->constvalue = maxValue;

	minConstant->constisnull = false;
	maxConstant->constisnull = false;

	minConstant->constbyval = true;
	maxConstant->constbyval = true;
}

export List *
GetClauseVars(List *whereClauseList, int natts)
{
	/*
	 * We don't recurse into or include aggregates, window functions, or
	 * PHVs. We don't expect any PHVs during execution; and Vars found inside
	 * an aggregate or window function aren't going to be useful in forming
	 * constraints that can be refuted.
	 */
	int flags = 0;
	List *vars = pull_var_clause(reinterpret_cast<Node *>(whereClauseList), flags);
	Var **deduplicate = make_array<Var *>(natts);

	for (auto node : PList<Node>(vars))
	{
		Var *var = pg_node_expect<Var>(node);
		int idx = var->varattno - 1;

		if (deduplicate[idx] != NULL)
		{
			/* if they have the same varattno, the rest should be identical */
			Assert(equal(var, deduplicate[idx]));
		}

		deduplicate[idx] = var;
	}

	PList<Var> whereClauseVars;
	for (int i = 0; i < natts; i++)
	{
		Var *var = deduplicate[i];
		if (var != NULL)
		{
			whereClauseVars.append(var);
		}
	}

	pfree(deduplicate);

	return whereClauseVars;
}

export bool *
SelectedChunkMask(StripeSkipList *stripeSkipList, List *whereClauseList,
				  List *whereClauseVars, int64 *chunkGroupsFiltered)
{
	uint32_t chunkIndex = 0;

	bool *selectedChunkMask = make_array<bool>(stripeSkipList->chunkCount);
	memset(selectedChunkMask, true, stripeSkipList->chunkCount * sizeof(bool));

	for (auto column : PList<Var>(whereClauseVars))
	{
		if (column->varattno <= 0 ||
			static_cast<uint32_t>(column->varattno) > stripeSkipList->columnCount)
		{
			bioseq_log<ERR, PG>("restriction column {} is outside stripe column count {}",
				   column->varattno, stripeSkipList->columnCount);
		}

		uint32_t columnIndex = column->varattno - 1;

		/* if this column's data type doesn't have a comparator, skip it */
		FmgrInfo *comparisonFunction = GetFunctionInfoOrNull(column->vartype,
															 BTREE_AM_OID,
															 BTORDER_PROC);
		if (comparisonFunction == NULL)
		{
			continue;
		}

		Node *baseConstraint = BuildBaseConstraint(column);
		for (chunkIndex = 0; chunkIndex < stripeSkipList->chunkCount; chunkIndex++)
		{
			ColumnChunkSkipNode *chunkSkipNodeArray =
				stripeSkipList->chunkSkipNodeArray[columnIndex];
			ColumnChunkSkipNode *chunkSkipNode = &chunkSkipNodeArray[chunkIndex];

			/*
			 * A column chunk with comparable data type can miss min/max values
			 * if all values in the chunk are NULL.
			 */
			if (!chunkSkipNode->hasMinMax)
			{
				continue;
			}

			UpdateConstraint(baseConstraint, chunkSkipNode->minimumValue,
							 chunkSkipNode->maximumValue);

			auto constraintList = PList<Node>::of(baseConstraint);
			bool predicateRefuted =
				predicate_refuted_by(constraintList, whereClauseList, false);
			if (predicateRefuted && selectedChunkMask[chunkIndex])
			{
				selectedChunkMask[chunkIndex] = false;
				*chunkGroupsFiltered += 1;
			}
		}
	}

	return selectedChunkMask;
}

export StripeSkipList *
SelectedChunkSkipList(StripeSkipList *stripeSkipList, bool *projectedColumnMask,
					  bool *selectedChunkMask)
{
	uint32_t selectedChunkCount = 0;
	uint32_t chunkIndex = 0;
	uint32_t columnIndex = 0;
	uint32_t columnCount = stripeSkipList->columnCount;
	uint32_t selectedChunkIndex = 0;

	for (chunkIndex = 0; chunkIndex < stripeSkipList->chunkCount; chunkIndex++)
	{
		if (selectedChunkMask[chunkIndex])
		{
			selectedChunkCount++;
		}
	}

	if (selectedChunkCount == stripeSkipList->chunkCount)
	{
		return stripeSkipList;
	}

	ColumnChunkSkipNode **selectedChunkSkipNodeArray =
		make_array<ColumnChunkSkipNode*>(columnCount);

	for (columnIndex = 0; columnIndex < columnCount; columnIndex++)
	{
		bool firstColumn = columnIndex == 0;
		selectedChunkIndex = 0;

		/* first column's chunk skip node is always read */
		if (!projectedColumnMask[columnIndex] && !firstColumn)
		{
			selectedChunkSkipNodeArray[columnIndex] = NULL;
			continue;
		}

		Assert(stripeSkipList->chunkSkipNodeArray[columnIndex] != NULL);

		selectedChunkSkipNodeArray[columnIndex] = make_array<ColumnChunkSkipNode>(selectedChunkCount);

		for (chunkIndex = 0; chunkIndex < stripeSkipList->chunkCount; chunkIndex++)
		{
			if (selectedChunkMask[chunkIndex])
			{
				selectedChunkSkipNodeArray[columnIndex][selectedChunkIndex] =
					stripeSkipList->chunkSkipNodeArray[columnIndex][chunkIndex];
				selectedChunkIndex++;
			}
		}
	}

	selectedChunkIndex = 0;
	uint32_t *chunkGroupRowCounts = make_array<uint32_t>(selectedChunkCount);
	uint32_t *chunkGroupRowOffset = make_array<uint32_t>(selectedChunkCount);
	uint32_t *chunkGroupDeletedRows = make_array<uint32_t>(selectedChunkCount);

	for (chunkIndex = 0; chunkIndex < stripeSkipList->chunkCount; chunkIndex++)
	{
		if (selectedChunkMask[chunkIndex])
		{
			chunkGroupRowCounts[selectedChunkIndex] =
				stripeSkipList->chunkGroupRowCounts[chunkIndex];
			chunkGroupRowOffset[selectedChunkIndex] =
				stripeSkipList->chunkGroupRowOffset[chunkIndex];
			chunkGroupDeletedRows[selectedChunkIndex] =
				stripeSkipList->chunkGroupDeletedRows[chunkIndex];
			selectedChunkIndex++;
		}
	}

	StripeSkipList *selectedChunkSkipList = make0<StripeSkipList>();
	selectedChunkSkipList->chunkSkipNodeArray = selectedChunkSkipNodeArray;
	selectedChunkSkipList->chunkCount = selectedChunkCount;
	selectedChunkSkipList->columnCount = stripeSkipList->columnCount;
	selectedChunkSkipList->chunkGroupRowCounts = chunkGroupRowCounts;
	selectedChunkSkipList->chunkGroupRowOffset = chunkGroupRowOffset;
	selectedChunkSkipList->chunkGroupDeletedRows = chunkGroupDeletedRows;

	return selectedChunkSkipList;
}

export uint32_t
StripeSkipListRowCount(StripeSkipList *stripeSkipList)
{
	uint32_t stripeSkipListRowCount = 0;
	uint32_t chunkIndex = 0;
	uint32_t *chunkGroupRowCounts = stripeSkipList->chunkGroupRowCounts;

	for (chunkIndex = 0; chunkIndex < stripeSkipList->chunkCount; chunkIndex++)
	{
		uint32_t chunkGroupRowCount = chunkGroupRowCounts[chunkIndex];
		stripeSkipListRowCount += chunkGroupRowCount;
	}

	return stripeSkipListRowCount;
}

export bool *
ProjectedColumnMask(uint32_t columnCount, List *projectedColumnList)
{
	bool *projectedColumnMask = make_array<bool>(columnCount);

	for (auto attno : PIntList{projectedColumnList})
	{
		if (attno <= 0 || static_cast<uint32_t>(attno) > columnCount)
		{
			bioseq_log<ERR, PG>("projected column {} is outside column count {}",
				   attno, columnCount);
		}

		/* attno is 1-indexed; projectedColumnMask is 0-indexed */
		int columnIndex = attno - 1;
		projectedColumnMask[columnIndex] = true;
	}

	return projectedColumnMask;
}

/*-------------------------------------------------------------------------
 *
 * bioseq_planner_hook.c
 *
 * Copyright (c) Hydra, Inc.
 *
 * Modify top plan and change aggregate function to provided ones that can execute on 
 * column vector.
 *
 *-------------------------------------------------------------------------
 */

#include "common.h"

#include "columnar/columnar.h"
#include "columnar/columnar_customscan.h"
#include "columnar/columnar_indexscan.h"
#include "columnar/vectorization/columnar_vector_execution.h"
#include "columnar/vectorization/nodes/columnar_aggregator_node.h"


#include "common.h"
import mlogging;

static planner_hook_type PreviousPlannerHook = NULL;
static ProcessUtility_hook_type PreviousProcessUtilityHook = NULL;

static Oid bioseq_tableam_oid = InvalidOid;

static PlannedStmt * ColumnarPlannerHook(Query *parse,  const char *query_string,
										 int cursorOptions, ParamListInfo boundParams);
static bool IsCreateTableAs(const char *query);


static Plan * PlanTreeMutator(Plan *node, void *context);

typedef struct PlanTreeMutatorContext
{
	bool vectorizedAggregation;
} PlanTreeMutatorContext;

#define FLATCOPY(newnode, node, nodetype)  \
	( (newnode) = (nodetype *) palloc(sizeof(nodetype)), \
	  memcpy((newnode), (node), sizeof(nodetype)) )


static bool
bioseq_index_table(Oid indexOid, Oid columnarTableAmOid)
{
	BMethod(bioseq_index_table);
	HeapTuple ht_idx;
	Form_pg_index idxrec;
	HeapTuple ht_table;
	Form_pg_class tablerec;
	bool index_on_columnar = false;

	/*
	 * Fetch the pg_index tuple by the Oid of the index
	 */
	ht_idx = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexOid));
	idxrec = (Form_pg_index) GETSTRUCT(ht_idx);

	ht_table = SearchSysCache1(RELOID, ObjectIdGetDatum(idxrec->indrelid));
	tablerec = (Form_pg_class) GETSTRUCT(ht_table);

	index_on_columnar = tablerec->relam == columnarTableAmOid;

	ReleaseSysCache(ht_idx);
	ReleaseSysCache(ht_table);

	return index_on_columnar;
}

static Node *
AggRefArgsExpressionMutator(Node *node, void *context)
{
	BMethod(AggRefArgsExpressionMutator);
	if (node == NULL)
		return NULL;

	Node *previousNode = (Node *) context;

	if (IsA(node, OpExpr) || IsA(node, DistinctExpr) || IsA(node, NullIfExpr) )
	{
		OpExpr *opExprNode = (OpExpr *) node;

		Form_pg_operator operatorForm;
		HeapTuple operatorTuple;

		if (list_length(opExprNode->args) != 2)
			elog(ERROR, "Aggregation vectorizaion works only on two arguments.");

		if (CheckOpExprArgumentRules(opExprNode->args))
			elog(ERROR, "Unsupported aggregate argument combination.");

		operatorTuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(opExprNode->opno));
		operatorForm = (Form_pg_operator) GETSTRUCT(operatorTuple);
		Oid procedureOid = operatorForm->oprcode;
		ReleaseSysCache(operatorTuple);

		Oid vectorizedProcedureOid;
		if (!GetVectorizedProcedureOid(procedureOid, &vectorizedProcedureOid))
			elog(ERROR, "Vectorized aggregate not found.");

		opExprNode->opfuncid = vectorizedProcedureOid;

		return (Node *) opExprNode;
	}

	/* This should handle aggregates that have non var(column) as argument*/
	if (previousNode != NULL && IsA(previousNode, TargetEntry) && !IsA(node, Var))
		elog(ERROR, "Vectorized Aggregates accept only valid column argument");

	return expression_tree_mutator(node, AggRefArgsExpressionMutator, (void *) node);
}

static Node *
ExpressionMutator(Node *node, void *context)
{
	BMethod(ExpressionMutator);
	if (node == NULL)
		return NULL;

	if (IsA(node, Aggref))
	{
		Aggref *oldAggRefNode = (Aggref *) node;
		Aggref *newAggRefNode = copyObject(oldAggRefNode);

		if (oldAggRefNode->aggdistinct)
		{
			elog(ERROR, "Vectorized aggregate with DISTINCT not supported.");
		}

		if (oldAggRefNode->aggfilter)
		{
			elog(ERROR, "Vectorized aggregate with FILTER not supported");
		}

		newAggRefNode->args = (List *)
			expression_tree_mutator((Node *) oldAggRefNode->args, AggRefArgsExpressionMutator, NULL);
		
		Oid vectorizedProcedureOid = 0;
		if (!GetVectorizedProcedureOid(newAggRefNode->aggfnoid, &vectorizedProcedureOid))
		{
			elog(ERROR, "Vectorized aggregate not found.");
		}

		newAggRefNode->aggfnoid = vectorizedProcedureOid;

		return (Node *) newAggRefNode;
	}

	return expression_tree_mutator(node, ExpressionMutator, (void *) context);
}


static Plan *
PlanTreeMutator(Plan *node, void *context)
{
	if (node == NULL)
		return NULL;

	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();

	switch (nodeTag(node))
	{
		case T_CustomScan:
		{
			JustLog("Plan tree mutator encountered custom plan");
			CustomScan *customScan = (CustomScan *) node;

			if (customScan->methods == bioseq_customscan_methods())
			{
				PlanTreeMutatorContext *planTreeContext = (PlanTreeMutatorContext *) context;

				Const * vectorizedAggregateExecution = makeNode(Const);

				vectorizedAggregateExecution->constbyval = true;
				vectorizedAggregateExecution->consttype = CUSTOM_SCAN_VECTORIZED_AGGREGATE;
				vectorizedAggregateExecution->constvalue =  planTreeContext->vectorizedAggregation;
				vectorizedAggregateExecution->constlen = sizeof(bool);

				customScan->custom_private = lappend(customScan->custom_private, vectorizedAggregateExecution);
			}
			else
			{
				elog(ERROR, "Custom Scan type is not ColumnarScan.");
			}

			break;
		}

		case T_Agg:
		{
			JustLog("Plan tree mutator encountered aggregation");
			Agg *aggNode = (Agg *) node;
			Agg	*newAgg;
			CustomScan *vectorizedAggNode;

			if (!bioseq_enable_vectorization)
				return node;

			if (aggNode->plan.lefttree->type == T_CustomScan)
			{
				JustLog("With custom scan");
				if (aggNode->aggstrategy == AGG_PLAIN)
				{
					JustLog("Agg plain");
					vectorizedAggNode = bioseq_create_aggregator_node();

					FLATCOPY(newAgg, aggNode, Agg);

					newAgg->plan.targetlist = 
						(List *) expression_tree_mutator((Node *) newAgg->plan.targetlist, ExpressionMutator, NULL);


					vectorizedAggNode->custom_plans = 
						lappend(vectorizedAggNode->custom_plans, newAgg);
					vectorizedAggNode->scan.plan.targetlist = 
						CustomBuildTargetList(aggNode->plan.targetlist, INDEX_VAR);
					vectorizedAggNode->custom_scan_tlist = newAgg->plan.targetlist;

					// Parallel agg node
					Plan *vectorizedAggNodePlan = (Plan *) vectorizedAggNode;
					vectorizedAggNodePlan->parallel_aware = aggNode->plan.lefttree->parallel_aware;
					vectorizedAggNodePlan->startup_cost = aggNode->plan.startup_cost;
					vectorizedAggNodePlan->total_cost = aggNode->plan.total_cost;
					vectorizedAggNodePlan->plan_rows = aggNode->plan.plan_rows;
					vectorizedAggNodePlan->plan_width = aggNode->plan.plan_width;


					PlanTreeMutatorContext *planTreeContext = (PlanTreeMutatorContext *) context;
					planTreeContext->vectorizedAggregation = true;

					PlanTreeMutator(node->lefttree, context);
					PlanTreeMutator(node->righttree, context);

					vectorizedAggNode->scan.plan.lefttree = node->lefttree;
					vectorizedAggNode->scan.plan.righttree = node->righttree;

					return (Plan *) vectorizedAggNode;
				}

				return node;
			}

			break;
		}
		case T_IndexScan:
		{
			JustLog("BISC immediate exit check {}", bioseq_index_scan);
			if (!bioseq_index_scan)
				return node;

			IndexScan *indexScanNode = (IndexScan *) node;
			IndexScan *newIndexScan;
			CustomScan *columnarIndexScan;

			/* Check if index is build on columnar table */
			if (!bioseq_index_table(indexScanNode->indexid, bioseq_tableam_oid)) {
				JustLog("BISC no index");
				return node;
			}

			columnarIndexScan = bioseq_create_indexscan_node();
			FLATCOPY(newIndexScan, indexScanNode, IndexScan);

			columnarIndexScan->custom_plans = 
						lappend(columnarIndexScan->custom_plans, newIndexScan);
		
			columnarIndexScan->scan.plan.targetlist = 
						CustomBuildTargetList(indexScanNode->scan.plan.targetlist, INDEX_VAR);

			columnarIndexScan->custom_scan_tlist = newIndexScan->scan.plan.targetlist;

			Plan *columnarIndexScanPlan = (Plan *) columnarIndexScan;
			columnarIndexScanPlan->parallel_aware = indexScanNode->scan.plan.parallel_aware;
			columnarIndexScanPlan->startup_cost = indexScanNode->scan.plan.startup_cost;
			columnarIndexScanPlan->total_cost = indexScanNode->scan.plan.total_cost;
			columnarIndexScanPlan->plan_rows = indexScanNode->scan.plan.plan_rows;
			columnarIndexScanPlan->plan_width = indexScanNode->scan.plan.plan_width;

			JustLog("BISC Plan modified");
			return (Plan *) columnarIndexScan;
		}
		default:
		{
			break;
		}
	}

	node->lefttree = PlanTreeMutator(node->lefttree, context);
	node->righttree = PlanTreeMutator(node->righttree, context);

	return node;
}

// New hook
static void ProcessUtilityHook(
    PlannedStmt *pstmt,
    const char *queryString,
    bool readOnlyTree,
    ProcessUtilityContext context,
    ParamListInfo params,
    QueryEnvironment *queryEnv,
    DestReceiver *dest,
    QueryCompletion *qc)
{
  Node *parsetree = pstmt->utilityStmt;

  // Reject DDL if enabled

  if (pstmt->commandType == CMD_UTILITY && GetCommandLogLevel(parsetree) == LOGSTMT_DDL) {
	//JustLog("DDL QUERY DETECTED [{}]", pstmt);
	auto ut = pstmt->utilityStmt;
	if(IsA(ut, CreateStmt)) {
		//JustLog("GOT CREATE! :D");
		auto createStatement = (CreateStmt*)(ut);
		if (createStatement->accessMethod != nullptr) {
			std::string am(createStatement->accessMethod);
			//JustLog("am = '{}'", am);
			if (am == "columnar") {
				// Create table via access method
				JustLog("Create table via access method [{}]", nodeStr(createStatement));
				for(auto elt : PList<Node>(createStatement->tableElts)) {
					if (IsA(elt, ColumnDef)) {
						// column def
						auto colDef = (ColumnDef*)(elt);
						JustLog("found column '{}'", colDef->colname);
					}
				}
			}
		}
	}
  }

  // Call downstream hooks

  if (PreviousProcessUtilityHook) {
    PreviousProcessUtilityHook(
        pstmt, queryString, readOnlyTree, context, params,
        queryEnv, dest, qc
    );
  } else {
    standard_ProcessUtility(
        pstmt, queryString, readOnlyTree, context, params,
        queryEnv, dest, qc
    );
  }

} // noddl_ProcessUtility

static PlannedStmt *
ColumnarPlannerHook(Query *parse,
					const char *query_string,
					int cursorOptions,
					ParamListInfo boundParams)
{
	BMethod(ColumnarPlannerHook);
	PlannedStmt	*stmt;
	Plan *savedPlanTree;
	List *savedSubplan;
	MemoryContext saved_context;

	JustLog("Planner hook invoked: {}", nodeStr(parse));
	//JustLog("Query: [{}]", std::string(query_string));

	if (PreviousPlannerHook) {
		JustLog("Previous planner hook");
		stmt = PreviousPlannerHook(parse, query_string, cursorOptions, boundParams);
	} else {
		JustLog("standard_planner");
		stmt = standard_planner(parse, query_string, cursorOptions, boundParams);
	}
	JustLog("Done previous hook exec.");

	/*
	 * In the case of a CREATE TABLE AS query, we are not able to successfully
	 * drop out of a parallel insert situation.  This checks for a CMD_SELECT
	 * and in that case examines the query string to see if it matches the
	 * pattern of a CREATE TABLE AS.  If so, set the parallelism to 0 (off).
	 */
	JustLog("Check CREATE TABLE AS query");
	if (parse->commandType == CMD_SELECT)
	{
		JustLog("Sanity check query is null => {}", query_string == nullptr);
		if (query_string != nullptr && IsCreateTableAs(query_string))
		{
			stmt->parallelModeNeeded = 0;
		}
	}

	JustLog("Check vectorization");
	if (!(bioseq_enable_vectorization			/* Vectorization should be enabled */
			|| bioseq_index_scan)				/* or Columnar Index Scan */
		|| stmt->commandType != CMD_SELECT		/* only SELECTS are supported  */
		|| list_length(stmt->rtable) != 1)		/* JOINs are not yet supported */
		{
			JustLog("Plan fast return <-- vec={} iscan={} cmdtype={} rtable={}", bioseq_enable_vectorization, bioseq_index_scan, (stmt->commandType != CMD_SELECT), (list_length(stmt->rtable) != 1));
			return stmt;
		}

	if (bioseq_tableam_oid == InvalidOid)
		bioseq_tableam_oid = get_table_am_oid("columnar", true);

	savedPlanTree = stmt->planTree;
	savedSubplan = stmt->subplans;

	saved_context = CurrentMemoryContext;

	JustLog("Plan mutate");
	PG_TRY();
	{
		List		*subplans = NULL;
		ListCell	*cell;

		PlanTreeMutatorContext plainTreeContext;
		plainTreeContext.vectorizedAggregation = 0;

		stmt->planTree = (Plan *) PlanTreeMutator(stmt->planTree, (void *) &plainTreeContext);

		//foreach(cell, stmt->subplans)
		JustLog("Iterate subplans");
		for(auto plan : PList<Plan>(stmt->subplans))
		{
			JustLog("Iterate next");
			PlanTreeMutatorContext subPlainTreeContext;
			plainTreeContext.vectorizedAggregation = 0;
			Plan *subplan = static_cast<Plan*>(PlanTreeMutator(plan, (void *) &subPlainTreeContext));
			subplans = lappend(subplans, subplan);
		}
		JustLog("Iterate subplans END");

		stmt->subplans = subplans;
	}
	PG_CATCH();
	{
		ErrorData  *edata;
		MemoryContextSwitchTo(saved_context);

		edata = CopyErrorData();
		FlushErrorState();
		ereport(DEBUG1,
				(errcode(ERRCODE_INTERNAL_ERROR),
					errmsg("Query can't be vectorized. Falling back to original execution."),
					errdetail("%s", edata->message)));
		stmt->planTree = savedPlanTree;
		stmt->subplans = savedSubplan;
		JustLog("Vectorized catch-block completed");
	}

	PG_END_TRY();
	JustLog("Columnar planner hook exited");

	return stmt;
}

/*
 * IsCreateTableAs
 *
 * Searches a lower case copy of the query string using strstr to check
 * for the keywords CREATE, TABLE, and AS, in that order.  There can be
 * false positives, but we try to minimize them.
 */
static bool
IsCreateTableAs(const char *query)
{
	BMethod(IsCreateTableAs);
	char *c, *t, *a;
	size_t query_len = strlen(query);
	char *haystack = (char *) palloc(query_len + 1);
	int32 i;

	/* Create a lower case copy of the string. */
	for (i = 0; i < query_len; i++)
	{
		haystack[i] = tolower(query[i]);
	}

	haystack[i] = '\0';

	c = strstr(haystack, "create");
	if (c == NULL)
	{
		JustLog("IsCreateTableAs exit => false");
		pfree(haystack);
		return false;
	}

	t = strstr(c + 6, "table");
	if (t == NULL)
	{
		JustLog("IsCreateTableAs exit => false");
		pfree(haystack);
		return false;
	}

	a = strstr(t + 5, "as");
	if (a == NULL)
	{
		JustLog("IsCreateTableAs exit => false");
		pfree(haystack);
		return false;
	}

	pfree(haystack);

	JustLog("IsCreateTableAs exit => true");
	return true;
}

void bioseq_planner_init(void)
{
	BMethod(bioseq_planner_init);

	PreviousPlannerHook = planner_hook;
	planner_hook = ColumnarPlannerHook;

	PreviousProcessUtilityHook = ProcessUtility_hook;
	ProcessUtility_hook = ProcessUtilityHook;


	bioseq_register_aggregator_node();
	bioseq_register_indexscan_node();
}

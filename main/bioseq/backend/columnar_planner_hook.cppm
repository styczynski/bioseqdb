/*-------------------------------------------------------------------------
 *
 * columnar_planner_hook.cppm
 *
 * Top-level planner_hook and ProcessUtility_hook used to splice the
 * bioseq custom scan into query plans. Responsibilities:
 *   - Walk the planned tree and rewrite aggregation nodes to use the
 *     vectorised aggregator when the input is a bioseq columnar table.
 *   - Rewrite IndexScan nodes to the bioseq IndexScan custom node.
 *   - Detect CREATE TABLE AS queries and disable parallel insert so they
 *     work with the columnar writer.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include "columnar/columnar.h"
#include "columnar/vectorization/columnar_vector_execution.h"
#include "columnar/vectorization/nodes/columnar_aggregator_node.h"

export module bioseq_planner_hook;

import mlogging;
import bioseq_table;
import pg_utils;

extern "C++" {

static planner_hook_type PreviousPlannerHook = NULL;
static ProcessUtility_hook_type PreviousProcessUtilityHook = NULL;

static Oid bioseq_tableam_oid = InvalidOid;

static PlannedStmt* ColumnarPlannerHook(Query* parse, const char* query_string,
										int cursor_options, ParamListInfo bound_params);
static bool IsCreateTableAs(const char* query);


static Plan* PlanTreeMutator(Plan* node, void* context);

struct PlanTreeMutatorContext
{
	bool vectorizedAggregation;
};

/*
 * FlatCopyPlan clones the body of a plan node of the given concrete type.
 * Replaces the classic FLATCOPY token-pasting macro with a type-checked
 * template helper so callers read like plain C++.
 */
template<typename PlanNode>
[[nodiscard]] inline PlanNode* FlatCopyPlan(const PlanNode* source) {
	PlanNode* cloned = static_cast<PlanNode*>(palloc(sizeof(PlanNode)));
	memcpy(cloned, source, sizeof(PlanNode));
	return cloned;
}


/*
 * bioseq_index_table returns true when the given index is defined on a
 * table that uses the bioseq columnar access method. Used during planning
 * to decide whether an IndexScan should be wrapped in the bioseq custom
 * scan.
 */
static bool
bioseq_index_table(Oid index_oid, Oid columnar_tableam_oid)
{
	JustLog("[bioseq] pg: bioseq_index_table() index_oid={} columnar_tableam_oid={}", index_oid, columnar_tableam_oid);

	HeapTuple index_tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(index_oid));
	if (!HeapTupleIsValid(index_tuple))
	{
		JustLog("[bioseq] pg: bioseq_index_table() index syscache miss for oid={}", index_oid);
		return false;
	}
	Form_pg_index index_record = reinterpret_cast<Form_pg_index>(GETSTRUCT(index_tuple));

	HeapTuple table_tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(index_record->indrelid));
	if (!HeapTupleIsValid(table_tuple))
	{
		ReleaseSysCache(index_tuple);
		JustLog("[bioseq] pg: bioseq_index_table() table syscache miss for indrelid={}", index_record->indrelid);
		return false;
	}
	Form_pg_class table_record = reinterpret_cast<Form_pg_class>(GETSTRUCT(table_tuple));

	const bool index_on_columnar = table_record->relam == columnar_tableam_oid;

	ReleaseSysCache(index_tuple);
	ReleaseSysCache(table_tuple);

	return index_on_columnar;
}

/*
 * AggRefArgsExpressionMutator walks the argument expressions of a single
 * Aggref node. OpExpr/DistinctExpr/NullIfExpr nodes are rewritten so that
 * opfuncid points at the vectorised implementation; anything else must be a
 * plain Var or the mutation is rejected.
 */
static Node*
AggRefArgsExpressionMutator(Node* node, void* context)
{
	JustLog("[bioseq] pg: AggRefArgsExpressionMutator() entered");
	if (node == NULL)
		return NULL;

	Node* previous_node = static_cast<Node*>(context);

	if (IsA(node, OpExpr) || IsA(node, DistinctExpr) || IsA(node, NullIfExpr))
	{
		OpExpr* op_expr_node = reinterpret_cast<OpExpr*>(node);

		if (list_length(op_expr_node->args) != 2)
			elog(ERROR, "Aggregation vectorization works only on two arguments.");

		if (CheckOpExprArgumentRules(op_expr_node->args))
			elog(ERROR, "Unsupported aggregate argument combination.");

		HeapTuple operator_tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(op_expr_node->opno));
		Form_pg_operator operator_form = reinterpret_cast<Form_pg_operator>(GETSTRUCT(operator_tuple));
		Oid procedure_oid = operator_form->oprcode;
		ReleaseSysCache(operator_tuple);

		Oid vectorized_procedure_oid;
		if (!GetVectorizedProcedureOid(procedure_oid, &vectorized_procedure_oid))
			elog(ERROR, "Vectorized aggregate not found.");

		op_expr_node->opfuncid = vectorized_procedure_oid;

		return reinterpret_cast<Node*>(op_expr_node);
	}

	/* This should handle aggregates that have non var(column) as argument*/
	if (previous_node != NULL && IsA(previous_node, TargetEntry) && !IsA(node, Var))
		elog(ERROR, "Vectorized Aggregates accept only valid column argument");

	return expression_tree_mutator(node, AggRefArgsExpressionMutator, static_cast<void*>(node));
}

/*
 * ExpressionMutator runs over a target-list / qual expression tree and
 * rewrites Aggref nodes so their aggfnoid points at the vectorised variant.
 * DISTINCT / FILTER are unsupported and raise an error up-front.
 */
static Node*
ExpressionMutator(Node* node, void* context)
{
	JustLog("[bioseq] pg: ExpressionMutator() entered");
	if (node == NULL)
		return NULL;

	if (IsA(node, Aggref))
	{
		Aggref* old_aggref_node = reinterpret_cast<Aggref*>(node);
		Aggref* new_aggref_node = reinterpret_cast<Aggref*>(copyObject(old_aggref_node));

		if (old_aggref_node->aggdistinct)
		{
			elog(ERROR, "Vectorized aggregate with DISTINCT not supported.");
		}

		if (old_aggref_node->aggfilter)
		{
			elog(ERROR, "Vectorized aggregate with FILTER not supported");
		}

		new_aggref_node->args = reinterpret_cast<List*>(
			expression_tree_mutator(reinterpret_cast<Node*>(old_aggref_node->args), AggRefArgsExpressionMutator, NULL));

		Oid vectorized_procedure_oid = 0;
		if (!GetVectorizedProcedureOid(new_aggref_node->aggfnoid, &vectorized_procedure_oid))
		{
			elog(ERROR, "Vectorized aggregate not found.");
		}

		new_aggref_node->aggfnoid = vectorized_procedure_oid;

		return reinterpret_cast<Node*>(new_aggref_node);
	}

	return expression_tree_mutator(node, ExpressionMutator, context);
}


/*
 * PlanTreeMutator recursively rewrites the plan tree. CustomScan nodes get
 * a vectorization flag appended; Agg nodes get replaced with the bioseq
 * aggregator custom node; IndexScan nodes on columnar relations get wrapped
 * in the bioseq IndexScan custom node.
 */
static Plan*
PlanTreeMutator(Plan* node, void* context)
{
	if (node == NULL)
		return NULL;

	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();

	switch (nodeTag(node))
	{
		case T_CustomScan:
		{
			JustLog("[bioseq] pg: PlanTreeMutator() encountered CustomScan");
			CustomScan* custom_scan = reinterpret_cast<CustomScan*>(node);

			if (custom_scan->methods == bioseq_customscan_methods())
			{
				PlanTreeMutatorContext* plan_tree_context = static_cast<PlanTreeMutatorContext*>(context);

				Const* vectorized_aggregate_execution = makeNode(Const);

				vectorized_aggregate_execution->constbyval = true;
				vectorized_aggregate_execution->consttype = CUSTOM_SCAN_VECTORIZED_AGGREGATE;
				vectorized_aggregate_execution->constvalue = BoolGetDatum(plan_tree_context->vectorizedAggregation);
				vectorized_aggregate_execution->constlen = sizeof(bool);

				custom_scan->custom_private = lappend(custom_scan->custom_private, vectorized_aggregate_execution);
			}
			else
			{
				elog(ERROR, "Custom Scan type is not ColumnarScan.");
			}

			break;
		}

		case T_Agg:
		{
			JustLog("[bioseq] pg: PlanTreeMutator() encountered Agg");
			Agg* agg_node = reinterpret_cast<Agg*>(node);

			if (!bioseq_enable_vectorization)
				return node;

			if (agg_node->plan.lefttree->type == T_CustomScan)
			{
				JustLog("[bioseq] pg: PlanTreeMutator() Agg has CustomScan child");
				if (agg_node->aggstrategy == AGG_PLAIN)
				{
					JustLog("[bioseq] pg: PlanTreeMutator() rewriting AGG_PLAIN into vectorized aggregator");
					CustomScan* vectorized_agg_node = bioseq_create_aggregator_node();

					Agg* new_agg = FlatCopyPlan(agg_node);

					new_agg->plan.targetlist =
						reinterpret_cast<List*>(expression_tree_mutator(reinterpret_cast<Node*>(new_agg->plan.targetlist), ExpressionMutator, NULL));


					vectorized_agg_node->custom_plans =
						lappend(vectorized_agg_node->custom_plans, new_agg);
					vectorized_agg_node->scan.plan.targetlist =
						CustomBuildTargetList(agg_node->plan.targetlist, INDEX_VAR);
					vectorized_agg_node->custom_scan_tlist = new_agg->plan.targetlist;

					Plan* vectorized_agg_plan = reinterpret_cast<Plan*>(vectorized_agg_node);
					CopyPlanCosts(vectorized_agg_plan, &agg_node->plan);
					vectorized_agg_plan->parallel_aware = agg_node->plan.lefttree->parallel_aware;


					PlanTreeMutatorContext* plan_tree_context = static_cast<PlanTreeMutatorContext*>(context);
					plan_tree_context->vectorizedAggregation = true;

					/*
					 * PlanTreeMutator is a *replacing* mutator: when it
					 * encounters an IndexScan or CustomScan it returns a
					 * new node that needs to take the place of the input.
					 * We therefore must reassign lefttree / righttree
					 * with the result so any nested rewrites land in the
					 * vectorized aggregator's children.
					 */
					node->lefttree = PlanTreeMutator(node->lefttree, context);
					node->righttree = PlanTreeMutator(node->righttree, context);

					vectorized_agg_node->scan.plan.lefttree = node->lefttree;
					vectorized_agg_node->scan.plan.righttree = node->righttree;

					return reinterpret_cast<Plan*>(vectorized_agg_node);
				}

				return node;
			}

			break;
		}
		case T_IndexScan:
		{
			JustLog("[bioseq] pg: PlanTreeMutator() encountered IndexScan; bioseq_index_scan={}", bioseq_index_scan);
			if (!bioseq_index_scan)
				return node;

			IndexScan* index_scan_node = reinterpret_cast<IndexScan*>(node);

			if (!bioseq_index_table(index_scan_node->indexid, bioseq_tableam_oid)) {
				JustLog("[bioseq] pg: PlanTreeMutator() IndexScan not on columnar table; skipping rewrite");
				return node;
			}

			CustomScan* columnar_index_scan = bioseq_create_indexscan_node();
			IndexScan* new_index_scan = FlatCopyPlan(index_scan_node);

			columnar_index_scan->custom_plans =
						lappend(columnar_index_scan->custom_plans, new_index_scan);

			columnar_index_scan->scan.plan.targetlist =
						CustomBuildTargetList(index_scan_node->scan.plan.targetlist, INDEX_VAR);

			columnar_index_scan->custom_scan_tlist = new_index_scan->scan.plan.targetlist;

			Plan* columnar_index_scan_plan = reinterpret_cast<Plan*>(columnar_index_scan);
			CopyPlanCosts(columnar_index_scan_plan, &index_scan_node->scan.plan);

			JustLog("[bioseq] pg: PlanTreeMutator() IndexScan rewritten to bioseq custom scan");
			return reinterpret_cast<Plan*>(columnar_index_scan);
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

/*
 * ProcessUtilityHook watches DDL statements for CREATE TABLE ... USING
 * columnar so that logging can surface column definitions for diagnostics.
 * Downstream behaviour is delegated to the previous hook (or the standard
 * handler) regardless of logging outcome.
 */
static void ProcessUtilityHook(
	PlannedStmt* pstmt,
	const char* query_string,
	bool read_only_tree,
	ProcessUtilityContext utility_context,
	ParamListInfo params,
	QueryEnvironment* query_env,
	DestReceiver* dest,
	QueryCompletion* completion)
{
	Node* parse_tree = pstmt->utilityStmt;

	if (pstmt->commandType == CMD_UTILITY && GetCommandLogLevel(parse_tree) == LOGSTMT_DDL) {
		Node* utility_statement = pstmt->utilityStmt;
		if (IsA(utility_statement, CreateStmt)) {
			CreateStmt* create_statement = reinterpret_cast<CreateStmt*>(utility_statement);
			if (create_statement->accessMethod != nullptr) {
				std::string access_method(create_statement->accessMethod);
				if (access_method == "columnar") {
					JustLog("[bioseq] pg: ProcessUtilityHook() CREATE TABLE USING columnar [{}]", nodeStr(create_statement));
					for (auto element : PList<Node>(create_statement->tableElts)) {
						if (IsA(element, ColumnDef)) {
							ColumnDef* column_def = reinterpret_cast<ColumnDef*>(element);
							JustLog("[bioseq] pg: ProcessUtilityHook() column '{}'", column_def->colname);
						}
					}
				}
			}
		}
	}

	if (PreviousProcessUtilityHook) {
		PreviousProcessUtilityHook(
			pstmt, query_string, read_only_tree, utility_context, params,
			query_env, dest, completion);
	} else {
		standard_ProcessUtility(
			pstmt, query_string, read_only_tree, utility_context, params,
			query_env, dest, completion);
	}
}

/*
 * ColumnarPlannerHook is invoked once per SELECT plan. It delegates to the
 * previous hook (or standard_planner) and then runs the PlanTreeMutator
 * over the result to splice in the vectorised aggregator / IndexScan. If
 * the mutation raises an error we revert to the saved plan tree.
 */
static PlannedStmt*
ColumnarPlannerHook(Query* parse,
					const char* query_string,
					int cursor_options,
					ParamListInfo bound_params)
{
	JustLog("[bioseq] pg: ColumnarPlannerHook() entered; parse_tree=[{}]", nodeStr(parse));

	PlannedStmt* stmt;
	if (PreviousPlannerHook) {
		JustLog("[bioseq] pg: ColumnarPlannerHook() delegating to previous planner hook");
		stmt = PreviousPlannerHook(parse, query_string, cursor_options, bound_params);
	} else {
		JustLog("[bioseq] pg: ColumnarPlannerHook() delegating to standard_planner");
		stmt = standard_planner(parse, query_string, cursor_options, bound_params);
	}
	JustLog("[bioseq] pg: ColumnarPlannerHook() upstream planner returned");

	/*
	 * In the case of a CREATE TABLE AS query, we are not able to successfully
	 * drop out of a parallel insert situation. Disable parallelism for those.
	 */
	JustLog("[bioseq] pg: ColumnarPlannerHook() checking for CREATE TABLE AS");
	if (parse->commandType == CMD_SELECT)
	{
		JustLog("[bioseq] pg: ColumnarPlannerHook() query_string_null={}", query_string == nullptr);
		if (query_string != nullptr && IsCreateTableAs(query_string))
		{
			stmt->parallelModeNeeded = false;
		}
	}

	JustLog("[bioseq] pg: ColumnarPlannerHook() checking vectorization preconditions");
	if (!(bioseq_enable_vectorization			/* Vectorization should be enabled */
			|| bioseq_index_scan)				/* or Columnar Index Scan */
		|| stmt->commandType != CMD_SELECT		/* only SELECTS are supported  */
		|| list_length(stmt->rtable) != 1)		/* JOINs are not yet supported */
		{
			JustLog("[bioseq] pg: ColumnarPlannerHook() fast-return; vec={} idx={} cmd_not_select={} rtable_not_one={}",
				bioseq_enable_vectorization, bioseq_index_scan,
				(stmt->commandType != CMD_SELECT), (list_length(stmt->rtable) != 1));
			return stmt;
		}

	if (bioseq_tableam_oid == InvalidOid)
		bioseq_tableam_oid = get_table_am_oid("columnar", true);

	Plan* saved_plan_tree = stmt->planTree;
	List* saved_subplans = stmt->subplans;

	MemoryContext saved_context = CurrentMemoryContext;

	JustLog("[bioseq] pg: ColumnarPlannerHook() running plan mutator");
	PG_TRY();
	{
		List* mutated_subplans = NULL;

		PlanTreeMutatorContext plan_tree_context;
		plan_tree_context.vectorizedAggregation = false;

		stmt->planTree = reinterpret_cast<Plan*>(PlanTreeMutator(stmt->planTree, static_cast<void*>(&plan_tree_context)));

		JustLog("[bioseq] pg: ColumnarPlannerHook() iterating subplans");
		for (auto subplan_node : PList<Plan>(stmt->subplans))
		{
			JustLog("[bioseq] pg: ColumnarPlannerHook() processing next subplan");
			PlanTreeMutatorContext sub_plan_tree_context;
			sub_plan_tree_context.vectorizedAggregation = false;
			Plan* mutated_subplan = static_cast<Plan*>(PlanTreeMutator(subplan_node, static_cast<void*>(&sub_plan_tree_context)));
			mutated_subplans = lappend(mutated_subplans, mutated_subplan);
		}
		JustLog("[bioseq] pg: ColumnarPlannerHook() subplan iteration complete");

		stmt->subplans = mutated_subplans;
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(saved_context);

		ErrorData* error_data = CopyErrorData();
		FlushErrorState();
		ereport(DEBUG1,
				(errcode(ERRCODE_INTERNAL_ERROR),
					errmsg("Query can't be vectorized. Falling back to original execution."),
					errdetail("%s", error_data->message)));
		stmt->planTree = saved_plan_tree;
		stmt->subplans = saved_subplans;
		JustLog("[bioseq] pg: ColumnarPlannerHook() caught PG_TRY exception, reverted plan");
	}

	PG_END_TRY();
	JustLog("[bioseq] pg: ColumnarPlannerHook() returning");

	return stmt;
}

/*
 * IsCreateTableAs returns true if the query string (case-insensitively)
 * contains the keywords CREATE, TABLE, AS in order. This is a textual
 * heuristic; false positives are possible but minimised by requiring the
 * three tokens to appear sequentially.
 */
static bool
IsCreateTableAs(const char* query)
{
	JustLog("[bioseq] pg: IsCreateTableAs() entered");
	const size_t query_length = strlen(query);
	char* haystack = static_cast<char*>(palloc(query_length + 1));

	for (size_t character_index = 0; character_index < query_length; character_index++)
	{
		haystack[character_index] = tolower(query[character_index]);
	}
	haystack[query_length] = '\0';

	char* create_ptr = strstr(haystack, "create");
	if (create_ptr == NULL)
	{
		JustLog("[bioseq] pg: IsCreateTableAs() returning false; 'create' not found");
		pfree(haystack);
		return false;
	}

	char* table_ptr = strstr(create_ptr + 6, "table");
	if (table_ptr == NULL)
	{
		JustLog("[bioseq] pg: IsCreateTableAs() returning false; 'table' not found");
		pfree(haystack);
		return false;
	}

	char* as_ptr = strstr(table_ptr + 5, "as");
	if (as_ptr == NULL)
	{
		JustLog("[bioseq] pg: IsCreateTableAs() returning false; 'as' not found");
		pfree(haystack);
		return false;
	}

	pfree(haystack);

	JustLog("[bioseq] pg: IsCreateTableAs() returning true");
	return true;
}

/*
 * bioseq_planner_init installs the bioseq planner_hook and
 * ProcessUtility_hook, saving the previous hooks so they can be chained.
 * Also registers the custom-scan node types used by the plan rewriter.
 */
void bioseq_planner_init()
{
	JustLog("[bioseq] pg: bioseq_planner_init() entered");

	PreviousPlannerHook = planner_hook;
	planner_hook = ColumnarPlannerHook;

	PreviousProcessUtilityHook = ProcessUtility_hook;
	ProcessUtility_hook = ProcessUtilityHook;

	bioseq_register_aggregator_node();
	bioseq_register_indexscan_node();
}

}

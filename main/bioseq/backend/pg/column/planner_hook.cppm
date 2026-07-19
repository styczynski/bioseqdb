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


export module bioseq.column.planner_hook;

import bioseq.common.mlogging;
import bioseq.table;                      // custom/index scan planner API
import bioseq.pg_utils;
import bioseq.config;                    // DOC0322 §9.3 — bioseq_enable_vectorization, bioseq_index_scan GUCs
import bioseq.vector.execution;   // GetVectorizedProcedureOid
import bioseq.vector.aggregator_node;    // bioseq_create_aggregator_node, bioseq_register_aggregator_node
import bioseq.pg_utils.nodes;            // DOC0333 — node views, expression+plan mutator adapters, op-expr-family helper

using namespace bioseq::pg_utils;        // DOC0333 — node view/mutator helpers live in bioseq::pg_utils

/*
 * DOC0322 §7.7/§9.4 — the file-body `extern "C++"` wrapper is dropped.
 * bioseq_planner_init is `export`ed (so columnar.h's extern can be removed);
 * all the file-static hook state and helper functions below stay
 * module-internal (internal linkage / not exported).
 */

static planner_hook_type PreviousPlannerHook = NULL;
static ProcessUtility_hook_type PreviousProcessUtilityHook = NULL;

static Oid bioseq_tableam_oid = InvalidOid;

static PlannedStmt* BioseqColumnPlannerHook(Query* parse, const char* query_string,
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
bioseq_index_table(Oid index_oid, Oid bioseq_column_tableam_oid)
{
	bioseq_log<DBG>("[bioseq] pg: bioseq_index_table() index_oid={} bioseq_column_tableam_oid={}", index_oid, bioseq_column_tableam_oid);

	HeapTuple index_tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(index_oid));
	if (!HeapTupleIsValid(index_tuple))
	{
		bioseq_log<DBG>("[bioseq] pg: bioseq_index_table() index syscache miss for oid={}", index_oid);
		return false;
	}
	Form_pg_index index_record = reinterpret_cast<Form_pg_index>(GETSTRUCT(index_tuple));

	HeapTuple table_tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(index_record->indrelid));
	if (!HeapTupleIsValid(table_tuple))
	{
		ReleaseSysCache(index_tuple);
		bioseq_log<DBG>("[bioseq] pg: bioseq_index_table() table syscache miss for indrelid={}", index_record->indrelid);
		return false;
	}
	Form_pg_class table_record = reinterpret_cast<Form_pg_class>(GETSTRUCT(table_tuple));

	const bool index_on_bioseq_column = table_record->relam == bioseq_column_tableam_oid;

	ReleaseSysCache(index_tuple);
	ReleaseSysCache(table_tuple);

	return index_on_bioseq_column;
}

/*
 * aggref_args_mutator rewrites the argument expressions of a single Aggref.
 * OpExpr/DistinctExpr/NullIfExpr nodes (grouped via
 * pg_node_try_as_op_expr_family) get opfuncid pointed at the vectorised
 * implementation; any other expression sitting directly under an Aggref-arg
 * TargetEntry must be a plain Var or the mutation is rejected. The parent
 * node is threaded through aggref_args_state so the TargetEntry-parent check
 * still fires exactly where it did when the parent was the walker context.
 */
struct aggref_args_state {
	Node* parent = nullptr;
};

struct aggref_args_mutator {
	[[nodiscard]] pg_node_mutation
	operator()(pg_expression_mutation_cursor cursor, pg_node_view node, aggref_args_state& state) const {
		if (OpExpr* op_expr_node = pg_node_try_as_op_expr_family(node.raw()))
		{
			if (list_length(op_expr_node->args) != 2)
				bioseq_log<ERR, PG>( "Aggregation vectorization works only on two arguments.");

			if (!pg_is_const_var_binary_args(op_expr_node->args))
				bioseq_log<ERR, PG>( "Unsupported aggregate argument combination.");

			HeapTuple operator_tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(op_expr_node->opno));
			Form_pg_operator operator_form = reinterpret_cast<Form_pg_operator>(GETSTRUCT(operator_tuple));
			Oid procedure_oid = operator_form->oprcode;
			ReleaseSysCache(operator_tuple);

			Oid vectorized_procedure_oid;
			if (!GetVectorizedProcedureOid(procedure_oid, &vectorized_procedure_oid))
				bioseq_log<ERR, PG>( "Vectorized aggregate not found.");

			op_expr_node->opfuncid = vectorized_procedure_oid;

			return pg_node_mutation::with(pg_as_node(op_expr_node));
		}

		/* This should handle aggregates that have non var(column) as argument*/
		if (state.parent != NULL && pg_node_view{state.parent}.is<TargetEntry>() && !node.is<Var>())
			bioseq_log<ERR, PG>( "Vectorized Aggregates accept only valid column argument");

		Node* saved_parent = state.parent;
		state.parent = node.raw();
		Node* rewritten = cursor.descend(node.raw());
		state.parent = saved_parent;
		return pg_node_mutation::with(rewritten);
	}
};

/*
 * expr_mutator runs over a target-list / qual expression tree and rewrites
 * Aggref nodes so their aggfnoid points at the vectorised variant. DISTINCT
 * / FILTER are unsupported and raise an error up-front. The Aggref is deep
 * copied (pg_copy_node) before its args are rewritten via aggref_args_mutator.
 */
struct expr_mutator_state {};

struct expr_mutator {
	[[nodiscard]] pg_node_mutation
	operator()(pg_expression_mutation_cursor, pg_node_view node, expr_mutator_state&) const {
		if (Aggref* old_aggref_node = node.try_as<Aggref>())
		{
			Aggref* new_aggref_node = pg_copy_node(old_aggref_node);

			if (old_aggref_node->aggdistinct)
			{
				bioseq_log<ERR, PG>( "Vectorized aggregate with DISTINCT not supported.");
			}

			if (old_aggref_node->aggfilter)
			{
				bioseq_log<ERR, PG>( "Vectorized aggregate with FILTER not supported");
			}

			aggref_args_state args_state;
			aggref_args_mutator args_mutator;
			new_aggref_node->args = reinterpret_cast<List*>(
				pg_mutate_expression(pg_as_node(old_aggref_node->args), args_state, args_mutator));

			Oid vectorized_procedure_oid = 0;
			if (!GetVectorizedProcedureOid(new_aggref_node->aggfnoid, &vectorized_procedure_oid))
			{
				bioseq_log<ERR, PG>( "Vectorized aggregate not found.");
			}

			new_aggref_node->aggfnoid = vectorized_procedure_oid;

			return pg_node_mutation::with(pg_as_node(new_aggref_node));
		}

		return pg_node_mutation::descend();
	}
};


/*
 * plan_tree_mutator rewrites the plan tree (driven by pg_mutate_plan_tree).
 * CustomScan nodes get a vectorization flag appended; Agg nodes get replaced
 * with the bioseq aggregator custom node; IndexScan nodes on columnar
 * relations get wrapped in the bioseq IndexScan custom node. The adapter
 * calls check_stack_depth() per node and reassigns lefttree/righttree, so
 * the replacement contract lives in the returned pg_plan_mutation rather
 * than in ad hoc child recursion.
 */
struct plan_tree_mutator {
	[[nodiscard]] pg_plan_mutation
	operator()(pg_plan_mutation_cursor cursor, pg_node_view node, PlanTreeMutatorContext& plan_tree_context) const {
		if (CustomScan* custom_scan = node.try_as<CustomScan>())
		{
			bioseq_log<DBG>("[bioseq] pg: plan_tree_mutator() encountered CustomScan");

			const char* custom_name = custom_scan->methods == nullptr
				? nullptr : custom_scan->methods->CustomName;
			const bool is_bioseq_scan =
				custom_scan->methods == bioseq_customscan_methods()
				|| (custom_name != nullptr
					&& strcmp(custom_name, "BioseqScan") == 0);
			if (is_bioseq_scan)
			{
				Const* vectorized_aggregate_execution = makeNode(Const);

				vectorized_aggregate_execution->constbyval = true;
				vectorized_aggregate_execution->consttype = CUSTOM_SCAN_VECTORIZED_AGGREGATE;
				vectorized_aggregate_execution->constvalue = BoolGetDatum(plan_tree_context.vectorizedAggregation);
				vectorized_aggregate_execution->constlen = sizeof(bool);

				custom_scan->custom_private = lappend(custom_scan->custom_private, vectorized_aggregate_execution);
			}
			else if (custom_name != nullptr
				&& (strcmp(custom_name, "BioseqBatchJoin") == 0
					|| strcmp(custom_name, "BioseqBatchSortScan") == 0
					|| strcmp(custom_name, "BioseqSliceProject") == 0
					|| strcmp(custom_name, "BioseqIndexScan") == 0
					|| strcmp(custom_name, "BioseqExternalScan") == 0
					|| strcmp(custom_name, "VectorAggNode") == 0))
			{
				bioseq_log<DBG>("[bioseq] pg: plan_tree_mutator() traversing Bioseq custom scan {}", custom_name);
			}

			return pg_plan_mutation::descend();
		}

		if (Agg* agg_node = node.try_as<Agg>())
		{
			bioseq_log<DBG>("[bioseq] pg: plan_tree_mutator() encountered Agg");

			if (!bioseq_enable_vectorization)
				return pg_plan_mutation::descend();

			if (agg_node->plan.lefttree->type == T_CustomScan)
			{
				bioseq_log<DBG>("[bioseq] pg: plan_tree_mutator() Agg has CustomScan child");
				/*
				 * Skip the vectorised-aggregator rewrite when the Agg
				 * pulls a value from a bioseq.<projection>(...) call or
				 * an mmsearch FuncExpr. Those go through the BioseqScan's
				 * placeholder-Const rewrite at execution time, which
				 * yields a regular tuple slot — not the vector-typed
				 * slot the vectorised aggregator expects. Without this
				 * guard the vectorised aggregator engages, dereferences
				 * the regular slot as if it were a VectorTupleTableSlot,
				 * and crashes with a SIGSEGV (observed for
				 * `SELECT MIN(bioseq.target_start(a)) FROM ... WHERE
				 *  mmsearch(seq, '...')`).
				 */
				const bool agg_input_uses_bioseq_projection =
					bioseq_expr_uses_bioseq_projection(
						reinterpret_cast<Node*>(agg_node->plan.targetlist))
					|| bioseq_expr_uses_bioseq_projection(
						reinterpret_cast<Node*>(agg_node->plan.qual))
					|| bioseq_expr_uses_bioseq_projection(
						reinterpret_cast<Node*>(agg_node->plan.lefttree->targetlist))
					|| bioseq_expr_uses_bioseq_projection(
						reinterpret_cast<Node*>(agg_node->plan.lefttree->qual));
				if (agg_input_uses_bioseq_projection) {
					bioseq_log<DBG>("[bioseq] pg: plan_tree_mutator() Agg input depends on bioseq projection / mmsearch — skipping vectorised aggregator rewrite");
					return pg_plan_mutation::descend();
				}
				if (agg_node->aggstrategy == AGG_PLAIN)
				{
					bioseq_log<DBG>("[bioseq] pg: plan_tree_mutator() rewriting AGG_PLAIN into vectorized aggregator");
					CustomScan* vectorized_agg_node = bioseq_create_aggregator_node();

					Agg* new_agg = FlatCopyPlan(agg_node);

					expr_mutator_state targetlist_state;
					expr_mutator targetlist_mutator;
					new_agg->plan.targetlist =
						reinterpret_cast<List*>(pg_mutate_expression(reinterpret_cast<Node*>(new_agg->plan.targetlist), targetlist_state, targetlist_mutator));


					vectorized_agg_node->custom_plans =
						lappend(vectorized_agg_node->custom_plans, new_agg);
					vectorized_agg_node->scan.plan.targetlist =
						CustomBuildTargetList(agg_node->plan.targetlist, INDEX_VAR);
					vectorized_agg_node->custom_scan_tlist = new_agg->plan.targetlist;

					Plan* vectorized_agg_plan = reinterpret_cast<Plan*>(vectorized_agg_node);
					CopyPlanCosts(vectorized_agg_plan, &agg_node->plan);
					vectorized_agg_plan->parallel_aware = agg_node->plan.lefttree->parallel_aware;


					plan_tree_context.vectorizedAggregation = true;

					/*
					 * plan_tree_mutator is a *replacing* mutator: when it
					 * encounters an IndexScan or CustomScan it returns a
					 * new node that takes the place of the input. The
					 * cursor reassigns lefttree / righttree on the original
					 * Agg before we graft them onto the vectorized
					 * aggregator, so any nested rewrites land in its
					 * children. (&agg_node->plan aliases the input Plan.)
					 */
					Plan* recursed_agg_plan = cursor.descend_children(&agg_node->plan);

					vectorized_agg_node->scan.plan.lefttree = recursed_agg_plan->lefttree;
					vectorized_agg_node->scan.plan.righttree = recursed_agg_plan->righttree;

					return pg_plan_mutation::with(reinterpret_cast<Plan*>(vectorized_agg_node));
				}

				return pg_plan_mutation::descend();
			}

			return pg_plan_mutation::descend();
		}

		if (IndexScan* index_scan_node = node.try_as<IndexScan>())
		{
			bioseq_log<DBG>("[bioseq] pg: plan_tree_mutator() encountered IndexScan; bioseq_index_scan={}", bioseq_index_scan);
			if (!bioseq_index_scan)
				return pg_plan_mutation::skip();

			if (!bioseq_index_table(index_scan_node->indexid, bioseq_tableam_oid)) {
				bioseq_log<DBG>("[bioseq] pg: plan_tree_mutator() IndexScan not on columnar table; skipping rewrite");
				return pg_plan_mutation::skip();
			}

			CustomScan* bioseq_column_index_scan = bioseq_create_indexscan_node();
			IndexScan* new_index_scan = FlatCopyPlan(index_scan_node);

			bioseq_column_index_scan->custom_plans =
						lappend(bioseq_column_index_scan->custom_plans, new_index_scan);

			bioseq_column_index_scan->scan.plan.targetlist =
						CustomBuildTargetList(index_scan_node->scan.plan.targetlist, INDEX_VAR);

			bioseq_column_index_scan->custom_scan_tlist = new_index_scan->scan.plan.targetlist;

			Plan* bioseq_column_index_scan_plan = reinterpret_cast<Plan*>(bioseq_column_index_scan);
			CopyPlanCosts(bioseq_column_index_scan_plan, &index_scan_node->scan.plan);

			bioseq_log<DBG>("[bioseq] pg: plan_tree_mutator() IndexScan rewritten to bioseq custom scan");
			return pg_plan_mutation::with(reinterpret_cast<Plan*>(bioseq_column_index_scan));
		}

		return pg_plan_mutation::descend();
	}
};

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
		if (CreateStmt* create_statement = pg_node_try_as<CreateStmt>(utility_statement)) {
			if (create_statement->accessMethod != nullptr) {
				std::string access_method(create_statement->accessMethod);
				if (access_method == "bioseq") {
					bioseq_log<DBG>("[bioseq] pg: ProcessUtilityHook() CREATE TABLE USING columnar");
					for (auto element : PList<Node>(create_statement->tableElts)) {
						if (ColumnDef* column_def = pg_node_try_as<ColumnDef>(element)) {
							bioseq_log<DBG>("[bioseq] pg: ProcessUtilityHook() column '{}'", column_def->colname);
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
 * BioseqColumnPlannerHook is invoked once per SELECT plan. It delegates to the
 * previous hook (or standard_planner) and then runs the PlanTreeMutator
 * over the result to splice in the vectorised aggregator / IndexScan. If
 * the mutation raises an error we revert to the saved plan tree.
 */
static PlannedStmt*
BioseqColumnPlannerHook(Query* parse,
					const char* query_string,
					int cursor_options,
					ParamListInfo bound_params)
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnPlannerHook() entered");

	PlannedStmt* stmt;
	if (PreviousPlannerHook) {
		bioseq_log<DBG>("[bioseq] pg: BioseqColumnPlannerHook() delegating to previous planner hook");
		stmt = PreviousPlannerHook(parse, query_string, cursor_options, bound_params);
	} else {
		bioseq_log<DBG>("[bioseq] pg: BioseqColumnPlannerHook() delegating to standard_planner");
		stmt = standard_planner(parse, query_string, cursor_options, bound_params);
	}
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnPlannerHook() upstream planner returned");

	/*
	 * In the case of a CREATE TABLE AS query, we are not able to successfully
	 * drop out of a parallel insert situation. Disable parallelism for those.
	 */
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnPlannerHook() checking for CREATE TABLE AS");
	if (parse->commandType == CMD_SELECT)
	{
		bioseq_log<DBG>("[bioseq] pg: BioseqColumnPlannerHook() query_string_null={}", query_string == nullptr);
		if (query_string != nullptr && IsCreateTableAs(query_string))
		{
			stmt->parallelModeNeeded = false;
		}
	}

	bioseq_log<DBG>("[bioseq] pg: BioseqColumnPlannerHook() checking vectorization preconditions");
	if (!(bioseq_enable_vectorization			/* Vectorization should be enabled */
			|| bioseq_index_scan)				/* or BioseqColumn Index Scan */
		|| stmt->commandType != CMD_SELECT		/* only SELECTS are supported  */
		|| list_length(stmt->rtable) != 1)		/* JOINs are not yet supported */
		{
			bioseq_log<DBG>("[bioseq] pg: BioseqColumnPlannerHook() fast-return; vec={} idx={} cmd_not_select={} rtable_not_one={}",
				bioseq_enable_vectorization, bioseq_index_scan,
				(stmt->commandType != CMD_SELECT), (list_length(stmt->rtable) != 1));
			return stmt;
		}

	if (bioseq_tableam_oid == InvalidOid)
		bioseq_tableam_oid = get_table_am_oid("bioseq", true);

	Plan* saved_plan_tree = stmt->planTree;
	List* saved_subplans = stmt->subplans;

	MemoryContext saved_context = CurrentMemoryContext;

	bioseq_log<DBG>("[bioseq] pg: BioseqColumnPlannerHook() running plan mutator");
	PG_TRY();
	{
		List* mutated_subplans = NULL;

		plan_tree_mutator plan_mutator;

		PlanTreeMutatorContext plan_tree_context;
		plan_tree_context.vectorizedAggregation = false;

		stmt->planTree = pg_mutate_plan_tree(stmt->planTree, plan_tree_context, plan_mutator);

		bioseq_log<DBG>("[bioseq] pg: BioseqColumnPlannerHook() iterating subplans");
		for (auto subplan_node : PList<Plan>(stmt->subplans))
		{
			bioseq_log<DBG>("[bioseq] pg: BioseqColumnPlannerHook() processing next subplan");
			PlanTreeMutatorContext sub_plan_tree_context;
			sub_plan_tree_context.vectorizedAggregation = false;
			Plan* mutated_subplan = pg_mutate_plan_tree(subplan_node, sub_plan_tree_context, plan_mutator);
			mutated_subplans = lappend(mutated_subplans, mutated_subplan);
		}
		bioseq_log<DBG>("[bioseq] pg: BioseqColumnPlannerHook() subplan iteration complete");

		stmt->subplans = mutated_subplans;
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(saved_context);

		ErrorData* error_data = CopyErrorData();
		FlushErrorState();
		bioseq_report_pg(DEBUG1)
			.code(ERRCODE_INTERNAL_ERROR)
			.msg("Query can't be vectorized. Falling back to original execution.")
			.detail("{}", error_data->message);
		stmt->planTree = saved_plan_tree;
		stmt->subplans = saved_subplans;
		bioseq_log<DBG>("[bioseq] pg: BioseqColumnPlannerHook() caught PG_TRY exception, reverted plan");
	}

	PG_END_TRY();
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnPlannerHook() returning");

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
	bioseq_log<DBG>("[bioseq] pg: IsCreateTableAs() entered");
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
		bioseq_log<DBG>("[bioseq] pg: IsCreateTableAs() returning false; 'create' not found");
		pfree(haystack);
		return false;
	}

	char* table_ptr = strstr(create_ptr + 6, "table");
	if (table_ptr == NULL)
	{
		bioseq_log<DBG>("[bioseq] pg: IsCreateTableAs() returning false; 'table' not found");
		pfree(haystack);
		return false;
	}

	char* as_ptr = strstr(table_ptr + 5, "as");
	if (as_ptr == NULL)
	{
		bioseq_log<DBG>("[bioseq] pg: IsCreateTableAs() returning false; 'as' not found");
		pfree(haystack);
		return false;
	}

	pfree(haystack);

	bioseq_log<DBG>("[bioseq] pg: IsCreateTableAs() returning true");
	return true;
}

/*
 * bioseq_planner_init installs the bioseq planner_hook and
 * ProcessUtility_hook, saving the previous hooks so they can be chained.
 * Also registers the custom-scan node types used by the plan rewriter.
 */
export void bioseq_planner_init()
{
	bioseq_log<DBG>("[bioseq] pg: bioseq_planner_init() entered");

	PreviousPlannerHook = planner_hook;
	planner_hook = BioseqColumnPlannerHook;

	PreviousProcessUtilityHook = ProcessUtility_hook;
	ProcessUtility_hook = ProcessUtilityHook;

	bioseq_register_aggregator_node();
	bioseq_register_indexscan_node();
}

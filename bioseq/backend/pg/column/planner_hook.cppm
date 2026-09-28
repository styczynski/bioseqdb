/**
 * @file planner_hook.cppm
 * @brief Add Bioseq aggregation and index scans to eligible PostgreSQL plans.
 *
 * The planner hook validates mmsearch options and binds match-result helpers
 * before trying optional plan rewrites. It  also clears parallelModeNeeded
 * for queries detected as CREATE TABLE AS. The utility hook logs column
 * definitions for CREATE TABLE USING bioseq.
 */
module;

#include <bioseq/pg_compat.h>

#include <cctype>
#include <cstring>
#include <optional>
#include <string>

export module bioseq.postgres.column.planner_hook;

import bioseq.common.logging;
import bioseq.postgres.extension.cache_provider;
import bioseq.postgres.table;
import bioseq.postgres.table.types;
import bioseq.postgres.support;
import bioseq.postgres.configuration.core;
import bioseq.postgres.vectorization.execution;
import bioseq.postgres.vectorization.aggregator_node;

using namespace bioseq::postgres::support;

static planner_hook_type previous_planner_hook = nullptr;
static ProcessUtility_hook_type previous_process_utility_hook = nullptr;

static Oid column_table_access_method_oid = InvalidOid;

struct plan_tree_mutator_context
{
	bool vectorized_aggregation;
};

/** @brief Check whether a plan is BioseqSliceProject.
 * Its output contains ordinary scalar Datums, so aggregates above it must
 * keep scalar execution. The regivtered CustomName identifies copied plans.
 */
[[nodiscard]] static inline bool is_bioseq_slice_project_scan(
		const Plan* plan) noexcept {
	if (plan == nullptr || !IsA(plan, CustomScan))
	{
		return false;
	}
	const CustomScan* custom_scan = reinterpret_cast<const CustomScan*>(plan);
	return custom_scan->methods != nullptr
		&& custom_scan->methods->CustomName != nullptr
		&& strcmp(custom_scan->methods->CustomName,
			"BioseqSliceProject") == 0;
}

/** @brief Check the mmsearch expression owned by a BioseqScan.
 * The scan stores it in custom_exprs[3] after removing it from Plan.qual.
 * Vector-aggregate checks must inspect this slot because mmsearch produces
 * a scalar row for each match.
 */
[[nodiscard]] static inline bool bioseq_scan_owns_mmsearch(
		const CustomScan* custom_scan) {
	constexpr int owned_mmsearch_expression_index = 3;
	if (custom_scan == nullptr || custom_scan->methods == nullptr
		|| custom_scan->methods->CustomName == nullptr
		|| (custom_scan->methods != bioseq_customscan_methods()
			&& strcmp(custom_scan->methods->CustomName, "BioseqScan") != 0)
		|| list_length(custom_scan->custom_exprs)
			<= owned_mmsearch_expression_index) {
		return false;
	}
	return bioseq_expr_uses_bioseq_projection(
		reinterpret_cast<Node*>(list_nth(
			custom_scan->custom_exprs, owned_mmsearch_expression_index)));
}

/// @brief Make a shallow plan-node copy in the current PostgreSQL memory context.
template<typename p_plan_node>
[[nodiscard]] inline p_plan_node* copy_plan_node(const p_plan_node* source) {
	p_plan_node* cloned = static_cast<p_plan_node*>(palloc(sizeof(p_plan_node)));
	memcpy(cloned, source, sizeof(p_plan_node));
	return cloned;
}


/** @brief Check whether an index belongs to a Bioseq columnar  table.
 * This decides whether an IndexScan can use Bioseq's custom index scan.
 */
static bool
index_belongs_to_column_table(Oid index_oid, Oid column_table_am_oid)
{
	bioseq_log<logging_level::debug>("[bioseq] pg: index_belongs_to_column_table() index_oid={} table_am_oid={}", index_oid, column_table_am_oid);

	HeapTuple index_tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(index_oid));
	if (!HeapTupleIsValid(index_tuple))
	{
		bioseq_log<logging_level::debug>("[bioseq] pg: index_belongs_to_column_table() index syscache miss for oid={}", index_oid);
		return false;
	}
	Form_pg_index index_record = reinterpret_cast<Form_pg_index>(GETSTRUCT(index_tuple));

	HeapTuple table_tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(index_record->indrelid));
	if (!HeapTupleIsValid(table_tuple))
	{
		ReleaseSysCache(index_tuple);
		bioseq_log<logging_level::debug>("[bioseq] pg: index_belongs_to_column_table() table syscache miss for indrelid={}", index_record->indrelid);
		return false;
	}
	Form_pg_class table_record = reinterpret_cast<Form_pg_class>(GETSTRUCT(table_tuple));

	const bool index_on_bioseq_column = table_record->relam == column_table_am_oid;

	ReleaseSysCache(index_tuple);
	ReleaseSysCache(table_tuple);

	return index_on_bioseq_column;
}

/* Rewrite aggregate arguments to call supported vector operations.
 * Binary operators must have a supported column/constant argument pattern
 * Other expressions directly below an argument TargetEntry must be Vars.
 * Track the parent node to enforce that restriction during traversal.
 */
struct aggref_args_state {
	Node* parent = nullptr;
};

struct aggref_args_mutator {
	[[nodiscard]] pg_node_mutation
	operator()(pg_expression_mutation_cursor cursor, pg_node_view node, aggref_args_state& state) const {
		if (OpExpr* op_expr_node = try_cast_pg_operator_expression(node.get()))
		{
			if (list_length(op_expr_node->args) != 2)
				bioseq_log<logging_level::error, logging_target::postgres>( "Aggregation vectorization works only on two arguments.");

			if (!is_pg_const_var_binary_args(op_expr_node->args))
				bioseq_log<logging_level::error, logging_target::postgres>( "Unsupported aggregate argument combination.");

			HeapTuple operator_tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(op_expr_node->opno));
			Form_pg_operator operator_form = reinterpret_cast<Form_pg_operator>(GETSTRUCT(operator_tuple));
			Oid procedure_oid = operator_form->oprcode;
			ReleaseSysCache(operator_tuple);

			const std::optional<Oid> vectorized_procedure_oid =
				lookup_vectorized_routine_oid(procedure_oid);
			if (!vectorized_procedure_oid)
				bioseq_log<logging_level::error, logging_target::postgres>( "Vectorized aggregate not found.");

			op_expr_node->opfuncid = *vectorized_procedure_oid;

			return pg_node_mutation::replace_with(to_pg_node(op_expr_node));
		}

		/* Reject unsupported expressions used directly as aggregate arguments. */
		if (state.parent != nullptr
			&& pg_node_view{state.parent}.is_type<TargetEntry>()
			&& !node.is_type<Var>())
			bioseq_log<logging_level::error, logging_target::postgres>( "Vectorized Aggregates accept only valid column argument");

		Node* saved_parent = state.parent;
		state.parent = node.get();
		Node* rewritten = cursor.descend(node.get());
		state.parent = saved_parent;
		return pg_node_mutation::replace_with(rewritten);
	}
};

/* Copy aggregate expressions and select their vector implementations.
 * Rewrite their arguments as  well. DISTINCT and FILTER aggregates are
 * rejected here so the caller can keep ordinary execution.
 */
struct expr_mutator_state {};

struct expr_mutator {
	[[nodiscard]] pg_node_mutation
	operator()(pg_expression_mutation_cursor, pg_node_view node, expr_mutator_state&) const {
		if (Aggref* old_aggref_node = node.try_cast<Aggref>())
		{
			Aggref* new_aggref_node = copy_pg_node(old_aggref_node);

			if (old_aggref_node->aggdistinct)
			{
				bioseq_log<logging_level::error, logging_target::postgres>( "Vectorized aggregate with DISTINCT not supported.");
			}

			if (old_aggref_node->aggfilter)
			{
				bioseq_log<logging_level::error, logging_target::postgres>( "Vectorized aggregate with FILTER not supported");
			}

			aggref_args_state args_state;
			aggref_args_mutator args_mutator;
			new_aggref_node->args = reinterpret_cast<List*>(
				mutate_pg_expression(
					to_pg_node(old_aggref_node->args), args_state, args_mutator));

			const std::optional<Oid> vectorized_procedure_oid =
				lookup_vectorized_routine_oid(
					new_aggref_node->aggfnoid);
			if (!vectorized_procedure_oid)
			{
				bioseq_log<logging_level::error, logging_target::postgres>( "Vectorized aggregate not found.");
			}

			new_aggref_node->aggfnoid = *vectorized_procedure_oid;

			return pg_node_mutation::replace_with(to_pg_node(new_aggref_node));
		}

		return pg_node_mutation::descend();
	}
};


/* Rewrite supported aggregate and, index-scan nodes. Mark BioseqScan
 * children when they must supply vectors to an aggregate. The returned
 * mutation tells the walker whethr to replace a node or visit its children.
 */
struct plan_tree_mutator {
	[[nodiscard]] pg_plan_mutation
	operator()(pg_plan_mutation_cursor cursor, pg_node_view node, plan_tree_mutator_context& plan_tree_context) const {
		if (CustomScan* custom_scan = node.try_cast<CustomScan>())
		{
			bioseq_log<logging_level::debug>("[bioseq] pg: plan_tree_mutator() encountered CustomScan");

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
				vectorized_aggregate_execution->consttype = k_vectorized_aggregate_private_marker;
				vectorized_aggregate_execution->constvalue = BoolGetDatum(plan_tree_context.vectorized_aggregation);
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
				bioseq_log<logging_level::debug>("[bioseq] pg: plan_tree_mutator() traversing Bioseq custom scan {}", custom_name);
			}

			return pg_plan_mutation::descend();
		}

		if (Agg* agg_node = node.try_cast<Agg>())
		{
			bioseq_log<logging_level::debug>("[bioseq] pg: plan_tree_mutator() encountered Agg");

			if (!bioseq::settings::vectorization::enabled)
				return pg_plan_mutation::descend();

			/* SliceProject returns scalar values. Keep this aggregate and its
			 * BioseqScan child in scalar mode so vector functions cannot treat
			 * ordinary Datums as vector_column pointers. Restore the surrounding
			 * traversal mode after visiting the children. */
			if (is_bioseq_slice_project_scan(agg_node->plan.lefttree)) {
				bioseq_log<logging_level::debug>("[bioseq] pg: plan_tree_mutator() Agg has BioseqSliceProject child -- keeping scalar aggregate");
				const bool saved_vectorized_aggregation =
					plan_tree_context.vectorized_aggregation;
				plan_tree_context.vectorized_aggregation = false;
				Plan* scalar_aggregate =
					cursor.descend_children(&agg_node->plan);
				plan_tree_context.vectorized_aggregation =
					saved_vectorized_aggregation;
				return pg_plan_mutation::replace_with(scalar_aggregate);
			}

			if (agg_node->plan.lefttree->type == T_CustomScan)
			{
				bioseq_log<logging_level::debug>("[bioseq] pg: plan_tree_mutator() Agg has CustomScan child");
				CustomScan* aggregate_input = reinterpret_cast<CustomScan*>(
					agg_node->plan.lefttree);
				/* Match projections and mmsearch produce  scalar tuple slots.
				 * For example, MIN(bioseq.target_start(a)) aggregates one scalar
				 * value per match. Keep ordinary aggregation here because VectorAgg
				 * expects vector_tuple_table_slot_storage and would misread the slot. */
				const bool agg_input_uses_bioseq_projection =
					bioseq_expr_uses_bioseq_projection(
						reinterpret_cast<Node*>(agg_node->plan.targetlist))
					|| bioseq_expr_uses_bioseq_projection(
						reinterpret_cast<Node*>(agg_node->plan.qual))
					|| bioseq_expr_uses_bioseq_projection(
						reinterpret_cast<Node*>(agg_node->plan.lefttree->targetlist))
					|| bioseq_expr_uses_bioseq_projection(
						reinterpret_cast<Node*>(agg_node->plan.lefttree->qual));
				const bool agg_input_owns_mmsearch =
					bioseq_scan_owns_mmsearch(aggregate_input);
				if (agg_input_uses_bioseq_projection
					|| agg_input_owns_mmsearch) {
					bioseq_log<logging_level::debug>("[bioseq] pg: plan_tree_mutator() Agg input depends on bioseq projection / executor-owned mmsearch — skipping vectorised aggregator rewrite");
					return pg_plan_mutation::descend();
				}
				if (agg_node->aggstrategy == AGG_PLAIN)
				{
					bioseq_log<logging_level::debug>("[bioseq] pg: plan_tree_mutator() rewriting AGG_PLAIN into vectorized aggregator");
					CustomScan* vectorized_agg_node = create_vector_aggregate_scan();

					Agg* new_agg = copy_plan_node(agg_node);

					expr_mutator_state targetlist_state;
					expr_mutator targetlist_mutator;
					new_agg->plan.targetlist =
						reinterpret_cast<List*>(mutate_pg_expression(
							reinterpret_cast<Node*>(new_agg->plan.targetlist),
							targetlist_state,
							targetlist_mutator));


					vectorized_agg_node->custom_plans =
						lappend(vectorized_agg_node->custom_plans, new_agg);
					vectorized_agg_node->scan.plan.targetlist =
						build_custom_target_list(agg_node->plan.targetlist, INDEX_VAR);
					vectorized_agg_node->custom_scan_tlist = new_agg->plan.targetlist;

					Plan* vectorized_agg_plan = reinterpret_cast<Plan*>(vectorized_agg_node);
					copy_plan_costs(vectorized_agg_plan, &agg_node->plan);
					vectorized_agg_plan->parallel_aware = agg_node->plan.lefttree->parallel_aware;


					plan_tree_context.vectorized_aggregation = true;

					/* Rewrite the original aggregate's children before attaching them
					 * to the vector aggregate. Use the returned child pointers so nested
					 * index-scan replacements remain in the plan. */
					Plan* recursed_agg_plan = cursor.descend_children(&agg_node->plan);

					vectorized_agg_node->scan.plan.lefttree = recursed_agg_plan->lefttree;
					vectorized_agg_node->scan.plan.righttree = recursed_agg_plan->righttree;

					return pg_plan_mutation::replace_with(
						reinterpret_cast<Plan*>(vectorized_agg_node));
				}

				return pg_plan_mutation::descend();
			}

			return pg_plan_mutation::descend();
		}

		if (IndexScan* index_scan_node = node.try_cast<IndexScan>())
		{
			bioseq_log<logging_level::debug>("[bioseq] pg: plan_tree_mutator() encountered IndexScan; bioseq::settings::planner::index_scan::enabled={}", bioseq::settings::planner::index_scan::enabled);
			if (!bioseq::settings::planner::index_scan::enabled)
				return pg_plan_mutation::skip();

			if (!index_belongs_to_column_table(index_scan_node->indexid, column_table_access_method_oid)) {
				bioseq_log<logging_level::debug>("[bioseq] pg: plan_tree_mutator() IndexScan not on columnar table; skipping rewrite");
				return pg_plan_mutation::skip();
			}

			CustomScan* bioseq_column_index_scan = bioseq_create_index_scan_node();
			IndexScan* new_index_scan = copy_plan_node(index_scan_node);

			bioseq_column_index_scan->custom_plans =
						lappend(bioseq_column_index_scan->custom_plans, new_index_scan);

			bioseq_column_index_scan->scan.plan.targetlist =
						build_custom_target_list(index_scan_node->scan.plan.targetlist, INDEX_VAR);

			bioseq_column_index_scan->custom_scan_tlist = new_index_scan->scan.plan.targetlist;

			Plan* bioseq_column_index_scan_plan = reinterpret_cast<Plan*>(bioseq_column_index_scan);
			copy_plan_costs(bioseq_column_index_scan_plan, &index_scan_node->scan.plan);

			bioseq_log<logging_level::debug>("[bioseq] pg: plan_tree_mutator() IndexScan rewritten to bioseq custom scan");
			return pg_plan_mutation::replace_with(
				reinterpret_cast<Plan*>(bioseq_column_index_scan));
		}

		return pg_plan_mutation::descend();
	}
};

/* Log column definitions for CREATE TABLE USING bioseq, then pass the
 * statement to the previous utility hook or PostgreSQL's standard handler
 */
static void process_utility_hook(
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
		if (CreateStmt* create_statement = try_cast_pg_node<CreateStmt>(utility_statement)) {
			if (create_statement->accessMethod != nullptr) {
				std::string access_method(create_statement->accessMethod);
				if (access_method == "bioseq") {
					bioseq_log<logging_level::debug>("[bioseq] pg: process_utility_hook() CREATE TABLE USING columnar");
					for (Node* element :
						 postgres_list<Node>{create_statement->tableElts}) {
						if (ColumnDef* column_def = try_cast_pg_node<ColumnDef>(element)) {
							bioseq_log<logging_level::debug>("[bioseq] pg: process_utility_hook() column '{}'", column_def->colname);
						}
					}
				}
			}
		}
	}

	if (previous_process_utility_hook) {
		previous_process_utility_hook(
			pstmt, query_string, read_only_tree, utility_context, params,
			query_env, dest, completion);
	} else {
		standard_ProcessUtility(
			pstmt, query_string, read_only_tree, utility_context, params,
			query_env, dest, completion);
	}
}

/* Guess whether query text contains CREATE TABLE AS by finding those
 * strings in order, ignoring case. This  does not parse SQL, so names,
 * comments or quoted text can also matc.
 */
static bool query_may_be_create_table_as(const char* query) {
    bioseq_log<logging_level::debug>("[bioseq] pg: query_may_be_create_table_as() entered");
    const size_t query_length = strlen(query);
    char* haystack = static_cast<char*>(palloc(query_length + 1));

    for (size_t character_index = 0; character_index < query_length; character_index++) {
        haystack[character_index] = tolower(query[character_index]);
    }
    haystack[query_length] = '\0';

    char* create_ptr = strstr(haystack, "create");
    if (create_ptr == nullptr) {
        bioseq_log<logging_level::debug>(
            "[bioseq] pg: query_may_be_create_table_as() returning false; 'create' not found");
        pfree(haystack);
        return false;
    }

    char* table_ptr = strstr(create_ptr + 6, "table");
    if (table_ptr == nullptr) {
        bioseq_log<logging_level::debug>(
            "[bioseq] pg: query_may_be_create_table_as() returning false; 'table' not found");
        pfree(haystack);
        return false;
    }

    char* as_ptr = strstr(table_ptr + 5, "as");
    if (as_ptr == nullptr) {
        bioseq_log<logging_level::debug>(
            "[bioseq] pg: query_may_be_create_table_as() returning false; 'as' not found");
        pfree(haystack);
        return false;
    }

    pfree(haystack);

    bioseq_log<logging_level::debug>("[bioseq] pg: query_may_be_create_table_as() returning true");
    return true;
}

/* Run the previous planner, bind match helpers, then try Bioseq
 * aggregation and index-scan rewrites for eligible SELECT plans. If an
 * optional rewrite raises a PostgreSQL error, restore the saved plan pointers.
 */
static PlannedStmt*
column_planner_hook(Query* parse,
					const char* query_string,
					int cursor_options,
					ParamListInfo bound_params
#if PG_VERSION_NUM >= 190000
                    , ExplainState* explain_state
#endif
                    )
{
	bioseq_log<logging_level::debug>("[bioseq] pg: column_planner_hook() entered");

	/* Check mmsearch options before planning can fold a volatile SQL wrapper
	 * into a Const. The third argument must stay fixed for the statement. */
	bioseq_validate_raw_mmsearch_options(parse);

	PlannedStmt* stmt;
	if (previous_planner_hook) {
		bioseq_log<logging_level::debug>("[bioseq] pg: column_planner_hook() delegating to previous planner hook");
		stmt = previous_planner_hook(parse, query_string, cursor_options, bound_params
#if PG_VERSION_NUM >= 190000
                                     , explain_state
#endif
        );
	} else {
		bioseq_log<logging_level::debug>("[bioseq] pg: column_planner_hook() delegating to standard_planner");
		stmt = standard_planner(parse, query_string, cursor_options, bound_params
#if PG_VERSION_NUM >= 190000
                                , explain_state
#endif
        );
	}
	bioseq_log<logging_level::debug>("[bioseq] pg: column_planner_hook() upstream planner returned");

	/* Final plan-node ids identify the scans that own match-result helpers.
	 * Bind them before any early  return or optional rewrite so copies and
	 * rescans keep the same owner. Ambiguous, ownership must raise a planning
	 * error outside the optional vectorization fallback. */
	bioseq_finalize_match_helper_bindings(stmt);

	/* Clear the parallel-mode flag for possible CREATE TABLE AS queries,
  * which may write through the columnar table access method. */
	bioseq_log<logging_level::debug>("[bioseq] pg: column_planner_hook() checking for CREATE TABLE AS");
	if (parse->commandType == CMD_SELECT)
	{
		bioseq_log<logging_level::debug>("[bioseq] pg: column_planner_hook() query_string_null={}", query_string == nullptr);
		if (query_string != nullptr && query_may_be_create_table_as(query_string))
		{
			stmt->parallelModeNeeded = false;
		}
	}

	bioseq_log<logging_level::debug>("[bioseq] pg: column_planner_hook() checking vectorization preconditions");
	if (!(bioseq::settings::vectorization::enabled			/* Vector aggregation enabled */
			|| bioseq::settings::planner::index_scan::enabled)				/* Bioseq index scans enabled */
		|| stmt->commandType != CMD_SELECT		/* Rewrites require SELECT */
		|| list_length(stmt->rtable) != 1)		/* These rewrites require one range-table entry */
		{
			bioseq_log<logging_level::debug>("[bioseq] pg: column_planner_hook() fast-return; vec={} idx={} cmd_not_select={} rtable_not_one={}",
				bioseq::settings::vectorization::enabled, bioseq::settings::planner::index_scan::enabled,
				(stmt->commandType != CMD_SELECT), (list_length(stmt->rtable) != 1));
			bioseq::postgres::extension::cache_provider::
				bioseq_cache_provider_after_planned_statement(stmt);
			return stmt;
		}

	if (column_table_access_method_oid == InvalidOid)
		column_table_access_method_oid = get_table_am_oid("bioseq", true);

	Plan* saved_plan_tree = stmt->planTree;
	List* saved_subplans = stmt->subplans;

	MemoryContext saved_context = CurrentMemoryContext;

	bioseq_log<logging_level::debug>("[bioseq] pg: column_planner_hook() running plan mutator");
	PG_TRY();
	{
		List* mutated_subplans = nullptr;

		plan_tree_mutator plan_mutator;

		plan_tree_mutator_context plan_tree_context;
		plan_tree_context.vectorized_aggregation = false;

		stmt->planTree = mutate_pg_plan_tree(
			stmt->planTree, plan_tree_context, plan_mutator);

		bioseq_log<logging_level::debug>("[bioseq] pg: column_planner_hook() iterating subplans");
		for (Plan* subplan_node : postgres_list<Plan>{stmt->subplans})
		{
			bioseq_log<logging_level::debug>("[bioseq] pg: column_planner_hook() processing next subplan");
			plan_tree_mutator_context sub_plan_tree_context;
			sub_plan_tree_context.vectorized_aggregation = false;
			Plan* mutated_subplan = mutate_pg_plan_tree(
				subplan_node, sub_plan_tree_context, plan_mutator);
			mutated_subplans = lappend(mutated_subplans, mutated_subplan);
		}
		bioseq_log<logging_level::debug>("[bioseq] pg: column_planner_hook() subplan iteration complete");

		stmt->subplans = mutated_subplans;
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(saved_context);

		ErrorData* error_data = CopyErrorData();
		FlushErrorState();
		bioseq_report_pg(DEBUG1)
			.with_sql_error_code(ERRCODE_INTERNAL_ERROR)
			.with_message("Query can't be vectorized. Falling back to original execution.")
			.with_detail("{}", error_data->message);
		stmt->planTree = saved_plan_tree;
		stmt->subplans = saved_subplans;
		bioseq_log<logging_level::debug>("[bioseq] pg: column_planner_hook() caught PG_TRY exception, reverted plan");
	}

	PG_END_TRY();
	bioseq_log<logging_level::debug>("[bioseq] pg: column_planner_hook() returning");

	bioseq::postgres::extension::cache_provider::
		bioseq_cache_provider_after_planned_statement(stmt);
	return stmt;
}

/** @brief Install Bioseq planner and utility hooks.
 * Keep  the previous hooks so calls can be chained. Register the vector
 * aggregate and index-scan node types used by the plan rewriter
 */
export void install_column_planner_hooks()
{
	bioseq_log<logging_level::debug>("[bioseq] pg: install_column_planner_hooks() entered");

	previous_planner_hook = planner_hook;
	planner_hook = column_planner_hook;

	previous_process_utility_hook = ProcessUtility_hook;
	ProcessUtility_hook = process_utility_hook;

	register_vector_aggregate_scan();
	bioseq_register_index_scan_node();
}

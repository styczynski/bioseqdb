/**
 * @file tuple_ops.cppm
 * @brief Tuple modification helpers for columnar metadata catalogs.
 *
 * Shared by storage_metadata catalog code.
 * Keeping executor-state setup and tuple insert/delete plumbing in this leaf
 * partition keeps metadata-core focused on catalog semantics.
 */
module;
#include <bioseq/pg_compat.h>
#include "safe_lib.h"

#include <cstring>

export module bioseq.postgres.storage.metadata:tuple_ops;

import bioseq.postgres.support;

export namespace bioseq::storage::metadata::tuple_ops {

/**
 * @brief Holds borrowed relation state and executor resources for catalog mutations.
 *
 * The relation remains owned by the caller. `finish_relation_modification` releases the
 * executor and index resources and invalidates the remaining pointers.
 */
struct relation_modification_state
{
	Relation relation;
	EState *executor_state;
	ResultRelInfo *result_rel_info;
};

/// @brief Creates executor state configured for catalog tuple modification.
/// @param[in] relation Open relation borrowed for the executor's range table.
/// @return Executor state that must be released after the mutation batch.
EState *
create_relation_executor_state(Relation relation)
{
	EState *executor_state = CreateExecutorState();

	RangeTblEntry *range_table_entry = makeNode(RangeTblEntry);
	range_table_entry->rtekind = RTE_RELATION;
	range_table_entry->relid = RelationGetRelid(relation);
	range_table_entry->relkind = relation->rd_rel->relkind;
	range_table_entry->rellockmode = AccessShareLock;

	List *permission_info = NIL;
	addRTEPermissionInfo(&permission_info, range_table_entry);
	bioseq_exec_init_range_table_without_pruning(
		executor_state, list_make1(range_table_entry), permission_info);

	executor_state->es_output_cid = GetCurrentCommandId(true);

	/* Prepare to catch AFTER triggers. */
	AfterTriggerBeginQuery();

	return executor_state;
}

/// @brief Opens indexes and allocates state for relation modification.
/// @param[in] relation Open relation retained by the caller for the whole mutation batch.
/// @return Transaction-context state closed by `finish_relation_modification`.
relation_modification_state *
begin_relation_modification(Relation relation)
{
	EState *executor_state = create_relation_executor_state(relation);
	ResultRelInfo *result_rel_info = makeNode(ResultRelInfo);

	InitResultRelInfo(result_rel_info, relation, 1, nullptr, 0);

	/* ExecSimpleRelationInsert, ... require caller to open indexes */
	ExecOpenIndices(result_rel_info, false);

	relation_modification_state *state =
		bioseq::postgres::support::allocate_zeroed_object<
			relation_modification_state>();
	state->relation = relation;
	state->executor_state = executor_state;
	state->result_rel_info = result_rel_info;

	return state;
}

/**
 * @brief Inserts one catalog tuple while enforcing indexes, constraints, and triggers.
 * @param[in] state Active modification state for the destination relation.
 * @param[in] values Attribute values ordered by the destination tuple descriptor.
 * @param[in] nulls Per-attribute null flags parallel to `values`.
 * @note PostgreSQL reports constraint and trigger failures through its error boundary.
 */
void
insert_catalog_tuple(relation_modification_state *state, Datum *values, bool *nulls)
{
	TupleDesc tuple_desc = RelationGetDescr(state->relation);
	HeapTuple tuple = heap_form_tuple(tuple_desc, values, nulls);

	TupleTableSlot *slot = ExecInitExtraTupleSlot(state->executor_state, tuple_desc,
												  &TTSOpsHeapTuple);

	/*
	 * The tuple has no other reference. Therefore, we can safely set
	 * should_free to true, which can avoid duplicate memory allocation.
	 *
	 * When working with low-memory machines, it's important to be mindful
	 * of duplicate memory allocation for large values. This can lead to
	 * out-of-memory errors and cause issues with the performance of the
	 * machine.
	 */
	ExecStoreHeapTuple(tuple, slot, true);

	/* use ExecSimpleRelationInsert to enforce constraints */
	ExecSimpleRelationInsert(state->result_rel_info, state->executor_state, slot);
}

/**
 * @brief Deletes one catalog tuple and runs its row-delete triggers.
 * @param[in] state Active modification state for the owning relation.
 * @param[in] heap_tuple Borrowed tuple whose self pointer identifies the row to delete.
 */
void
delete_catalog_tuple(relation_modification_state *state, HeapTuple heap_tuple)
{
	EState *executor_state = state->executor_state;
	ResultRelInfo *result_rel_info = state->result_rel_info;

	ItemPointer tuple_id = &(heap_tuple->t_self);
	simple_heap_delete(state->relation, tuple_id);

	/* execute AFTER ROW DELETE Triggers to enforce constraints */
	ExecARDeleteTriggers(
		executor_state, result_rel_info, tuple_id, nullptr, nullptr, true);
}

/**
 * @brief Closes indexes, fires queued triggers, and releases executor resources.
 * @param[in] state Completed modification state, invalid after this call.
 */
void
finish_relation_modification(relation_modification_state *state)
{
	ExecCloseIndices(state->result_rel_info);

	AfterTriggerEndQuery(state->executor_state);

	ExecCloseResultRelations(state->executor_state);
	ExecCloseRangeTableRelations(state->executor_state);

	ExecResetTupleTable(state->executor_state->es_tupleTable, false);
	FreeExecutorState(state->executor_state);

	CommandCounterIncrement();
}

} // namespace bioseq::storage::metadata::tuple_ops

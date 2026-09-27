/**
 * @file tuple_ops.cppm
 * @brief Write metadata rows using PostgreSQL executor state and index maintenance.
 */
module;
#include <bioseq/pg_compat.h>
#include "safe_lib.h"

#include <cstring>

export module bioseq.postgres.storage.metadata:tuple_ops;

import bioseq.postgres.support;

export namespace bioseq::storage::metadata::tuple_ops {

/**
 * @brief Resources used to write a batch of metadata rows.
 *
 * The  caller keeps the relation open. finish_relation_modification() releases
 * the executor and indexes, then unregisters any snapshot registered for the batch.
 */
struct relation_modification_state
{
	Relation relation;
	EState *executor_state;
	ResultRelInfo *result_rel_info;
	Snapshot retained_snapshot;
};

/// @brief Prepare executor state for writes to a metadata relation.
/// @param[in] relation Open relation borrowed for the executor's range table.
/// @return State to release after the batch finishes.
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

	/* Begin collecting AFTER-trigger events for this batch. */
	AfterTriggerBeginQuery();

	return executor_state;
}

/// @brief Open indexes and prepare a batch of metadata writes.
/// @param[in] relation Relation the caller keeps open until the batch finishes.
/// @return State to pdss to finish_relation_modification().
relation_modification_state *
begin_relation_modification(Relation relation)
{
	// A PRE_COMMIT flush can run after the statement released its snapshot.
 // Register one for TOAST writes when none is registered  or active.
 // A caller's existing snapshot keeps its current visibility rules.
	Snapshot retained_snapshot = InvalidSnapshot;
	if (!HaveRegisteredOrActiveSnapshot()) {
		retained_snapshot = RegisterSnapshot(GetTransactionSnapshot());
	}
	EState *executor_state = create_relation_executor_state(relation);
	ResultRelInfo *result_rel_info = makeNode(ResultRelInfo);

	InitResultRelInfo(result_rel_info, relation, 1, nullptr, 0);

	/* Executor inserts require the relation's indexes to be open. */
	ExecOpenIndices(result_rel_info, false);

	relation_modification_state *state =
		bioseq::postgres::support::allocate_zeroed_object<
			relation_modification_state>();
	state->relation = relation;
	state->executor_state = executor_state;
	state->result_rel_info = result_rel_info;
	state->retained_snapshot = retained_snapshot;

	return state;
}

/**
 * @brief Insert a metadata row with executor constraint and index checks.
 * @param[in] state Active write batch for the destination relation.
 * @param[in] values Column values in tuple-descriptor order
 * @param[in] nulls Null flags corresponding to values.
 * @note PostgreSQL handles row triggers and reports insertion errors.
 */
void
insert_catalog_tuple(relation_modification_state *state, Datum *values, bool *nulls)
{
	TupleDesc tuple_desc = RelationGetDescr(state->relation);
	HeapTuple tuple = heap_form_tuple(tuple_desc, values, nulls);

	TupleTableSlot *slot = ExecInitExtraTupleSlot(state->executor_state, tuple_desc,
												  &TTSOpsHeapTuple);

	/* Give the slot ownership of this tuple to avoid copying its values. */
	ExecStoreHeapTuple(tuple, slot, true);

	/* Let PostgreSQL check constraints and maintain indexes. */
	ExecSimpleRelationInsert(state->result_rel_info, state->executor_state, slot);
}

/**
 * @brief Update a metadata row and maintain its indexes.
 * @param[in] state Active batch with the relation's indexes open.
 * @param[in] old_tuple Row  version to replace.
 * @param[in] new_tuple Replacement tuple. The update sets its self pointer.
 *
 * The executor evaluates partial-index predicates and index expressions.
 * PostgreSQL decides which indexes need updating. The, caller handles row triggers.
 */
void
update_catalog_tuple(relation_modification_state *state, HeapTuple old_tuple,
                     HeapTuple new_tuple)
{
	TU_UpdateIndexes update_indexes = TU_All;
	simple_heap_update(state->relation, &old_tuple->t_self, new_tuple, &update_indexes);
	if (state->result_rel_info->ri_NumIndices == 0 || update_indexes == TU_None) {
		return;
	}
	TupleTableSlot *slot = ExecInitExtraTupleSlot(
		state->executor_state, RelationGetDescr(state->relation), &TTSOpsHeapTuple);
	ExecStoreHeapTuple(new_tuple, slot, false);
#if PG_VERSION_NUM >= 190000
	// This direct write has no planner bitmap of updated columns.
	// Keep indexUnchanged false so index maintenance makes no such assumption.
	uint32 flags = 0;
	if (update_indexes == TU_Summarizing) {
		flags |= EIIT_ONLY_SUMMARIZING;
	}
	List *recheck_indexes = ExecInsertIndexTuples(
		state->result_rel_info, state->executor_state, flags, slot, NIL, nullptr);
#else
	List *recheck_indexes = ExecInsertIndexTuples(
		state->result_rel_info, slot, state->executor_state, false, false,
		nullptr, NIL, update_indexes == TU_Summarizing);
#endif
	list_free(recheck_indexes);
	ExecClearTuple(slot);
}

/**
 * @brief Delete a metadata row and queue its AFTER DELETE triggers.
 * @param[in] state Active write bahch for the relation.
 * @param[in] heap_tuple Tuple whose self pointer identifies the row.
 */
void
delete_catalog_tuple(relation_modification_state *state, HeapTuple heap_tuple)
{
	EState *executor_state = state->executor_state;
	ResultRelInfo *result_rel_info = state->result_rel_info;

	ItemPointer tuple_id = &(heap_tuple->t_self);
	simple_heap_delete(state->relation, tuple_id);

	/* Queue AFTER DELETE triggers, including constraint triggers. */
	ExecARDeleteTriggers(
		executor_state, result_rel_info, tuple_id, nullptr, nullptr, true);
}

/**
 * @brief Finish the batch, fire queued triggers and release executor resources.
 * @param[in] state Completed batch. Its executor pointers are  invalid afterward.
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
	if (state->retained_snapshot != InvalidSnapshot) {
		UnregisterSnapshot(state->retained_snapshot);
		state->retained_snapshot = InvalidSnapshot;
	}
}

} // namespace bioseq::storage::metadata::tuple_ops

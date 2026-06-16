/*-------------------------------------------------------------------------
 *
 * metadata-tuple-ops.cppm
 *
 * Tuple modification helpers shared by storage_metadata catalog code.
 * Keeping executor-state setup and tuple insert/delete plumbing in this leaf
 * partition keeps metadata-core focused on catalog semantics.
 *
 *-------------------------------------------------------------------------
 */
module;
#include "common.h"
#include "safe_lib.h"

#include <cstring>

export module bioseq.storage.metadata:tuple_ops;

import bioseq.pg_utils;

export namespace bioseq::metadata::tuple_ops {

struct ModifyState
{
	Relation rel;
	EState *estate;
	ResultRelInfo *resultRelInfo;
};

/*
 * Based on similar code from postgres/src/backend/replication/logical/worker.c
 * and copy.c.
 */
EState *
create_estate_for_relation(Relation rel)
{
	EState *estate = CreateExecutorState();

	RangeTblEntry *rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = RelationGetRelid(rel);
	rte->relkind = rel->rd_rel->relkind;
	rte->rellockmode = AccessShareLock;

	List *perminfos = NIL;
	addRTEPermissionInfo(&perminfos, rte);
	ExecInitRangeTable_compat(estate, list_make1(rte), perminfos);

	estate->es_output_cid = GetCurrentCommandId(true);

	/* Prepare to catch AFTER triggers. */
	AfterTriggerBeginQuery();

	return estate;
}

/*
 * StartModifyRelation allocates resources for modifications.
 */
ModifyState *
start_modify_relation(Relation rel)
{
	EState *estate = create_estate_for_relation(rel);
	ResultRelInfo *resultRelInfo = makeNode(ResultRelInfo);

	InitResultRelInfo(resultRelInfo, rel, 1, NULL, 0);

	/* ExecSimpleRelationInsert, ... require caller to open indexes */
	ExecOpenIndices(resultRelInfo, false);

	ModifyState *modifyState = make0<ModifyState>();
	modifyState->rel = rel;
	modifyState->estate = estate;
	modifyState->resultRelInfo = resultRelInfo;

	return modifyState;
}

/*
 * insert_tuple_and_enforce_constraints inserts a tuple into a relation and makes
 * sure that inserted tuple obeys the constraints.
 */
void
insert_tuple_and_enforce_constraints(ModifyState *state, Datum *values, bool *nulls)
{
	TupleDesc tupleDesc = RelationGetDescr(state->rel);
	HeapTuple tuple = heap_form_tuple(tupleDesc, values, nulls);

	TupleTableSlot *slot = ExecInitExtraTupleSlot(state->estate, tupleDesc,
												  &TTSOpsHeapTuple);

	/*
	 * The tuple has no other reference. Therefore, we can safely set
	 * shouldFree to true, which can avoid duplicate memory allocation.
	 *
	 * When working with low-memory machines, it's important to be mindful
	 * of duplicate memory allocation for large values. This can lead to
	 * out-of-memory errors and cause issues with the performance of the
	 * machine.
	 */
	ExecStoreHeapTuple(tuple, slot, true);

	/* use ExecSimpleRelationInsert to enforce constraints */
	ExecSimpleRelationInsert_compat(state->resultRelInfo, state->estate, slot);
}

/*
 * delete_tuple_and_enforce_constraints deletes a tuple from a relation and
 * enforces constraints by checking index constraints.
 */
void
delete_tuple_and_enforce_constraints(ModifyState *state, HeapTuple heapTuple)
{
	EState *estate = state->estate;
	ResultRelInfo *resultRelInfo = state->resultRelInfo;

	ItemPointer tid = &(heapTuple->t_self);
	simple_heap_delete(state->rel, tid);

	/* execute AFTER ROW DELETE Triggers to enforce constraints */
	ExecARDeleteTriggers(estate, resultRelInfo, tid, NULL, NULL, true);
}

/*
 * finish_modify_relation cleans up resources after modifications are done.
 */
void
finish_modify_relation(ModifyState *state)
{
	ExecCloseIndices(state->resultRelInfo);

	AfterTriggerEndQuery(state->estate);

	ExecCloseResultRelations(state->estate);
	ExecCloseRangeTableRelations(state->estate);

	ExecResetTupleTable(state->estate->es_tupleTable, false);
	FreeExecutorState(state->estate);

	CommandCounterIncrement();
}

/*
 * datum_to_bytea serializes a datum into a bytea value.
 */
bytea *
datum_to_bytea(Datum value, Form_pg_attribute attrForm)
{
	int datumLength = att_addlength_datum(0, attrForm->attlen, value);
	bytea *result = make1<bytea>(datumLength + VARHDRSZ);

	SET_VARSIZE(result, datumLength + VARHDRSZ);

	if (attrForm->attlen > 0)
	{
		if (attrForm->attbyval)
		{
			Datum tmp;
			store_att_byval(&tmp, value, attrForm->attlen);
			memcpy(VARDATA(result), &tmp, attrForm->attlen);
		}
		else
		{
			memcpy(VARDATA(result), DatumGetPointer(value), attrForm->attlen);
		}
	}
	else
	{
		memcpy(VARDATA(result), DatumGetPointer(value), datumLength);
	}

	return result;
}

/*
 * bytea_to_datum deserializes a value which was previously serialized using
 * datum_to_bytea.
 */
Datum
bytea_to_datum(bytea *bytes, Form_pg_attribute attrForm)
{
	/*
	 * We copy the data so the result of this function lives even after the
	 * bytea Datum is freed.
	 */
	char *binaryDataCopy = make1<char>(VARSIZE_ANY_EXHDR(bytes));
	memcpy(binaryDataCopy, VARDATA_ANY(bytes), VARSIZE_ANY_EXHDR(bytes));

	return fetch_att(binaryDataCopy, attrForm->attbyval, attrForm->attlen);
}

} // namespace bioseq::metadata::tuple_ops

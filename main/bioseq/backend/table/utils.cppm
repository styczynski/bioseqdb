module;

#include "common.h"

#include <math.h>

#include "columnar/columnar.h"
#include "columnar/columnar_tableam.h"

#include "columnar/vectorization/columnar_vector_types.h"

#include "common.h"

export module bioseq_table:utils;
import storage_cache;
import storage_physical;
import mlogging;
import storage_metadata;
import bioseq_table_types;

const TableAmRoutine * GetColumnarTableAmRoutine(void);

/*
 * IsColumnarTableAmTable returns true if relation has bioseq_tableam
 * access method. This can be called before extension creation.
 */
export bool
IsColumnarTableAmTable(Oid relationId)
{
	BMethod(IsColumnarTableAmTable);
	if (!OidIsValid(relationId))
	{
		return false;
	}

	/*
	 * Lock relation to prevent it from being dropped &
	 * avoid race conditions.
	 */
	Relation rel = safe_relation_open(relationId, AccessShareLock);
	bool result = rel->rd_tableam == GetColumnarTableAmRoutine();
	relation_close(rel, NoLock);

	return result;
}


/*
 * ColumnarAttrNeeded returns a list of AttrNumber's for the ones that are
 * needed during columnar custom scan.
 * Throws an error if finds a Var referencing to an attribute not supported
 * by ColumnarScan.
 */
export Bitmapset *
ColumnarAttrNeeded(ScanState *ss, List *customList)
{
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;
	int natts = slot->tts_tupleDescriptor->natts;
	Bitmapset *attr_needed = NULL;
	Plan *plan = ss->ps.plan;
	int flags = PVC_RECURSE_AGGREGATES |
				PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
	List *vars = list_concat(pull_var_clause((Node *) plan->targetlist, flags),
							 pull_var_clause((Node *) plan->qual, flags));

	if (customList != NULL)
		vars = list_concat(vars, pull_var_clause((Node *)customList, flags));

	//ListCell *lc;
	//foreach(lc, vars)
	for(auto var : PList<Var>(vars))
	{
		//Var *var = lfirst(lc);

		/* 
		 * We support following special variables with update / delete.
		 */
		if (var->varattno == SelfItemPointerAttributeNumber ||
			var->varattno == TableOidAttributeNumber)
		{
			continue;
		}
		else if (var->varattno < SelfItemPointerAttributeNumber)
		{
			ereport(ERROR, 
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("MIN / MAX TransactionID or CommandID not supported for BioseqScan")));
		}

		if (var->varattno == 0 )
		{
			/* all attributes are required, we don't need to add more so break*/
			attr_needed = bms_add_range(attr_needed, 0, natts - 1);
			break;
		}

		attr_needed = bms_add_member(attr_needed, var->varattno - 1);
	}

	return attr_needed;
}

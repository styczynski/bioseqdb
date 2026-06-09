/*-------------------------------------------------------------------------
 *
 * table/utils.cppm
 *
 * Small helpers shared between the bioseq custom scan, index scan and
 * table access method modules. IsColumnarTableAmTable identifies bioseq
 * tables by their TableAmRoutine pointer; ColumnarAttrNeeded scans a
 * plan node's targetlist+qual to compute the bitmap of attributes that
 * the columnar reader actually has to materialise.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include <cmath>

#include "columnar/columnar.h"
#include "columnar/columnar_tableam.h"

#include "columnar/vectorization/columnar_vector_types.h"

export module bioseq_table:utils;
// Minimal import set (DOC0283). This TU only references `enable_logging`
// (mlogging, reached through the bioseq_log<DBG> macro) and the PBitmapset / PList /
// BioseqReport wrappers (pg_utils, via its re-exported :bitmapset / :lists /
// :logging partitions). The columnar I/O, physical-storage, metadata and
// table-types modules it used to import are entirely unreferenced in the body
// below; dropping them cuts this TU's transitive BMI fan-out from 15 to ~6,
// which lowers its peak compile RSS on RAM-limited hosts. Every other symbol
// here (Oid, Relation, safe_relation_open, ScanState, List, Var, Bitmapset,
// pull_var_clause, the PVC_* / *AttributeNumber / ERRCODE_* macros, …) comes
// from the Postgres headers included in the global module fragment above, so
// it is independent of these imports.
import mlogging;
import pg_utils;

const TableAmRoutine * GetColumnarTableAmRoutine(void);

/*
 * IsColumnarTableAmTable returns true if relation has bioseq_tableam
 * access method. This can be called before extension creation.
 *
 * `extern "C++"` attaches the symbol to the global module fragment
 * so cross-module callers that can't `import bioseq_table` (e.g.
 * bioseq_sidecar, which would otherwise form a circular dependency)
 * can forward-declare and link against the same definition.
 */
export extern "C++" bool
IsColumnarTableAmTable(Oid relationId)
{
	bioseq_log<DBG>("[bioseq] pg: IsColumnarTableAmTable() entered");
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
export [[nodiscard]] Bitmapset *
ColumnarAttrNeeded(ScanState *ss, List *customList)
{
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;
	int natts = slot->tts_tupleDescriptor->natts;
	PBitmapset attr_needed;
	Plan *plan = ss->ps.plan;
	int flags = PVC_RECURSE_AGGREGATES |
				PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
	List *vars = list_concat(pull_var_clause(reinterpret_cast<Node *>(plan->targetlist), flags),
							 pull_var_clause(reinterpret_cast<Node *>(plan->qual), flags));

	if (customList != NULL)
		vars = list_concat(vars, pull_var_clause(reinterpret_cast<Node *>(customList), flags));

	for (auto var : PList<Var>(vars))
	{
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
			bioseq_report<ERR, PG>()
				.code(ERRCODE_FEATURE_NOT_SUPPORTED)
				.msg("MIN / MAX TransactionID or CommandID not supported for BioseqScan");
		}

		if (var->varattno == 0)
		{
			/* all attributes are required, we don't need to add more so break*/
			attr_needed.add_range(0, natts - 1);
			break;
		}

		attr_needed.add(var->varattno - 1);
	}

	return attr_needed.release();
}

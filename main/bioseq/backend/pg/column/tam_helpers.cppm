/*-------------------------------------------------------------------------
 *
 * tam_helpers.cppm
 *
 * Leaf module for the small Bioseq table-AM helper functions that several
 * modules (the scheduler dispatch buffer, the *_pg storage-indexing
 * modules, the extension entry module, and the custom-scan partition)
 * need to reach without importing the full bioseq_table module — which
 * would otherwise create an import cycle.
 *
 * DOC0322 §7.4 — replaces the former `extern "C++"` forward-decl reaches
 * for tid_to_row_number / row_number_to_tid / IsBioseqColumnTableAmTable with
 * a proper exported leaf module. The leaf imports only the lowest-level
 * modules (storage_physical for the row-number constants, pg_utils for
 * relation helpers, mlogging for the error path), so it sits at the very
 * bottom of the import DAG and never feeds back into its importers.
 *
 * The TableAmRoutine identity used by IsBioseqColumnTableAmTable is supplied
 * via dependency inversion: bioseq_tableam_init() (in
 * bioseq_table:access_method) calls register_bioseq_am_routine() with the
 * registered routine pointer, and IsBioseqColumnTableAmTable compares a
 * relation's rd_tableam against it. This keeps the leaf free of any edge
 * back into the table-AM module.
 *
 *-------------------------------------------------------------------------
 */
module;
#include "common.h"

export module bioseq.column.tam;

import bioseq.storage.physical;   // BIOSEQ_COLUMN_INVALID_ROW_NUMBER, BIOSEQ_COLUMN_MAX_ROW_NUMBER,
                           // BIOSEQ_COLUMN_VALID_ITEMPOINTER_OFFSETS
import bioseq.pg_utils;           // safe_relation_open
import bioseq.common.mlogging;           // bioseq_report<ERR, PG> / bioseq_log<DBG>

/*
 * ErrorIfInvalidRowNumber errors out if given rowNumber is invalid. Module
 * internal — only the two tid helpers below call it. (Moved here from
 * access_method.cppm together with the two helpers it guards.)
 */
static void
ErrorIfInvalidRowNumber(uint64_t rowNumber)
{
	if (rowNumber == BIOSEQ_COLUMN_INVALID_ROW_NUMBER)
	{
		/* not expected but be on the safe side */
		bioseq_report<ERR, PG>().code(ERRCODE_INTERNAL_ERROR)
			.msg("unexpected row number for columnar table");
	}
	else if (rowNumber > BIOSEQ_COLUMN_MAX_ROW_NUMBER)
	{
		bioseq_report<ERR, PG>().code(ERRCODE_FEATURE_NOT_SUPPORTED)
			.msg("columnar tables can't have row numbers greater than {}",
				 static_cast<uint64_t>(BIOSEQ_COLUMN_MAX_ROW_NUMBER))
			.hint("Consider using VACUUM FULL for your table");
	}
}


/*
 * row_number_to_tid maps given rowNumber to ItemPointerData.
 */
export ItemPointerData
row_number_to_tid(uint64_t rowNumber)
{
	ErrorIfInvalidRowNumber(rowNumber);

	ItemPointerData tid = { 0 };
	ItemPointerSetBlockNumber(&tid, rowNumber / BIOSEQ_COLUMN_VALID_ITEMPOINTER_OFFSETS);
	ItemPointerSetOffsetNumber(&tid, rowNumber % BIOSEQ_COLUMN_VALID_ITEMPOINTER_OFFSETS +
							   FirstOffsetNumber);
	return tid;
}


/*
 * tid_to_row_number maps given ItemPointerData to rowNumber.
 */
export uint64_t
tid_to_row_number(ItemPointerData tid)
{
	uint64_t rowNumber = ItemPointerGetBlockNumber(&tid) * BIOSEQ_COLUMN_VALID_ITEMPOINTER_OFFSETS +
					   ItemPointerGetOffsetNumber(&tid) - FirstOffsetNumber;

	ErrorIfInvalidRowNumber(rowNumber);

	return rowNumber;
}


/*
 * The registered bioseq TableAmRoutine pointer. Set once at module-init time
 * by bioseq_tableam_init() via register_bioseq_am_routine(). Used by
 * IsBioseqColumnTableAmTable to identify bioseq tables by their AM identity
 * without depending on bioseq_table:access_method (dependency inversion,
 * DOC0322 §7.4).
 */
static const TableAmRoutine* g_bioseq_am_routine = nullptr;

export void
register_bioseq_am_routine(const TableAmRoutine* routine) noexcept
{
	g_bioseq_am_routine = routine;
}


/*
 * IsBioseqColumnTableAmTable returns true if relation has the bioseq table
 * access method. This can be called before extension creation.
 */
export bool
IsBioseqColumnTableAmTable(Oid relationId)
{
	bioseq_log<DBG>("[bioseq] pg: IsBioseqColumnTableAmTable() entered");
	if (!OidIsValid(relationId))
	{
		return false;
	}

	/*
	 * Lock relation to prevent it from being dropped &
	 * avoid race conditions.
	 */
	Relation rel = safe_relation_open(relationId, AccessShareLock);
	bool result = rel->rd_tableam == g_bioseq_am_routine;
	relation_close(rel, NoLock);

	return result;
}

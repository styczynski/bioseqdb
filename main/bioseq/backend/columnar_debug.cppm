/*-------------------------------------------------------------------------
 *
 * columnar_debug.cppm
 *
 * Diagnostic SQL functions for the bioseq columnar extension. Exposes
 * runtime memory usage of the per-context HTABs (write state, row mask,
 * top transaction) and a per-relation storage info SRF that surfaces the
 * metapage version, storage id and reservation counters.
 *
 *-------------------------------------------------------------------------
 */
module;
#include "common.h"

#include "columnar/columnar.h"


export module storage_debug;
import mlogging;
import storage_physical;
import storage_metadata;
import bioseq_table;
import pg_utils;

/*
 * MemoryContextTotals adds stats of the given memory context and its
 * subtree to the given counters.
 */
static void
MemoryContextTotals(MemoryContext context, MemoryContextCounters *counters)
{
	if (context == NULL)
	{
		return;
	}

	MemoryContext child;
	for (child = context->firstchild; child != NULL; child = child->nextchild)
	{
		MemoryContextTotals(child, counters);
	}

	context->methods->stats_compat(context, NULL, NULL, counters, true);
}


/*
 * bioseq_store_memory_stats returns a record of 4 values: size of
 * TopMemoryContext, TopTransactionContext, , Write State context and
 * Row Mask context.
 */
POSTGRES_FN(bioseq_store_memory_stats)
{
	constexpr int resultColumnCount = 4;

	TupleDesc tupleDescriptor = CreateTemplateTupleDesc(resultColumnCount);

	TupleDescInitEntry(tupleDescriptor, static_cast<AttrNumber>(1), "TopMemoryContext",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupleDescriptor, static_cast<AttrNumber>(2), "TopTransactionContext",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupleDescriptor, static_cast<AttrNumber>(3), "WriteStateContext",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupleDescriptor, static_cast<AttrNumber>(4), "RowMaskWriteStateContext",
					   INT8OID, -1, 0);

	tupleDescriptor = BlessTupleDesc(tupleDescriptor);

	MemoryContextCounters transactionCounters = { 0 };
	MemoryContextCounters topCounters = { 0 };
	MemoryContextCounters writeStateCounters = { 0 };
	MemoryContextCounters rowMaskCacheCounters = { 0 };
	MemoryContextTotals(TopTransactionContext, &transactionCounters);
	MemoryContextTotals(TopMemoryContext, &topCounters);
	MemoryContextTotals(GetColumnarWriteContextForDebug(), &writeStateCounters);
	MemoryContextTotals(GetRowMaskWriteStateContextForDebug(), &rowMaskCacheCounters);

	bool nulls[4] = { false };
	Datum values[4] = {
		Int64GetDatum(topCounters.totalspace),
		Int64GetDatum(transactionCounters.totalspace),
		Int64GetDatum(writeStateCounters.totalspace),
		Int64GetDatum(rowMaskCacheCounters.totalspace)
	};

	HeapTuple tuple = heap_form_tuple(tupleDescriptor, values, nulls);

	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}


/*
 * bioseq_storage_info - UDF to return internal storage info for a columnar relation.
 *
 * DDL:
 *  CREATE OR REPLACE FUNCTION bioseq_storage_info(
 *      rel regclass,
 *      version_major OUT int4,
 *      version_minor OUT int4,
 *      storage_id OUT int8,
 *      reserved_stripe_id OUT int8,
 *      reserved_row_number OUT int8,
 *      reserved_offset OUT int8)
 *    STRICT
 *    LANGUAGE c AS 'MODULE_PATHNAME', 'bioseq_storage_info';
 */
POSTGRES_FN(bioseq_storage_info)
{
	constexpr int STORAGE_INFO_NATTS = 6;
	Oid relid = PG_GETARG_OID(0);
	TupleDesc tupdesc;

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
	{
		bioseq_log<ERR, PG>( "return type must be a row type");
	}

	if (tupdesc->natts != STORAGE_INFO_NATTS)
	{
		bioseq_log<ERR, PG>( "return type must have {} columns", STORAGE_INFO_NATTS);
	}

	Relation rel = table_open(relid, AccessShareLock);
	if (!IsColumnarTableAmTable(relid))
	{
		bioseq_log<ERR, PG>( "table \"{}\" is not a columnar table",
			   RelationGetRelationName(rel));
	}

	Datum values[STORAGE_INFO_NATTS] = { 0 };
	bool nulls[STORAGE_INFO_NATTS] = { 0 };

	/*
	 * Pass force = true so that we can inspect metapages that are not the
	 * current version.
	 *
	 * NB: ensure the order and number of attributes correspond to DDL
	 * declaration.
	 */
	values[0] = Int32GetDatum(ColumnarStorageGetVersionMajor(rel, true));
	values[1] = Int32GetDatum(ColumnarStorageGetVersionMinor(rel, true));
	values[2] = Int64GetDatum(ColumnarStorageGetStorageId(rel, true));
	values[3] = Int64GetDatum(ColumnarStorageGetReservedStripeId(rel, true));
	values[4] = Int64GetDatum(ColumnarStorageGetReservedRowNumber(rel, true));
	values[5] = Int64GetDatum(ColumnarStorageGetReservedOffset(rel, true));

	/* release lock */
	table_close(rel, AccessShareLock);

	HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);

	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

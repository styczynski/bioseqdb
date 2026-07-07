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



export module bioseq.storage.debug;
import bioseq.common.mlogging;
import bioseq.storage.physical;
import bioseq.storage.metadata;
import bioseq.column.tam;   // IsBioseqColumnTableAmTable (moved out of bioseq_table:utils, DOC0322 §7.4)
import bioseq.pg_utils;

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
	MemoryContextTotals(GetBioseqColumnWriteContextForDebug(), &writeStateCounters);
	MemoryContextTotals(GetRowMaskWriteStateContextForDebug(), &rowMaskCacheCounters);

	bioseq::pg_utils::pg_datum_list<4> values{};
	values.set(1, static_cast<std::int64_t>(topCounters.totalspace));
	values.set(2, static_cast<std::int64_t>(transactionCounters.totalspace));
	values.set(3, static_cast<std::int64_t>(writeStateCounters.totalspace));
	values.set(4, static_cast<std::int64_t>(rowMaskCacheCounters.totalspace));

	HeapTuple tuple = values.form_tuple(tupleDescriptor);

	PG_RETURN_DATUM(bioseq::pg_utils::pg_datum::from_heap_tuple(tuple).raw());
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
	if (!IsBioseqColumnTableAmTable(relid))
	{
		bioseq_log<ERR, PG>( "table \"{}\" is not a columnar table",
			   RelationGetRelationName(rel));
	}

	bioseq::pg_utils::pg_datum_list<STORAGE_INFO_NATTS> values{};

	/*
	 * Pass force = true so that we can inspect metapages that are not the
	 * current version.
	 *
	 * NB: ensure the order and number of attributes correspond to DDL
	 * declaration.
	 */
	values.set(1, static_cast<std::int32_t>(BioseqColumnStorageGetVersionMajor(rel, true)));
	values.set(2, static_cast<std::int32_t>(BioseqColumnStorageGetVersionMinor(rel, true)));
	values.set(3, static_cast<std::int64_t>(BioseqColumnStorageGetStorageId(rel, true)));
	values.set(4, static_cast<std::int64_t>(BioseqColumnStorageGetReservedStripeId(rel, true)));
	values.set(5, static_cast<std::int64_t>(BioseqColumnStorageGetReservedRowNumber(rel, true)));
	values.set(6, static_cast<std::int64_t>(BioseqColumnStorageGetReservedOffset(rel, true)));

	/* release lock */
	table_close(rel, AccessShareLock);

	HeapTuple tuple = values.form_tuple(tupdesc);

	PG_RETURN_DATUM(bioseq::pg_utils::pg_datum::from_heap_tuple(tuple).raw());
}

/*-------------------------------------------------------------------------
 *
 * table/access_method.cppm
 *
 * Bioseq table access method (TAM) implementation. Provides the
 * TableAmRoutine entry points (beginscan/endscan/getnextslot, tuple
 * insert/update/delete, index_fetch_*, vacuum, analyze, etc.) that
 * PostgreSQL needs to treat a bioseq columnar relation like any other
 * table. Internally this routes the work to the columnar reader/
 * writer/metadata modules and the in-memory column cache.
 *
 *-------------------------------------------------------------------------
 */
module;
#include "common.h"

#include <cmath>
#include <algorithm>
#include <cstring>
#include <new>
#include <optional>
#include <vector>

#include "columnar/columnar.h"
#include "columnar/columnar_tableam.h"

#include "columnar/vectorization/columnar_vector_types.h"

/*
 * Timing parameters for truncate locking heuristics.
 *
 * These are the same values from src/backend/access/heap/vacuumlazy.c
 */
#define VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL 50       /* ms */
#define VACUUM_TRUNCATE_LOCK_TIMEOUT 4500               /* ms */

#include <signal.h>
using sact = struct sigaction;

export module bioseq_table:access_method;

import storage_physical;
import mlogging;
import storage_metadata;
import bioseq_table_types;
import config;
import bioseq_table_io;
import pg_utils;
import bioseq_storage_indexing_admin;   // DOC0279 §11.2 — index_build_on_insert
import bioseq_storage;
import bioseq_am_cache;
import bioseq_types.pg_compact_sequence;

/*
 * This is a check whether we need to bail out of a vacuum, it is set by the
 * signal handler, and checked by the vacuum UDF process.
 */
static bool need_to_bail = false;
static int last_signal = 0;
sact abt_action = {};
sact int_action = {};
sact trm_action = {};
sact kil_action = {};

extern "C++" uint64_t ColumnarTableRowCount(Relation relation);
void bioseq_customscan_init();
const TableAmRoutine * GetColumnarTableAmRoutine(void);
bool ColumnarSupportsIndexAM(char *indexAMName);
extern "C++" bool IsColumnarTableAmTable(Oid relationId);
ItemPointerData row_number_to_tid(uint64_t rowNumber);

extern "C++" TableScanDesc
bioseq_beginscan_extended(Relation relation, Snapshot snapshot,
							int nkeys, ScanKey key,
							ParallelTableScanDesc parallel_scan,
							uint32_t flags, Bitmapset *attr_needed, List *scanQual,
							ParallelColumnarScan parallelColumnarScan,
							bool returnVectorizedTuple);

/* DOC0260 Phase A — same as bioseq_beginscan_extended, plus an explicit
 * `defer_seq_hydration` switch. Used by the custom scan / dispatch
 * buffer code paths that hydrate per-bucket later. */
TableScanDesc
bioseq_beginscan_with_options(Relation relation, Snapshot snapshot,
							int nkeys, ScanKey key,
							ParallelTableScanDesc parallel_scan,
							uint32_t flags, Bitmapset *attr_needed, List *scanQual,
							ParallelColumnarScan parallelColumnarScan,
							bool returnVectorizedTuple,
							bool defer_seq_hydration);

/* available to other extensions using find_rendezvous_variable() */
static ColumnarTableSetOptions_hook_type ColumnarTableSetOptions_hook = NULL;

static object_access_hook_type PrevObjectAccessHook = NULL;
static ProcessUtility_hook_type PrevProcessUtilityHook = NULL;

/* forward declaration for static functions */
static MemoryContext CreateColumnarScanMemoryContext(void);
static void ColumnarTableDropHook(Oid tgid);
static void ColumnarTriggerCreateHook(Oid tgid);
static void ColumnarTableAMObjectAccessHook(ObjectAccessType access, Oid classId,
											Oid objectId, int subId,
											void *arg);
static void ColumnarProcessUtility(PlannedStmt *pstmt,
								   const char *queryString,
								   bool readOnlyTree,
								   ProcessUtilityContext context,
								   ParamListInfo params,
								   QueryEnvironment *queryEnv,
								   DestReceiver *dest,
								   QueryCompletionCompat *completionTag);
static bool ConditionalLockRelationWithTimeout(Relation rel, LOCKMODE lockMode,
											   int timeout, int retryInterval,
											   bool acquire);
[[nodiscard]] static List * NeededColumnsList(TupleDesc tupdesc, const PBitmapset& attr_needed);
static void LogRelationStats(Relation rel, int elevel);
static void TruncateColumnar(Relation rel, int elevel);
static bool TruncateAndCombineColumnarStripes(Relation rel, int elevel);
static HeapTuple ColumnarSlotCopyHeapTuple(TupleTableSlot *slot);
static void ColumnarCheckLogicalReplication(Relation rel);
static Datum * detoast_values(TupleDesc tupleDesc, Datum *orig_values, bool *isnull);
extern "C++" uint64_t tid_to_row_number(ItemPointerData tid);
static void ErrorIfInvalidRowNumber(uint64_t rowNumber);
static void ColumnarReportTotalVirtualBlocks(Relation relation, Snapshot snapshot,
											 int progressArrIndex);
static BlockNumber ColumnarGetNumberOfVirtualBlocks(Relation relation, Snapshot snapshot);
static ItemPointerData ColumnarGetHighestItemPointer(Relation relation,
													 Snapshot snapshot);
static double ColumnarReadRowsIntoIndex(TableScanDesc scan,
										Relation indexRelation,
										IndexInfo *indexInfo,
										bool progress,
										IndexBuildCallback indexCallback,
										void *indexCallbackState,
										EState *estate, ExprState *predicate);
static void ColumnarReadMissingRowsIntoIndex(TableScanDesc scan, Relation indexRelation,
											 IndexInfo *indexInfo, EState *estate,
											 ExprState *predicate,
											 ValidateIndexState *state);
static ItemPointerData TupleSortSkipSmallerItemPointers(Tuplesortstate *tupleSort,
														ItemPointer targetItemPointer);


/* Custom tuple slot ops used for bioseq. Initialized in bioseq_tableam_init(). */
static TupleTableSlotOps TTSOpsColumnar;

/*
 * File-static holder for the page-cache flag value captured when a
 * scan/DML callback first disables the cache. Not convertible to a
 * PageCacheDisableScope guard because the save and restore happen in
 * *different* TableAM callbacks (begin/end for scans; insert/complete
 * for speculative inserts), so the lifetime cannot be expressed as a
 * single C++ scope. The global slot means concurrent scans in the same
 * backend would overwrite each other's saved state -- currently the
 * executor serialises these callbacks per-backend so that is fine, but
 * adding a second slot would require threading the previous value
 * through the per-scan descriptor.
 */
static bool previousCacheEnabledState = false;

static const TupleTableSlotOps *
bioseq_slot_callbacks(Relation relation)
{
	return &TTSOpsColumnar;
}


static TableScanDesc
bioseq_beginscan(Relation relation, Snapshot snapshot,
				   int nkeys, ScanKey key,
				   ParallelTableScanDesc parallel_scan,
				   uint32_t flags)
{
	previousCacheEnabledState = bioseq_enable_page_cache;

	int natts = relation->rd_att->natts;

	/* attr_needed represents 0-indexed attribute numbers */
	PBitmapset attr_needed = PBitmapset::full_range(natts);

	TableScanDesc scandesc = bioseq_beginscan_extended(relation, snapshot, nkeys, key,
														 parallel_scan,
														 flags, attr_needed, NULL,
														 NULL,
														 false);

	attr_needed.free();

	return scandesc;
}


extern "C++" TableScanDesc
bioseq_beginscan_extended(Relation relation, Snapshot snapshot,
							int nkeys, ScanKey key,
							ParallelTableScanDesc parallel_scan,
							uint32_t flags, Bitmapset *attr_needed, List *scanQual,
							ParallelColumnarScan parallelColumnarScan,
							bool returnVectorizedTuple)
{
	return bioseq_beginscan_with_options(relation, snapshot, nkeys, key,
		parallel_scan, flags, attr_needed, scanQual, parallelColumnarScan,
		returnVectorizedTuple, /*defer_seq_hydration=*/false);
}

/*
 * DOC0260 §8/§9 — the custom scan's deferred-hydration entry point. Same
 * signature as bioseq_beginscan_extended but turns the deferred-hydration
 * flag on, so bioseq_getnextslot returns locator stubs verbatim and the
 * dispatch buffer hydrates them in per-bucket bulk. extern "C++" (global
 * module fragment) so columnar_customscan — not a partition of
 * bioseq_table — can forward-declare and link against it.
 */
extern "C++" TableScanDesc
bioseq_beginscan_deferred(Relation relation, Snapshot snapshot,
							int nkeys, ScanKey key,
							ParallelTableScanDesc parallel_scan,
							uint32_t flags, Bitmapset *attr_needed, List *scanQual,
							ParallelColumnarScan parallelColumnarScan,
							bool returnVectorizedTuple)
{
	return bioseq_beginscan_with_options(relation, snapshot, nkeys, key,
		parallel_scan, flags, attr_needed, scanQual, parallelColumnarScan,
		returnVectorizedTuple, /*defer_seq_hydration=*/true);
}

extern "C" TableScanDesc
bioseq_beginscan_extended_c(Relation relation, Snapshot snapshot,
							int nkeys, ScanKey key,
							ParallelTableScanDesc parallel_scan,
							uint32_t flags, Bitmapset *attr_needed, List *scanQual,
							ParallelColumnarScan parallelColumnarScan,
							bool returnVectorizedTuple)
{
	return bioseq_beginscan_with_options(relation, snapshot, nkeys, key,
		parallel_scan, flags, attr_needed, scanQual, parallelColumnarScan,
		returnVectorizedTuple, /*defer_seq_hydration=*/false);
}

extern "C" TableScanDesc
bioseq_beginscan_deferred_c(Relation relation, Snapshot snapshot,
							int nkeys, ScanKey key,
							ParallelTableScanDesc parallel_scan,
							uint32_t flags, Bitmapset *attr_needed, List *scanQual,
							ParallelColumnarScan parallelColumnarScan,
							bool returnVectorizedTuple)
{
	return bioseq_beginscan_with_options(relation, snapshot, nkeys, key,
		parallel_scan, flags, attr_needed, scanQual, parallelColumnarScan,
		returnVectorizedTuple, /*defer_seq_hydration=*/true);
}

TableScanDesc
bioseq_beginscan_with_options(Relation relation, Snapshot snapshot,
							int nkeys, ScanKey key,
							ParallelTableScanDesc parallel_scan,
							uint32_t flags, Bitmapset *attr_needed, List *scanQual,
							ParallelColumnarScan parallelColumnarScan,
							bool returnVectorizedTuple,
							bool defer_seq_hydration)
{
	previousCacheEnabledState = bioseq_enable_page_cache;
	Oid relfilelocator = relation->rd_locator.relNumber;
	/*
	 * A memory context to use for scan-wide data, including the lazily
	 * initialized read state. We assume that beginscan is called in a
	 * context that will last until end of scan.
	 */
	MemoryContext scanContext = CreateColumnarScanMemoryContext();
	ColumnarScanDesc scan;
	{
		MemoryContextScope ctx_guard{scanContext};

		scan = ::new (palloc0(sizeof(ColumnarScanDescData))) ColumnarScanDescData{};
		scan->cs_base.rs_rd = relation;
		scan->cs_base.rs_snapshot = snapshot;
		scan->cs_base.rs_nkeys = nkeys;
		scan->cs_base.rs_key = key;
		scan->cs_base.rs_flags = flags;
		scan->cs_base.rs_parallel = parallel_scan;

		/*
		 * We will initialize this lazily in first tuple, where we have the actual
		 * tuple descriptor to use for reading. In some cases like ALTER TABLE ...
		 * ALTER COLUMN ... TYPE, the tuple descriptor of relation doesn't match
		 * the storage which we are reading, so we need to use the tuple descriptor
		 * of "slot" in first read.
		 */
		scan->cs_readState = NULL;
		scan->attr_needed = bms_copy(attr_needed);
		scan->scanQual = copyObject(scanQual);
		scan->scanContext = scanContext;

		/* Parallel execution scan data */;
		scan->parallelColumnarScan = parallelColumnarScan;

		/* Vectorized result */
		scan->returnVectorizedTuple = returnVectorizedTuple;

		/* DOC0260 Phase A — per-scan storage cache and deferred-hydration flag. */
		scan->defer_seq_hydration = defer_seq_hydration;
		auto* caches = bioseq::storage::alloc_seq_caches(scanContext);
		caches->side = bioseq::storage::seq_caches::Side::Read;
		caches->lock_mode = AccessShareLock;
		caches->read_state_mctx = scanContext;
		caches->main_database_oid   = relation->rd_locator.dbOid;
		caches->main_tablespace_oid = relation->rd_locator.spcOid;
		caches->main_relfilenode    = relation->rd_locator.relNumber;
		scan->seq_cache = caches;

		if (PendingWritesInUpperTransactions(relfilelocator, GetCurrentSubTransactionId()))
		{
			bioseq_log<ERR, PG>(
				 "cannot read from table when there is unflushed data in upper transactions");
		}
	}

	return reinterpret_cast<TableScanDesc>(scan);
}

/*
 * bioseq_relation_storageid returns storage id associated with the
 * given relation id, or -1 if there is no associated storage id yet.
 */
POSTGRES_FN(bioseq_relation_storageid)
{
	Oid relationId = PG_GETARG_OID(0);
	Relation relation = safe_relation_open(relationId, AccessShareLock);
	if (!IsColumnarTableAmTable(relationId))
	{
		bioseq_log<ERR, PG>( "relation \"{}\" is not a columnar table",
			 RelationGetRelationName(relation));
	}

	uint64_t storageId = ColumnarStorageGetStorageId(relation, false);

	relation_close(relation, AccessShareLock);

	PG_RETURN_INT64(storageId);
}

/*
 * create_table_row_mask creates empty row mask for table
 */
POSTGRES_FN(create_table_row_mask)
{
	Oid relationId = PG_GETARG_OID(0);

	Relation relation = safe_relation_open(relationId, AccessShareLock);

	if (!IsColumnarTableAmTable(relationId))
	{
		bioseq_log<ERR, PG>( "relation \"{}\" is not a columnar table",
			 RelationGetRelationName(relation));
	}

	uint64_t storageId = ColumnarStorageGetStorageId(relation, false);

	ScanKeyData scanKey[1];
	StripeMetadata *stripeMetadata = NULL;

	ScanKeyInit(&scanKey[0], Anum_bioseq_stripe_storageid,
				BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(storageId));

	Relation columnarStripes = table_open(ColumnarStripeRelationId(), AccessShareLock);
	
	Relation index = index_open(ColumnarStripePKeyIndexRelationId(),
								AccessShareLock);

	SysScanDesc scanDescriptor = systable_beginscan_ordered(columnarStripes, index,
															SnapshotSelf, 1, scanKey);

	HeapTuple heapTuple = NULL;

	bool created = true;

	while (HeapTupleIsValid(heapTuple = systable_getnext_ordered(scanDescriptor,
																 ForwardScanDirection)))
	{
		stripeMetadata = BuildStripeMetadata(columnarStripes, heapTuple);

		List *chunkGroupRowCount = NIL;

		int64_t lastChunkRowCount = stripeMetadata->rowCount % stripeMetadata->chunkGroupRowCount ?
									stripeMetadata->rowCount % stripeMetadata->chunkGroupRowCount :
									stripeMetadata->chunkGroupRowCount;

		for (int i = 0; i < stripeMetadata->chunkCount - 1; i++)
		{
			chunkGroupRowCount = 
				lappend_int(chunkGroupRowCount, stripeMetadata->chunkGroupRowCount);
		}

		chunkGroupRowCount = 
				lappend_int(chunkGroupRowCount, lastChunkRowCount);

		if(!SaveEmptyRowMask(storageId, stripeMetadata->id,
							 stripeMetadata->firstRowNumber, chunkGroupRowCount))
		{
			bioseq_log<WRN, PG>( "relation \"{}\" already has bioseq.row_mask populated.",
				 RelationGetRelationName(relation));
			created = false;
			break;
		}
	}

	systable_endscan_ordered(scanDescriptor);
	index_close(index, AccessShareLock);
	table_close(columnarStripes, AccessShareLock);
	relation_close(relation, AccessShareLock);

	PG_RETURN_BOOL(created);
}

/*
 * CreateColumnarScanMemoryContext creates a memory context to store
 * ColumnarReadStare in it.
 */
static MemoryContext
CreateColumnarScanMemoryContext(void)
{
	return AllocSetContextCreate(CurrentMemoryContext, "Columnar Scan Context",
								 ALLOCSET_DEFAULT_SIZES);
}


/*
 * init_bioseq_read_state initializes a column store table read and returns the
 * state.
 */
static ColumnarReadState *
init_bioseq_read_state(Relation relation, TupleDesc tupdesc, Bitmapset *attr_needed,
						 List *scanQual, MemoryContext scanContext, Snapshot snapshot,
						 bool randomAccess,
						 ParallelColumnarScan parallelColumnarScan)
{
	MemoryContextScope ctx_guard{scanContext};

	List *neededColumnList = NeededColumnsList(tupdesc, attr_needed);
	ColumnarReadState *readState = ColumnarBeginRead(relation, tupdesc, neededColumnList,
													 scanQual, scanContext, snapshot,
													 randomAccess,
													 parallelColumnarScan);

	return readState;
}


static void
bioseq_endscan(TableScanDesc sscan)
{
	ColumnarScanDesc scan = reinterpret_cast<ColumnarScanDesc>(sscan);

	/* DOC0260 Phase A — close per-scan seq_cache (releases relation pins,
	 * tears down any cached ColumnarReadStates on sub-tables). */
	if (scan->seq_cache != NULL) {
		auto* caches = static_cast<bioseq::storage::seq_caches*>(scan->seq_cache);
		caches->close_all(/*commit=*/true);
		bioseq::storage::destroy_seq_caches(caches);
		scan->seq_cache = NULL;
	}

	if (scan->cs_readState != NULL)
	{
		ColumnarEndRead(scan->cs_readState);
		scan->cs_readState = NULL;
	}

	if (scan->cs_base.rs_flags & SO_TEMP_SNAPSHOT)
	{
		UnregisterSnapshot(scan->cs_base.rs_snapshot);
	}

	/* clean up any caches. */
	if (bioseq_enable_page_cache == true)
	{
		ColumnarResetCache();
	}

	MemoryContextDelete(scan->scanContext);

	bioseq_enable_page_cache = previousCacheEnabledState;
}


static void
bioseq_rescan(TableScanDesc sscan, ScanKey key, bool set_params,
				bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	ColumnarScanDesc scan = reinterpret_cast<ColumnarScanDesc>(sscan);

	/* XXX: hack to pass in new quals that aren't actually scan keys */
	List *scanQual = reinterpret_cast<List *>(key);

	if (scan->cs_readState != NULL)
	{
		ColumnarRescan(scan->cs_readState, scanQual);
	}
}


/*
 * DOC0260 Phase A — walk every SEQ column on `slot`, decode the locator
 * stub (if present), and replace tts_values[idx] with the hydrated
 * pgseq_t* fetched from the matching sub-table shard. Skips non-stub
 * values (already-inline payloads) and NULLs. The caller must run with a
 * memory context where the hydrated allocations are legal — for
 * bioseq_getnextslot this is the scan's scanContext; for
 * bioseq_index_fetch_tuple the per-fetch scanContext.
 */
[[gnu::always_inline]]
static inline void
hydrate_seq_columns_in_slot(Relation main_rel, TupleTableSlot* slot,
                            bioseq::storage::seq_caches* cache,
                            const bioseq::storage::BioseqAmCache& amc,
                            MemoryContext payload_mctx)
{
	if (cache == nullptr || amc.seq_attnos.empty()) {
		return;
	}
	const RelFileLocator& loc = main_rel->rd_locator;
	for (std::int16_t attno : amc.seq_attnos) {
		const int idx = attno - 1;
		if (idx < 0 || idx >= slot->tts_tupleDescriptor->natts) continue;
		if (slot->tts_isnull[idx]) continue;
		auto* hdr = reinterpret_cast<const pgseq_t*>(
			DatumGetPointer(slot->tts_values[idx]));
		if (!bioseq::storage::is_locator_stub(hdr)) continue;
		const auto loc_decoded = bioseq::storage::decode_locator_from_stub(hdr);
		// DOC0264 §9.4 — guard against materialising sequences whose
		// stitched varlena would exceed MaxAllocSize. Phase D D-VARLENA
		// will replace this with a streaming SELECT/seq_send path.
		const std::uint64_t projected_payload_bytes =
			(loc_decoded.total_bp_size * /*bits_per_letter min=*/2u + 7u) / 8u;
		const std::uint64_t projected_varlena =
			static_cast<std::uint64_t>(pgseq_t::header_bytes())
			+ projected_payload_bytes;
		if (projected_varlena > static_cast<std::uint64_t>(MaxAllocSize)) {
			bioseq_log<ERR, PG>(
			       "bioseq storage Phase C: sequence too large ({} bp ⇒ ~{} bytes) "
			       "to materialise as a single varlena. SELECT seq / pg_dump "
			       "cannot return this row directly. Use bioseq.substring(seq, …) "
			       "to project a sub-region, or wait for Phase D D-VARLENA. "
			       "See DOC0267.",
			       loc_decoded.total_bp_size, projected_varlena);
		}
		pgseq_t* real = bioseq::storage::bioseq_default_storage_manager::fetch_payload(
			loc.dbOid, loc.spcOid, loc.relNumber,
			attno, loc_decoded, payload_mctx, cache);
		slot->tts_values[idx] = PointerGetDatum(real);
	}
}

/*
 * DOC0260 Phase A — hydrate SEQ-column stubs on a scalar slot produced by
 * the *vectorized* scan path. The scalar getnextslot path hydrates inline
 * (see bioseq_getnextslot below), but when returnVectorizedTuple is set the
 * TAM fills a vector slot without hydration; the custom scan then extracts
 * scalar tuples via ExtractTupleFromVectorSlot, which carry raw 40-byte
 * locator stubs. This wrapper lets columnar_customscan hydrate that extracted
 * slot using the scan's own per-scan seq_cache / scanContext, so a projected
 * (or join-Param-bound) seq value is the real payload rather than a stub.
 *
 * Wrapped in `extern "C++"` (global module fragment) so the custom scan —
 * which is not a partition of bioseq_table — can forward-declare and link
 * against it without importing the full table-AM module, exactly like
 * tid_to_row_number below.
 */
extern "C++" void
bioseq_hydrate_scan_slot(TableScanDesc sscan, TupleTableSlot* slot, bool force)
{
	ColumnarScanDesc scan = reinterpret_cast<ColumnarScanDesc>(sscan);
	if (scan == nullptr) {
		return;
	}
	/*
	 * `force` overrides the defer flag: the §9 custom scan runs with
	 * defer_seq_hydration=true (so the mmsearch batch path can capture
	 * locators), but its non-batched projecting branch still needs the
	 * slot hydrated before ExecProject — it passes force=true. Callers on
	 * the inline path pass force=false and become a no-op when deferring.
	 */
	if (!force && scan->defer_seq_hydration) {
		return;
	}
	const auto& amc = bioseq::storage::bioseq_am_cache_for(scan->cs_base.rs_rd);
	auto* caches = static_cast<bioseq::storage::seq_caches*>(scan->seq_cache);
	hydrate_seq_columns_in_slot(scan->cs_base.rs_rd, slot, caches, amc,
	                            scan->scanContext);
}

static bool
bioseq_getnextslot(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot)
{
	ColumnarScanDesc scan = reinterpret_cast<ColumnarScanDesc>(sscan);

	/*
	 * if this is the first row, initialize read state.
	 */
	if (scan->cs_readState == NULL)
	{
		bool randomAccess = false;
		scan->cs_readState =
			init_bioseq_read_state(scan->cs_base.rs_rd, slot->tts_tupleDescriptor,
									 scan->attr_needed, scan->scanQual,
									 scan->scanContext, scan->cs_base.rs_snapshot,
									 randomAccess,
									 scan->parallelColumnarScan);
	}

	ExecClearTuple(slot);

	if (scan->returnVectorizedTuple)
	{
		VectorTupleTableSlot * vectorTTS = reinterpret_cast<VectorTupleTableSlot *>(slot);

		int newVectorSize = 0;

		bool nextRowFound = ColumnarReadNextVector(scan->cs_readState,
												   vectorTTS->tts.tts_values,
												   vectorTTS->tts.tts_isnull,
												   vectorTTS->rowNumber,
												   &newVectorSize);

		if (!nextRowFound)
			return false;

		vectorTTS->dimension = newVectorSize;

		memset(vectorTTS->keep, true, newVectorSize);

		ExecStoreVirtualTuple(slot);
	}
	else
	{
		uint64_t rowNumber;
		bool nextRowFound = ColumnarReadNextRow(scan->cs_readState, slot->tts_values,
												slot->tts_isnull, &rowNumber);

		if (!nextRowFound)
		{
			return false;
		}

		ExecStoreVirtualTuple(slot);
		slot->tts_tid = row_number_to_tid(rowNumber);

		/*
		 * DOC0260 Phase A — hydrate every SEQ column unless the caller
		 * asked for deferred hydration (custom scan / dispatch buffer
		 * path, which performs per-bucket bulk hydration later).
		 */
		if (!scan->defer_seq_hydration)
		{
			const auto& amc = bioseq::storage::bioseq_am_cache_for(scan->cs_base.rs_rd);
			auto* caches = static_cast<bioseq::storage::seq_caches*>(scan->seq_cache);
			hydrate_seq_columns_in_slot(scan->cs_base.rs_rd, slot, caches, amc,
			                            scan->scanContext);
		}
	}

	return true;
}


/*
 * row_number_to_tid maps given rowNumber to ItemPointerData.
 */
ItemPointerData
row_number_to_tid(uint64_t rowNumber)
{
	ErrorIfInvalidRowNumber(rowNumber);

	ItemPointerData tid = { 0 };
	ItemPointerSetBlockNumber(&tid, rowNumber / VALID_ITEMPOINTER_OFFSETS);
	ItemPointerSetOffsetNumber(&tid, rowNumber % VALID_ITEMPOINTER_OFFSETS +
							   FirstOffsetNumber);
	return tid;
}


/*
 * tid_to_row_number maps given ItemPointerData to rowNumber.
 *
 * Wrapped in `extern "C++"` so the symbol is attached to the global
 * module fragment rather than `bioseq_table:access_method`. Scheduler
 * and scheduler-adjacent callers (dispatch_buffer.cppm, not a partition
 * of bioseq_table) can then forward-declare and link against it
 * without importing the full table-AM module.
 */
extern "C++" uint64_t
tid_to_row_number(ItemPointerData tid)
{
	uint64_t rowNumber = ItemPointerGetBlockNumber(&tid) * VALID_ITEMPOINTER_OFFSETS +
					   ItemPointerGetOffsetNumber(&tid) - FirstOffsetNumber;

	ErrorIfInvalidRowNumber(rowNumber);

	return rowNumber;
}


/*
 * ErrorIfInvalidRowNumber errors out if given rowNumber is invalid.
 */
static void
ErrorIfInvalidRowNumber(uint64_t rowNumber)
{
	if (rowNumber == COLUMNAR_INVALID_ROW_NUMBER)
	{
		/* not expected but be on the safe side */
		bioseq_report<ERR, PG>().code(ERRCODE_INTERNAL_ERROR)
			.msg("unexpected row number for columnar table");
	}
	else if (rowNumber > COLUMNAR_MAX_ROW_NUMBER)
	{
		bioseq_report<ERR, PG>().code(ERRCODE_FEATURE_NOT_SUPPORTED)
			.msg("columnar tables can't have row numbers greater than {}",
				 static_cast<uint64_t>(COLUMNAR_MAX_ROW_NUMBER))
			.hint("Consider using VACUUM FULL for your table");
	}
}


static Size
bioseq_parallelscan_estimate(Relation rel)
{
	bioseq_log<ERR, PG>( "bioseq_parallelscan_estimate not implemented");
}


static Size
bioseq_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
	bioseq_log<ERR, PG>( "bioseq_parallelscan_initialize not implemented");
}


static void
bioseq_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
	bioseq_log<ERR, PG>( "bioseq_parallelscan_reinitialize not implemented");
}


[[gnu::always_inline]]
static inline void
init_index_fetch_seq_cache(IndexFetchColumnarData *scan, Relation rel,
                           MemoryContext scanContext)
{
	auto *caches = bioseq::storage::alloc_seq_caches(scanContext);
	caches->side = bioseq::storage::seq_caches::Side::Read;
	caches->lock_mode = AccessShareLock;
	caches->read_state_mctx = scanContext;
	caches->main_database_oid   = rel->rd_locator.dbOid;
	caches->main_tablespace_oid = rel->rd_locator.spcOid;
	caches->main_relfilenode    = rel->rd_locator.relNumber;
	scan->seq_cache = caches;
	scan->defer_seq_hydration = false;
}

static IndexFetchTableData *
bioseq_index_fetch_begin(Relation rel)
{
	Oid relfilelocator = rel->rd_locator.relNumber;
	if (PendingWritesInUpperTransactions(relfilelocator, GetCurrentSubTransactionId()))
	{
		/* XXX: maybe we can just flush the data and continue */
		bioseq_log<ERR, PG>( "cannot read from index when there is unflushed data in "
					"upper transactions");
	}

	MemoryContext scanContext = CreateColumnarScanMemoryContext();
	IndexFetchColumnarData *scan;
	{
		MemoryContextScope ctx_guard{scanContext};

		scan = ::new (palloc0(sizeof(IndexFetchColumnarData))) IndexFetchColumnarData{};
		scan->cs_base.rel = rel;
		scan->cs_readState = NULL;
		scan->stripeMetadataList = NIL;
		scan->scanContext = scanContext;
		scan->is_select_query = false;
		init_index_fetch_seq_cache(scan, rel, scanContext);
	}

	return &scan->cs_base;
}

IndexFetchTableData *
bioseq_index_fetch_begin_extended(Relation rel, Bitmapset *attr_needed)
{
	Oid relfilenode = rel->rd_locator.relNumber;
	if (PendingWritesInUpperTransactions(relfilenode, GetCurrentSubTransactionId()))
	{
		/* XXX: maybe we can just flush the data and continue */
		bioseq_log<ERR, PG>( "cannot read from index when there is unflushed data in "
					"upper transactions");
	}

	MemoryContext scanContext = CreateColumnarScanMemoryContext();
	IndexFetchColumnarData *scan;
	{
		MemoryContextScope ctx_guard{scanContext};

		scan = ::new (palloc0(sizeof(IndexFetchColumnarData))) IndexFetchColumnarData{};
		scan->cs_base.rel = rel;
		scan->cs_readState = NULL;
		scan->stripeMetadataList = NIL;
		scan->scanContext = scanContext;

		scan->attr_needed = bms_copy(attr_needed);
		scan->is_select_query = true;
		init_index_fetch_seq_cache(scan, rel, scanContext);
	}

	return &scan->cs_base;
}


static void
bioseq_index_fetch_reset(IndexFetchTableData *sscan)
{
	/* no-op */
}


static void
bioseq_index_fetch_end(IndexFetchTableData *sscan)
{
	bioseq_index_fetch_reset(sscan);

	IndexFetchColumnarData *scan = reinterpret_cast<IndexFetchColumnarData *>(sscan);

	/* DOC0260 Phase A — close per-fetch seq_cache. */
	if (scan->seq_cache != NULL) {
		auto* caches = static_cast<bioseq::storage::seq_caches*>(scan->seq_cache);
		caches->close_all(/*commit=*/true);
		bioseq::storage::destroy_seq_caches(caches);
		scan->seq_cache = NULL;
	}

	if (scan->cs_readState)
	{
		ColumnarEndRead(scan->cs_readState);
		scan->cs_readState = NULL;
	}

	scan->attr_needed.free();

	/* clean up any caches. */
	if (bioseq_enable_page_cache == true)
	{
		ColumnarResetCache();
	}

	MemoryContextDelete(scan->scanContext);
}

static StripeMetadata *
FindStripeMetadataFromListBinarySearch(IndexFetchColumnarData *scan, uint64_t rowNumber)
{
	ListCell *lc = NULL;

	int high = scan->stripeMetadataList.length() - 1;
	int low = 0;

	while(low <= high)
	{
		int mid = low + (high - low) / 2;

		lc = list_nth_cell(scan->stripeMetadataList.ptr(), mid);

		StripeMetadata *stripeMetadata = static_cast<StripeMetadata*>(lc->ptr_value);

		if (rowNumber >= stripeMetadata->firstRowNumber &&
			rowNumber < stripeMetadata->firstRowNumber + stripeMetadata->rowCount)
		{
			return stripeMetadata;
		}

		if (stripeMetadata->firstRowNumber > rowNumber)
		{
			high = mid - 1;
		}
		else
		{
			low = mid + 1;
		}
	}
	return NULL;
}


static bool
bioseq_index_fetch_tuple(IndexFetchTableData *sscan,
						   ItemPointer tid,
						   Snapshot snapshot,
						   TupleTableSlot *slot,
						   bool *call_again, bool *all_dead)
{
	/* no HOT chains are possible in columnar, directly set it to false */
	*call_again = false;

	/*
	 * Initialize all_dead to false if passed to be non-NULL.
	 *
	 * XXX: For aborted writes, we should set all_dead to true but this would
	 * require implementing bioseq_index_delete_tuples for simple deletion
	 * of dead tuples (TM_IndexDeleteOp.bottomup = false).
	 */
	if (all_dead)
	{
		*all_dead = false;
	}

	ExecClearTuple(slot);

	IndexFetchColumnarData *scan = reinterpret_cast<IndexFetchColumnarData *>(sscan);
	Relation columnarRelation = scan->cs_base.rel;

	/* initialize read state for the first row */
	if (scan->cs_readState == NULL)
	{
		/* no quals for index scan */
		List *scanQual = NIL;

		if (scan->attr_needed.empty())
		{
			/* we need all columns */
			int natts = columnarRelation->rd_att->natts;
			scan->attr_needed.free();
			scan->attr_needed = PBitmapset::full_range(natts);
		}

		bool randomAccess = true;
		scan->cs_readState = init_bioseq_read_state(columnarRelation,
													  slot->tts_tupleDescriptor,
													  scan->attr_needed, scanQual,
													  scan->scanContext,
													  snapshot, randomAccess,
													  NULL);
		if (scan->is_select_query)
			scan->stripeMetadataList =
				StripesForRelfilenode(columnarRelation->rd_locator, ForwardScanDirection);

	}

	uint64_t rowNumber = tid_to_row_number(*tid);

	StripeMetadata *stripeMetadata = NULL;
	
	if (scan->is_select_query)
		stripeMetadata = FindStripeMetadataFromListBinarySearch(scan, rowNumber);
	else
		stripeMetadata = FindStripeWithMatchingFirstRowNumber(columnarRelation, rowNumber, snapshot);
	
	if (!stripeMetadata)
	{
		/* it is certain that tuple with rowNumber doesn't exist */
		return false;
	}

	StripeWriteStateEnum stripeWriteState = StripeWriteState(stripeMetadata);

	if (stripeWriteState == STRIPE_WRITE_FLUSHED &&
		!ColumnarReadRowByRowNumber(scan->cs_readState, rowNumber,
									slot->tts_values, slot->tts_isnull))
	{
		/*
		 * FindStripeWithMatchingFirstRowNumber doesn't verify upper row
		 * number boundary of found stripe. For this reason, we didn't
		 * certainly know if given row number belongs to one of the stripes.
		 */
		return false;
	}
	else if (stripeWriteState == STRIPE_WRITE_ABORTED)
	{
		/*
		 * We only expect to see un-flushed stripes when checking against
		 * constraint violation. In that case, indexAM provides dirty
		 * snapshot to index_fetch_tuple callback.
		 */
		pfree(stripeMetadata);
		Assert(snapshot->snapshot_type == SNAPSHOT_DIRTY);
		return false;
	}
	else if (stripeWriteState == STRIPE_WRITE_IN_PROGRESS)
	{
		if (stripeMetadata->insertedByCurrentXact)
		{
			/*
			 * Stripe write is in progress and its entry is inserted by current
			 * transaction, so obviously it must be written by me. Since caller
			 * might want to use tupleslot datums for some reason, do another
			 * look-up, but this time by first flushing our writes.
			 *
			 * XXX: For index scan, this is the only case that we flush pending
			 * writes of the current backend. If we have taught reader how to
			 * read from WriteStateMap. then we could guarantee that
			 * index_fetch_tuple would never flush pending writes, but this seem
			 * to be too much work for now, but should be doable.
			 */
			ColumnarReadFlushPendingWrites(scan->cs_readState);

			/*
			 * Fill the tupleslot and fall through to return true, it
			 * certainly exists.
			 */
			ColumnarReadRowByRowNumberOrError(scan->cs_readState, rowNumber,
											  slot->tts_values, slot->tts_isnull);
		}
		else
		{
			/* similar to aborted writes, it should be dirty snapshot */
			Assert(snapshot->snapshot_type == SNAPSHOT_DIRTY);

			/*
			 * Stripe that "might" contain the tuple with rowNumber is not
			 * flushed yet. Here we set all attributes of given tupleslot to NULL
			 * before returning true and expect the indexAM callback that called
			 * us --possibly to check against constraint violation-- blocks until
			 * writer transaction commits or aborts, without requiring us to fill
			 * the tupleslot properly.
			 *
			 * XXX: Note that the assumption we made above for the tupleslot
			 * holds for "unique" constraints defined on "btree" indexes.
			 *
			 * For the other constraints that we support, namely:
			 * * exclusion on btree,
			 * * exclusion on hash,
			 * * unique on btree;
			 * we still need to fill tts_values.
			 *
			 * However, for the same reason, we should have already flushed
			 * single tuple stripes when inserting into table for those three
			 * classes of constraints.
			 *
			 * This is annoying, but this also explains why this hack works for
			 * unique constraints on btree indexes, and also explains how we
			 * would never end up with "else" condition otherwise.
			 */
			memset(slot->tts_isnull, true, slot->tts_nvalid * sizeof(bool));
		}
	}
	else
	{
		/*
		 * At this point, we certainly know that stripe is flushed and
		 * ColumnarReadRowByRowNumber successfully filled the tupleslot.
		 */
		Assert(stripeWriteState == STRIPE_WRITE_FLUSHED);
	}

	if (!scan->is_select_query)
		pfree(stripeMetadata);
	slot->tts_tableOid = RelationGetRelid(columnarRelation);
	slot->tts_tid = *tid;
	ExecStoreVirtualTuple(slot);

	/*
	 * DOC0260 Phase A — hydrate SEQ stub columns unless the caller asked
	 * for deferred hydration. For the index-fetch path we always hydrate
	 * because downstream operators (recheck quals, executor projection)
	 * read SEQ values directly.
	 */
	if (!scan->defer_seq_hydration)
	{
		const auto& amc = bioseq::storage::bioseq_am_cache_for(columnarRelation);
		auto* caches = static_cast<bioseq::storage::seq_caches*>(scan->seq_cache);
		hydrate_seq_columns_in_slot(columnarRelation, slot, caches, amc,
		                            scan->scanContext);
	}

	return true;
}


static bool
bioseq_fetch_row_version(Relation relation,
						   ItemPointer tid,
						   Snapshot snapshot,
						   TupleTableSlot *slot)
{
	uint64_t rowNumber = tid_to_row_number(*tid);
	ColumnarReadState **readState =
		FindReadStateCache(relation, GetCurrentSubTransactionId());

	if (readState == NULL)
	{
		readState = InitColumnarReadStateCache(relation, GetCurrentSubTransactionId());

		int natts = relation->rd_att->natts;
		PBitmapset attr_needed = PBitmapset::full_range(natts);

		List *scanQual = NIL;

		bool randomAccess = false;

		*readState = init_bioseq_read_state(relation,
											  slot->tts_tupleDescriptor,
											  attr_needed, scanQual,
											  GetColumnarReadStateCache(),
											  snapshot, randomAccess,
											  NULL);
	}

	{
		MemoryContextScope ctx_guard{GetColumnarReadStateCache()};
		ColumnarReadRowByRowNumber(*readState, rowNumber,
								   slot->tts_values, slot->tts_isnull);
	}

	slot->tts_tableOid = RelationGetRelid(relation);
	slot->tts_tid = *tid;

	if (TTS_EMPTY(slot))
		ExecStoreVirtualTuple(slot);

	/* DOC0260 Phase A — hydrate any SEQ stubs in the slot. We do NOT
	 * cache a per-fetch seq_caches here because the row-version fetch
	 * is one-shot; allocate, use, destroy. */
	{
		const auto& amc = bioseq::storage::bioseq_am_cache_for(relation);
		if (!amc.seq_attnos.empty()) {
			auto* caches = bioseq::storage::alloc_seq_caches(GetColumnarReadStateCache());
			caches->side = bioseq::storage::seq_caches::Side::Read;
			caches->lock_mode = AccessShareLock;
			caches->read_state_mctx = GetColumnarReadStateCache();
			caches->main_database_oid   = relation->rd_locator.dbOid;
			caches->main_tablespace_oid = relation->rd_locator.spcOid;
			caches->main_relfilenode    = relation->rd_locator.relNumber;
			hydrate_seq_columns_in_slot(relation, slot, caches, amc,
			                            GetColumnarReadStateCache());
			caches->close_all(/*commit=*/true);
			bioseq::storage::destroy_seq_caches(caches);
		}
	}

	return true;
}


static void
bioseq_get_latest_tid(TableScanDesc sscan,
						ItemPointer tid)
{
	bioseq_log<ERR, PG>( "bioseq_get_latest_tid not implemented");
}


static bool
bioseq_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	bioseq_log<ERR, PG>( "bioseq_tuple_tid_valid not implemented");
}


static bool
bioseq_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
								  Snapshot snapshot)
{
	uint64_t rowNumber = tid_to_row_number(slot->tts_tid);
	StripeMetadata *stripeMetadata = FindStripeByRowNumber(rel, rowNumber, snapshot);
	return stripeMetadata != NULL;
}


static TransactionId
bioseq_index_delete_tuples(Relation rel,
							 TM_IndexDeleteOp *delstate)
{
	previousCacheEnabledState = bioseq_enable_page_cache;
	bioseq_enable_page_cache = false;

	/*
	 * XXX: We didn't bother implementing index_delete_tuple for neither of
	 * simple deletion and bottom-up deletion cases. There is no particular
	 * reason for that, just to keep things simple.
	 *
	 * See the rest of this function to see how we deal with
	 * index_delete_tuples requests made to columnarAM.
	 */

	if (delstate->bottomup)
	{
		/*
		 * Ignore any bottom-up deletion requests.
		 *
		 * Currently only caller in postgres that does bottom-up deletion is
		 * _bt_bottomupdel_pass, which in turn calls _bt_delitems_delete_check.
		 * And this function is okay with ndeltids being set to 0 by tableAM
		 * for bottom-up deletion.
		 */
		delstate->ndeltids = 0;
		return InvalidTransactionId;
	}
	else
	{
		/*
		 * TableAM is not expected to set ndeltids to 0 for simple deletion
		 * case, so here we cannot do the same trick that we do for
		 * bottom-up deletion.
		 * See the assertion around table_index_delete_tuples call in pg
		 * function index_compute_xid_horizon_for_tuples.
		 *
		 * For this reason, to avoid receiving simple deletion requests for
		 * columnar tables (bottomup = false), bioseq_index_fetch_tuple
		 * doesn't ever set all_dead to true in order to prevent triggering
		 * simple deletion of index tuples. But let's throw an error to be on
		 * the safe side.
		 */
		bioseq_log<ERR, PG>( "bioseq_index_delete_tuples not implemented for simple deletion");
	}
}


/*
 * DOC0260 Phase A — rewrite every non-NULL SEQ column in `detoasted_values`
 * to a 40-byte locator stub. The original payload bytes are written to the
 * matching sub-table shard via sequence_storage_manager::insert_payload.
 * Returns a freshly-palloc'd Datum* (in the per-tuple context); the caller
 * MUST hand THIS to ColumnarWriteRow (NOT slot->tts_values) so that
 * ExecInsertIndexTuples — which reads from `slot->tts_values[]` to build
 * btree/hash/gin keys — never sees the storage-layout bytes.
 *
 * `out_per_col_locator[c]` is the locator for amc.seq_attnos[c]; left as
 * std::nullopt for NULL/already-stub values.
 */
[[nodiscard]] static Datum *
rewrite_seq_columns_to_stubs(Relation relation,
                             TupleTableSlot *slot,
                             ColumnarWriteState *writeState,
                             const bioseq::storage::BioseqAmCache &amc,
                             Datum *detoasted_values,
                             std::vector<std::optional<bioseq::storage::seq_locator>>
                                 &out_per_col_locator)
{
	TupleDesc td = slot->tts_tupleDescriptor;
	Datum *values_for_write = static_cast<Datum *>(
		palloc(sizeof(Datum) * td->natts));
	std::memcpy(values_for_write, detoasted_values, sizeof(Datum) * td->natts);

	out_per_col_locator.assign(amc.seq_attnos.size(), std::nullopt);
	if (amc.seq_attnos.empty()) {
		return values_for_write;
	}
	// DOC0260 Phase A — lazily allocate the seq_caches on first call,
	// because write_state_management.cppm intentionally does not import
	// bioseq_storage (would cycle with bioseq_table). Allocate in the
	// write state's `stripeWriteContext` — the per-tuple context is reset
	// at the end of every bioseq_tuple_insert, which would leave
	// writeState->seq_caches as a dangling pointer that the NEXT
	// INSERT-row's `caches->...` dereference crashes on.
	if (writeState->seq_caches == nullptr) {
		auto *fresh = bioseq::storage::alloc_seq_caches(
			writeState->stripeWriteContext);
		fresh->side = bioseq::storage::seq_caches::Side::Write;
		fresh->lock_mode = RowExclusiveLock;
		fresh->main_database_oid   = relation->rd_locator.dbOid;
		fresh->main_tablespace_oid = relation->rd_locator.spcOid;
		fresh->main_relfilenode    = relation->rd_locator.relNumber;
		fresh->cap_bytes_snapshot  =
			bioseq::storage::read_max_sub_table_cap_bytes();
		writeState->seq_caches = fresh;
	}
	auto *caches = static_cast<bioseq::storage::seq_caches *>(writeState->seq_caches);
	const auto cap_bytes = caches->cap_bytes_snapshot;

	for (std::size_t c = 0; c < amc.seq_attnos.size(); ++c) {
		const std::int16_t attno = amc.seq_attnos[c];
		const int idx = attno - 1;
		if (idx < 0 || idx >= td->natts) continue;
		if (slot->tts_isnull[idx]) continue;
		auto *hdr = reinterpret_cast<pgseq_t *>(
			DatumGetPointer(values_for_write[idx]));
		if (bioseq::storage::is_locator_stub(hdr)) continue;
		const auto loc =
			bioseq::storage::bioseq_default_storage_manager::insert_payload_as_fragments(
				relation->rd_locator.dbOid,
				relation->rd_locator.spcOid,
				relation->rd_locator.relNumber,
				attno, td, hdr, cap_bytes, caches);
		out_per_col_locator[c] = loc;
		pgseq_t *stub = bioseq::storage::alloc_locator_stub_in(
			ColumnarWritePerTupleContext(writeState),
			static_cast<std::uint8_t>(hdr->alphabet_code),
			static_cast<std::uint8_t>(hdr->storage_unit_byte_width),
			hdr->padding_letters,
			/*total_bp_size=*/loc.total_bp_size,
			loc);
		values_for_write[idx] = PointerGetDatum(stub);
	}
	return values_for_write;
}

static void
bioseq_tuple_insert(Relation relation, TupleTableSlot *slot, CommandId cid,
					  int options, BulkInsertState bistate)
{
	if (relation->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
	{
		bioseq_log<ERR, PG>(
			"bioseq tables cannot be TEMPORARY; use UNLOGGED or permanent");
	}

	previousCacheEnabledState = bioseq_enable_page_cache;
	bioseq_enable_page_cache = false;

	/*
	 * bioseq_init_write_state allocates the write state in a longer
	 * lasting context, so no need to worry about it.
	 */
	ColumnarWriteState *writeState = bioseq_init_write_state(relation,
															   RelationGetDescr(relation),
																 slot->tts_tableOid,
															   GetCurrentSubTransactionId());
	{
		MemoryContextScope ctx_guard{ColumnarWritePerTupleContext(writeState)};

		ColumnarCheckLogicalReplication(relation);

		slot_getallattrs(slot);

		Datum *detoasted_values = detoast_values(slot->tts_tupleDescriptor,
									   slot->tts_values, slot->tts_isnull);

		/* DOC0260 Phase A — rewrite SEQ columns to locator stubs. */
		const auto &amc = bioseq::storage::bioseq_am_cache_for(relation);
		std::vector<std::optional<bioseq::storage::seq_locator>> per_col_locator;
		Datum *values_for_write = rewrite_seq_columns_to_stubs(
			relation, slot, writeState, amc, detoasted_values, per_col_locator);

		uint64_t writtenRowNumber = ColumnarWriteRow(writeState, values_for_write,
		                                             slot->tts_isnull);
		slot->tts_tid = row_number_to_tid(writtenRowNumber);

		/* DOC0279 §11.2 — build every UPDATE_ON_INSERT index (per SEQ column)
		 * via the new chunked-storage drivers. detoasted_values carries the
		 * ORIGINAL payload (pre-stub) so the index sees the real bytes. */
		bioseq::storage::indexing::index_build_on_insert_columns(
			RelationGetRelid(relation), amc.seq_attnos, slot->tts_isnull,
			detoasted_values, per_col_locator, writtenRowNumber,
			ColumnarWritePerTupleContext(writeState));
	}
	MemoryContextReset(ColumnarWritePerTupleContext(writeState));

	pgstat_count_heap_insert(relation, 1);
}


static void
bioseq_tuple_insert_speculative(Relation relation, TupleTableSlot *slot,
								  CommandId cid, int options,
								  BulkInsertState bistate, uint32_t specToken)
{
	if (relation->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
	{
		bioseq_log<ERR, PG>(
			"bioseq tables cannot be TEMPORARY; use UNLOGGED or permanent");
	}

	previousCacheEnabledState = bioseq_enable_page_cache;
	bioseq_enable_page_cache = false;

	/*
	 * bioseq_init_write_state allocates the write state in a longer
	 * lasting context, so no need to worry about it.
	 */
	ColumnarWriteState *writeState = bioseq_init_write_state(relation,
															   RelationGetDescr(relation),
																 slot->tts_tableOid,
															   GetCurrentSubTransactionId());
	{
		MemoryContextScope ctx_guard{ColumnarWritePerTupleContext(writeState)};

		ColumnarCheckLogicalReplication(relation);

		slot_getallattrs(slot);

		Datum *detoasted_values = detoast_values(slot->tts_tupleDescriptor,
									   slot->tts_values, slot->tts_isnull);

		const auto &amc = bioseq::storage::bioseq_am_cache_for(relation);
		std::vector<std::optional<bioseq::storage::seq_locator>> per_col_locator;
		Datum *values_for_write = rewrite_seq_columns_to_stubs(
			relation, slot, writeState, amc, detoasted_values, per_col_locator);

		uint64_t storageId = LookupStorageId(relation->rd_locator);
		uint64_t writtenRowNumber = ColumnarWriteRow(writeState, values_for_write,
		                                             slot->tts_isnull);
		UpdateRowMask(relation->rd_locator, storageId,  NULL, writtenRowNumber);
		slot->tts_tid = row_number_to_tid(writtenRowNumber);
	}
	MemoryContextReset(ColumnarWritePerTupleContext(writeState));

	pgstat_count_heap_insert(relation, 1);
}


static void
bioseq_tuple_complete_speculative(Relation relation, TupleTableSlot *slot,
									uint32_t specToken, bool succeeded)
{
	uint64_t storageId = LookupStorageId(relation->rd_locator);
	/* Set lock for relation until transaction ends */
	DirectFunctionCall1(pg_advisory_xact_lock_int8,
						Int64GetDatum((int64_t) storageId));

	bioseq_enable_page_cache = previousCacheEnabledState;
}

static void
bioseq_multi_insert(Relation relation, TupleTableSlot **slots, int ntuples,
					  CommandId cid, int options, BulkInsertState bistate)
{
	if (relation->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
	{
		bioseq_log<ERR, PG>(
			"bioseq tables cannot be TEMPORARY; use UNLOGGED or permanent");
	}

	ColumnarWriteState *writeState = bioseq_init_write_state(relation,
															   RelationGetDescr(relation),
																 slots[0]->tts_tableOid,
															   GetCurrentSubTransactionId());

	ColumnarCheckLogicalReplication(relation);

	const auto &amc = bioseq::storage::bioseq_am_cache_for(relation);

	{
		MemoryContextScope ctx_guard{ColumnarWritePerTupleContext(writeState)};

		for (int i = 0; i < ntuples; i++)
		{
			TupleTableSlot *tupleSlot = slots[i];

			slot_getallattrs(tupleSlot);

			Datum *detoasted_values = detoast_values(tupleSlot->tts_tupleDescriptor,
										   tupleSlot->tts_values, tupleSlot->tts_isnull);

			std::vector<std::optional<bioseq::storage::seq_locator>> per_col_locator;
			Datum *values_for_write = rewrite_seq_columns_to_stubs(
				relation, tupleSlot, writeState, amc,
				detoasted_values, per_col_locator);

			uint64_t writtenRowNumber = ColumnarWriteRow(writeState,
				values_for_write, tupleSlot->tts_isnull);

			bioseq::storage::indexing::index_build_on_insert_columns(
				RelationGetRelid(relation), amc.seq_attnos, tupleSlot->tts_isnull,
				detoasted_values, per_col_locator, writtenRowNumber,
				ColumnarWritePerTupleContext(writeState));

			EState *estate = create_estate_for_relation(relation);

			ResultRelInfo *resultRelInfo = makeNode(ResultRelInfo);
			InitResultRelInfo(resultRelInfo, relation, 1, NULL, 0);

			ExecOpenIndices(resultRelInfo, false);

			if (relation->rd_att->constr)
				ExecConstraints(resultRelInfo, tupleSlot, estate);

			ExecCloseIndices(resultRelInfo);

			AfterTriggerEndQuery(estate);
			ExecCloseResultRelations(estate);
			ExecCloseRangeTableRelations(estate);

			ExecResetTupleTable(estate->es_tupleTable, false);
			FreeExecutorState(estate);

			tupleSlot->tts_tid = row_number_to_tid(writtenRowNumber);

			MemoryContextReset(ColumnarWritePerTupleContext(writeState));
		}
	}

	pgstat_count_heap_insert(relation, ntuples);
}


static TM_Result
bioseq_tuple_delete(Relation relation, ItemPointer tid, CommandId cid,
					  Snapshot snapshot, Snapshot crosscheck, bool wait,
					  TM_FailureData *tmfd, bool changingPart)
{
	uint64_t rowNumber = tid_to_row_number(*tid);
	uint64_t storageId = LookupStorageId(relation->rd_locator);
	/* Set lock for relation until transaction ends */
	DirectFunctionCall1(pg_advisory_xact_lock_int8,
						Int64GetDatum((int64_t) storageId));

	if (!UpdateRowMask(relation->rd_locator, storageId,  snapshot, rowNumber))
		return TM_Deleted;

	pgstat_count_heap_delete(relation);

	return TM_Ok;
}


static TM_Result
bioseq_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
					  CommandId cid, Snapshot snapshot, Snapshot crosscheck,
					  bool wait, TM_FailureData *tmfd,
					  LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{

	
	uint64_t rowNumber = tid_to_row_number(*otid);
	uint64_t storageId = LookupStorageId(relation->rd_locator);

	/* Set lock for relation until transaction ends */
	DirectFunctionCall1(pg_advisory_xact_lock_int8,
						Int64GetDatum((int64_t) storageId));

	if (!UpdateRowMask(relation->rd_locator, storageId, snapshot, rowNumber))
		return TM_Deleted;

	bioseq_tuple_insert(relation, slot, cid, 0, NULL);

	*update_indexes = (TU_UpdateIndexes) true;

	pgstat_count_heap_update(relation, false, false);
	return TM_Ok;
}


static TM_Result
bioseq_tuple_lock(Relation relation, ItemPointer tid, Snapshot snapshot,
					TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
					LockWaitPolicy wait_policy, uint8_t flags,
					TM_FailureData *tmfd)
{
	uint64_t rowNumber = tid_to_row_number(*tid);
	ColumnarReadState *readState = NULL;

	int natts = relation->rd_att->natts;
	PBitmapset attr_needed = PBitmapset::full_range(natts);

	List *scanQual = NIL;

	bool randomAccess = true;

	readState = init_bioseq_read_state(relation,
										slot->tts_tupleDescriptor,
										attr_needed, scanQual,
										CurrentMemoryContext, // to be checked
										GetTransactionSnapshot(), randomAccess,
										NULL);

	ColumnarReadRowByRowNumber(readState, rowNumber,
							   slot->tts_values, slot->tts_isnull);

	slot->tts_tableOid = RelationGetRelid(relation);
	slot->tts_tid = *tid;

	if (TTS_EMPTY(slot))
	{
		ExecStoreVirtualTuple(slot);
	}

	return TM_Ok;
}


static void
bioseq_finish_bulk_insert(Relation relation, int options)
{
	/*
	 * Nothing to do here. We keep write states live until transaction end.
	 */
}


static void
bioseq_relation_set_new_filenode(Relation rel,
								   const RelFileLocator *newrnode,
								   char persistence,
								   TransactionId *freezeXid,
								   MultiXactId *minmulti)
{
	if (persistence == RELPERSISTENCE_TEMP)
	{
		bioseq_log<ERR, PG>(
			"bioseq tables cannot be TEMPORARY; use UNLOGGED or permanent");
	}

	/*
	 * If existing and new relfilelocator are different, that means the existing
	 * storage was dropped and we also need to clean up the metadata and
	 * state. If they are equal, this is a new relation object and we don't
	 * need to clean anything.
	 */
	if (rel->rd_locator.relNumber != newrnode->relNumber)
	{
		MarkRelfilenodeDropped(rel->rd_locator.relNumber, GetCurrentSubTransactionId());

		DeleteMetadataRows(rel->rd_locator);

		/* DOC0260 Phase A — cascade-drop sub-tables under the OLD
		 * relfilenode triple. */
		bioseq::storage::bioseq_default_storage_manager::drop_sub_tables_for_main(
			rel->rd_locator.dbOid,
			rel->rd_locator.spcOid,
			rel->rd_locator.relNumber);
	}
	*freezeXid = RecentXmin;
	*minmulti = GetOldestMultiXactId();
	SMgrRelation srel = RelationCreateStorage(*newrnode, persistence, true);

	bioseq_log<DBG>("[bioseq] pg: bioseq_relation_set_new_filenode() initialising columnar storage");
	ColumnarStorageInit(srel, ColumnarMetadataNewStorageId());
	bioseq_log<DBG>("[bioseq] pg: bioseq_relation_set_new_filenode() initialising columnar options");
	InitColumnarOptions(rel->rd_id);

	smgrclose(srel);

	/* we will lazily initialize metadata in first stripe reservation */
}


static void
bioseq_relation_nontransactional_truncate(Relation rel)
{
	RelFileLocator relfilelocator = rel->rd_locator;

	NonTransactionDropWriteState(relfilelocator.relNumber);
	/* Delete old relfilelocator metadata */
	DeleteMetadataRows(relfilelocator);

	/*
	 * No need to set new relfilelocator, since the table was created in this
	 * transaction and no other transaction can see this relation yet. We
	 * can just truncate the relation.
	 *
	 * This is similar to what is done in heapam_relation_nontransactional_truncate.
	 */
	RelationTruncate(rel, 0);

	bioseq_log<DBG>("[bioseq] pg: bioseq_relation_nontransactional_truncate() initialising columnar storage");
	uint64_t storageId = ColumnarMetadataNewStorageId();

	ColumnarStorageInit(RelationGetSmgr(rel), storageId);
}


static void
bioseq_relation_copy_data(Relation rel, const RelFileLocator *newrnode)
{
	char persistence = rel->rd_rel->relpersistence;

	/*
	 * Since we copy the file directly without looking at the shared buffers,
	 * we'd better first flush out any pages of the source relation that are
	 * in shared buffers.  We assume on new changes will be made while we are
	 * holding exclusive lock on the rel.
	 */
	FlushRelationBuffers(rel);

	SMgrRelation srel = RelationCreateStorage(*newrnode, persistence, true);

	/* copy main fork */
	RelationCopyStorage(RelationGetSmgr(rel), srel, MAIN_FORKNUM, persistence);

	for (ForkNumber forkNum = static_cast<ForkNumber>(MAIN_FORKNUM + 1);
		forkNum <= MAX_FORKNUM; forkNum = static_cast<ForkNumber>(static_cast<int>(forkNum) + 1))
	{
		if (smgrexists(srel, forkNum))
		{
			smgrcreate(srel, forkNum, false);

			/*
			 * WAL log creation if the relation is persistent, or
			 * this is the init fork of an unlogged relation.
			 */
			if (RelationIsPermanent(rel) ||
				(persistence == RELPERSISTENCE_UNLOGGED &&
				forkNum == INIT_FORKNUM))
				log_smgrcreate(newrnode, forkNum);

			RelationCopyStorage(RelationGetSmgr(rel), srel,
						forkNum, persistence);
		}
	}
}


/*
 * bioseq_relation_copy_for_cluster is called on VACUUM FULL, at which
 * we should copy data from OldHeap to NewHeap.
 *
 * In general TableAM case this can also be called for the CLUSTER command
 * which is not applicable for columnar since it doesn't support indexes.
 */
static void
bioseq_relation_copy_for_cluster(Relation OldHeap, Relation NewHeap,
								   Relation OldIndex, bool use_sort,
								   TransactionId OldestXmin,
								   TransactionId *xid_cutoff,
								   MultiXactId *multi_cutoff,
								   double *num_tuples,
								   double *tups_vacuumed,
								   double *tups_recently_dead)
{
	TupleDesc sourceDesc = RelationGetDescr(OldHeap);
	TupleDesc targetDesc = RelationGetDescr(NewHeap);

	if (OldIndex != NULL || use_sort)
	{
		bioseq_report<ERR, PG>().code(ERRCODE_FEATURE_NOT_SUPPORTED)
			.msg("clustering columnar tables using indexes is not supported");
	}

	/*
	 * copy_table_data in cluster.c assumes tuple descriptors are exactly
	 * the same. Even dropped columns exist and are marked as attisdropped
	 * in the target relation.
	 */
	Assert(sourceDesc->natts == targetDesc->natts);

	/* read settings from old heap, relfilelocator will be swapped at the end */
	ColumnarOptions columnarOptions = { 0 };
	ReadColumnarOptions(OldHeap->rd_id, &columnarOptions);

	/*
	 * DOC0260 Phase A — CLUSTER allocates a new relfilenode for NewHeap.
	 * The catalog rows in bioseq_internal.seq_sub_table are keyed on the
	 * (db, spc, relfilenode) triple, so OldHeap's sub-tables (under
	 * OldHeap's relfilenode) are independent from NewHeap's sub-tables
	 * (under NewHeap's relfilenode).
	 *
	 * We thread the source scan through with defer_seq_hydration=TRUE so
	 * we see OldHeap's stubs verbatim. For each row, decode the stub,
	 * fetch_payload against OldHeap's triple, then re-insert_payload into
	 * NewHeap's triple via the regular rewrite path on the destination
	 * write state.
	 */
	ColumnarWriteState *writeState = bioseq_init_write_state(NewHeap, targetDesc,
		RelationGetRelid(NewHeap), GetCurrentSubTransactionId());

	/* we need all columns */
	int natts = OldHeap->rd_att->natts;
	PBitmapset attr_needed = PBitmapset::full_range(natts);

	/* no quals for table rewrite */
	List *scanQual = NIL;

	/* use SnapshotAny when re-writing table as heapAM does */
	Snapshot snapshot = SnapshotAny;

	MemoryContext scanContext = CreateColumnarScanMemoryContext();
	bool randomAccess = false;
	ColumnarReadState *readState = init_bioseq_read_state(OldHeap, sourceDesc,
															attr_needed, scanQual,
															scanContext, snapshot,
															randomAccess,
															NULL);

	/* Read-side seq cache: hydrates OldHeap's stubs to real payloads. */
	auto* read_caches = bioseq::storage::alloc_seq_caches(scanContext);
	read_caches->side = bioseq::storage::seq_caches::Side::Read;
	read_caches->lock_mode = AccessShareLock;
	read_caches->read_state_mctx = scanContext;
	read_caches->main_database_oid   = OldHeap->rd_locator.dbOid;
	read_caches->main_tablespace_oid = OldHeap->rd_locator.spcOid;
	read_caches->main_relfilenode    = OldHeap->rd_locator.relNumber;

	Datum *values = make_array<Datum>(sourceDesc->natts);
	bool *nulls = make_array<bool>(sourceDesc->natts);

	const auto &source_amc = bioseq::storage::bioseq_am_cache_for(OldHeap);

	*num_tuples = 0;

	while (ColumnarReadNextRow(readState, values, nulls, NULL))
	{
		/* Hydrate any SEQ stub columns from OldHeap's sub-tables. */
		for (std::int16_t attno : source_amc.seq_attnos) {
			const int idx = attno - 1;
			if (idx < 0 || idx >= sourceDesc->natts) continue;
			if (nulls[idx]) continue;
			auto* hdr = reinterpret_cast<const pgseq_t*>(
				DatumGetPointer(values[idx]));
			if (!bioseq::storage::is_locator_stub(hdr)) continue;
			const auto loc = bioseq::storage::decode_locator_from_stub(hdr);
			pgseq_t* real = bioseq::storage::bioseq_default_storage_manager::fetch_payload(
				OldHeap->rd_locator.dbOid, OldHeap->rd_locator.spcOid,
				OldHeap->rd_locator.relNumber, attno, loc, scanContext, read_caches);
			values[idx] = PointerGetDatum(real);
		}

		/* Rewrite SEQ columns into stubs targeting NewHeap's sub-tables.
		 * We mimic the per-tuple insert path by using the write-side cache. */
		auto* write_caches = static_cast<bioseq::storage::seq_caches*>(
			writeState->seq_caches);
		for (std::int16_t attno : source_amc.seq_attnos) {
			const int idx = attno - 1;
			if (idx < 0 || idx >= sourceDesc->natts) continue;
			if (nulls[idx]) continue;
			auto* hdr = reinterpret_cast<const pgseq_t*>(
				DatumGetPointer(values[idx]));
			if (bioseq::storage::is_locator_stub(hdr)) continue;
			const auto cap_bytes = write_caches != nullptr
				? write_caches->cap_bytes_snapshot
				: bioseq::storage::read_max_sub_table_cap_bytes();
			const auto loc =
				bioseq::storage::bioseq_default_storage_manager::insert_payload_as_fragments(
					NewHeap->rd_locator.dbOid, NewHeap->rd_locator.spcOid,
					NewHeap->rd_locator.relNumber,
					attno, targetDesc, hdr, cap_bytes, write_caches);
			pgseq_t* stub = bioseq::storage::alloc_locator_stub_in(
				ColumnarWritePerTupleContext(writeState),
				static_cast<std::uint8_t>(hdr->alphabet_code),
				static_cast<std::uint8_t>(hdr->storage_unit_byte_width),
				hdr->padding_letters,
				loc.total_bp_size,
				loc);
			values[idx] = PointerGetDatum(stub);
		}

		ColumnarWriteRow(writeState, values, nulls);
		(*num_tuples)++;
	}

	*tups_vacuumed = 0;

	read_caches->close_all(/*commit=*/true);
	bioseq::storage::destroy_seq_caches(read_caches);

	ColumnarEndRead(readState);
	MemoryContextDelete(scanContext);

	/* DOC0260 §8.7 — cascade-drop OldHeap's sub-tables before the
	 * finish_heap_swap. After the swap the relfilenodes are swapped and
	 * OldHeap's sub-tables become orphans under the (now-defunct) old
	 * triple. The reaper is the long-tail safety net; this call makes
	 * the cleanup deterministic. */
	bioseq::storage::bioseq_default_storage_manager::drop_sub_tables_for_main(
		OldHeap->rd_locator.dbOid,
		OldHeap->rd_locator.spcOid,
		OldHeap->rd_locator.relNumber);
}


/*
 * NeededColumnsList returns a list of AttrNumber's for the columns that
 * are not dropped and specified by attr_needed.
 */
[[nodiscard]] static List *
NeededColumnsList(TupleDesc tupdesc, const PBitmapset& attr_needed)
{
	PIntList columnList;

	for (int i = 0; i < tupdesc->natts; i++)
	{
		if (TupleDescAttr(tupdesc, i)->attisdropped)
		{
			continue;
		}

		/* attr_needed is 0-indexed but columnList is 1-indexed */
		if (attr_needed.contains(i))
		{
			AttrNumber varattno = i + 1;
			columnList.append(varattno);
		}
	}

	return columnList.release();
}

/*
 * TruncateAndCombineColumnarStripes will combine last n stripes so they can fit
 * maximum number of rows per stripe. Stripes that are going to be combined are deleted
 * and new stripe will be written at the end of untouched last stripe. We also include
 * information about deleted rows for this implementation.
 */
static bool
TruncateAndCombineColumnarStripes(Relation rel, int elevel)
{
	uint64_t totalRowNumberCount = 0;
	uint32_t startingStripeListPosition = 0;

	TupleDesc tupleDesc = RelationGetDescr(rel);

	if (tupleDesc->natts == 0)
	{
		bioseq_log_pg(elevel, "\"{}\": stopping vacuum due to zero column table",
			   RelationGetRelationName(rel));
		return false;
	}

	/* Get current columnar options */
	ColumnarOptions columnarOptions = { 0 };
	ReadColumnarOptions(rel->rd_id, &columnarOptions);

	/* Get all stripes in reverse order */
	List *stripeMetadataList = StripesForRelfilenode(rel->rd_locator, BackwardScanDirection);
	/* Empty table nothing to do */
	if (stripeMetadataList == NIL)
	{
		bioseq_log_pg(elevel, "\"{}\": stopping vacuum due to empty table",
			   RelationGetRelationName(rel));
		return false;
	}

	//ListCell *lc = NULL;

	uint32_t lastStripeDeletedRows = 0;

	Size totalDecompressedStripeLength = 0;

	for (auto stripeMetadata : PList<StripeMetadata>(stripeMetadataList))
	{
		lastStripeDeletedRows = DeletedRowsForStripe(rel->rd_locator,
													 stripeMetadata->chunkCount,
													 stripeMetadata->id);
		totalDecompressedStripeLength +=
			DecompressedLengthForStripe(rel->rd_locator, stripeMetadata->id);

		if (totalDecompressedStripeLength >= 1024000000)
		{
			break;
		}

		uint64_t stripeRowCount = stripeMetadata->rowCount - lastStripeDeletedRows;

		if ((totalRowNumberCount + stripeRowCount >= columnarOptions.stripeRowCount))
		{
			break;
		}

		totalRowNumberCount += stripeRowCount;
		startingStripeListPosition++;
	}

	/*
	 * One stripe that is "full" so do nothing.
	 */
	if (startingStripeListPosition == 0)
	{
		return false;
	}

	/*
	 * There is only one stripe that is candidate. Maybe we should vacuum
	 * it if condition is met.
	 */
	else if (startingStripeListPosition == 1)
	{
		/* Maybe we should vacuum only one stripe if count of
		 * deleted rows is higher than 20 percent.
		 */
		float percentageOfDeleteRows =
			(float)lastStripeDeletedRows / (float)(totalRowNumberCount + lastStripeDeletedRows);

		bool shouldVacuumOnlyStripe = percentageOfDeleteRows > 0.2f;

		if (!shouldVacuumOnlyStripe)
		{
			return false;
		}
	}

	/*
	 * We need to clear current proces (VACUUUM) status flag here. Why?
	 * Current process has flag `PROC_IN_VACUUM` which is problematic because
	 * we write here into metadata heap tables. If concurrent process
	 * read page in which we inserted metadata tuples these tuples will be considered
	 * DEAD and will be removed (in problematic scenarion).
	 * Concurrent process, when assigning RecentXmin will scan all active processes in
	 * system but will NOT consider this process because of
	 * `PROC_IN_VACUUM` flag set. It looks that invalidating status flag here doesn't affect
	 * further execution.
	 */

	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	MyProc->statusFlags = 0;
	ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;

	LWLockRelease(ProcArrayLock);

	/* We need to re-assing RecentXmin here */
	PushActiveSnapshot(GetTransactionSnapshot());
	ColumnarWriteState *writeState = ColumnarBeginWrite(rel->rd_locator,
														columnarOptions,
														tupleDesc);

	/* we need all columns */
	int natts = rel->rd_att->natts;
	PBitmapset attr_needed = PBitmapset::full_range(natts);

	/* no quals for table rewrite */
	List *scanQual = NIL;

	MemoryContext scanContext = CreateColumnarScanMemoryContext();
	bool randomAccess = true;
	ColumnarReadState *readState = init_bioseq_read_state(rel, tupleDesc,
															attr_needed, scanQual,
															scanContext, SnapshotAny,
															randomAccess,
															NULL);

	ColumnarSetStripeReadState(readState,
							   static_cast<StripeMetadata*>(list_nth(stripeMetadataList, startingStripeListPosition - 1)));

	Datum *values = make_array<Datum>(tupleDesc->natts);
	bool *nulls = make_array<bool>(tupleDesc->natts);

	/* we don't need to know rowNumber here */
	while (ColumnarReadNextRow(readState, values, nulls, NULL))
	{
		ColumnarWriteRow(writeState, values, nulls);
	}

	uint64_t newDataReservation;

	if (list_length(stripeMetadataList) > startingStripeListPosition)
	{
		StripeMetadata *mtd = static_cast<StripeMetadata*>(list_nth(stripeMetadataList, startingStripeListPosition));
		newDataReservation = mtd->fileOffset + mtd->dataLength - 1;
	}
	else
	{
		StripeMetadata *mtd = static_cast<StripeMetadata*>(list_nth(stripeMetadataList, startingStripeListPosition - 1));
		newDataReservation = mtd->fileOffset;
	}

	ColumnarStorageTruncate(rel, newDataReservation);

	ColumnarEndWrite(writeState);
	ColumnarEndRead(readState);
	MemoryContextDelete(scanContext);

	for (int i = 0; i < startingStripeListPosition; i++)
	{
		StripeMetadata *metadata = static_cast<StripeMetadata*>(list_nth(stripeMetadataList, i));
		DeleteMetadataRowsForStripeId(rel->rd_locator, metadata->id);
	}

	PopActiveSnapshot();

	return true;
}

/*
 * ColumnarTableTupleCount returns the number of tuples that columnar
 * table with relationId has by using stripe metadata.
 */
static uint64_t
ColumnarTableTupleCount(Relation relation)
{
	List *stripeList = StripesForRelfilenode(relation->rd_locator, ForwardScanDirection);
	uint64_t tupleCount = 0;

	for (auto stripe : PList<StripeMetadata>(stripeList))
	{
		tupleCount += stripe->rowCount;
	}

	return tupleCount;
}

/*
 * bioseq_vacuum_rel implements VACUUM without FULL option.
 */
static void
bioseq_vacuum_rel(Relation rel, VacuumParams *params,
					BufferAccessStrategy bstrategy)
{
	/* Disable the page cache for the duration of the vacuum. */
	PageCacheDisableScope cache_guard{bioseq_enable_page_cache};

	pgstat_progress_start_command(PROGRESS_COMMAND_VACUUM,
 									RelationGetRelid(rel));

	/*
	 * If metapage version of relation is older, then we hint users to VACUUM
	 * the relation in ColumnarMetapageCheckVersion. So if needed, upgrade
	 * the metapage before doing anything.
	 */
	bool isUpgrade = true;
	ColumnarStorageUpdateIfNeeded(rel, isUpgrade);

	int elevel = (params->options & VACOPT_VERBOSE) ? INFO : DEBUG2;

	/* this should have been resolved by vacuum.c until now */
	Assert(params->truncate != VACOPTVALUE_UNSPECIFIED);

	if (params->options & VACOPT_VERBOSE)
	{
		LogRelationStats(rel, elevel);
	}

	/*
	 * We don't have updates, deletes, or concurrent updates, so all we
	 * care for now is truncating the unused space at the end of storage.
	 */
	if (params->truncate == VACOPTVALUE_ENABLED)
	{
		TruncateColumnar(rel, elevel);
	}

	BlockNumber new_rel_pages = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);

	/* get the number of indexes */
	List *indexList = RelationGetIndexList(rel);
	int nindexes = list_length(indexList);

	VacuumCutoffs cutoffs;
	vacuum_get_cutoffs(rel, params, &cutoffs);

	Assert(MultiXactIdPrecedesOrEquals(cutoffs.MultiXactCutoff, cutoffs.OldestMxact));
	Assert(TransactionIdPrecedesOrEquals(cutoffs.FreezeLimit, cutoffs.OldestXmin));

	/*
	 * Columnar storage doesn't hold any transaction IDs, so we can always
	 * just advance to the most aggressive value.
	 */
	TransactionId newRelFrozenXid = cutoffs.OldestXmin;
	MultiXactId newRelminMxid = cutoffs.OldestMxact;
	double new_live_tuples = ColumnarTableTupleCount(rel);

	/* all visible pages are always 0 */
	BlockNumber new_rel_allvisible = 0;

	bool frozenxid_updated;
	bool minmulti_updated;

	/*
	 * PG-18 added a `num_all_frozen_pages` parameter between
	 * `num_all_visible_pages` and `hasindex`. Columnar storage doesn't
	 * track per-page visibility/freezing so the value is always 0,
	 * matching the behaviour we already report for `new_rel_allvisible`.
	 */
#if PG_VERSION_NUM >= PG_VERSION_18
	BlockNumber new_rel_allfrozen = 0;
	vac_update_relstats(rel, new_rel_pages, new_live_tuples,
						new_rel_allvisible, new_rel_allfrozen,
						nindexes > 0,
						newRelFrozenXid, newRelminMxid,
						&frozenxid_updated, &minmulti_updated, false);
#else
	vac_update_relstats(rel, new_rel_pages, new_live_tuples,
						new_rel_allvisible, nindexes > 0,
						newRelFrozenXid, newRelminMxid,
						&frozenxid_updated, &minmulti_updated, false);
#endif

	/*
	 * PG-18 added a `starttime` argument so pg_stat_progress_vacuum can
	 * report how long the vacuum has been running. We don't have a real
	 * start time here (the columnar vacuum is bookkeeping only) so pass 0
	 * which the stats subsystem treats as "unknown".
	 */
#if PG_VERSION_NUM >= PG_VERSION_18
	pgstat_report_vacuum(RelationGetRelid(rel),
							rel->rd_rel->relisshared,
							std::max<int64_t>(new_live_tuples, 0),
							0,
							0 /* starttime */);
#else
	pgstat_report_vacuum(RelationGetRelid(rel),
							rel->rd_rel->relisshared,
							std::max<int64_t>(new_live_tuples, 0),
							0);
#endif
	pgstat_progress_end_command();
}


/*
 * LogRelationStats logs statistics as the output of the VACUUM VERBOSE.
 */
static void
LogRelationStats(Relation rel, int elevel)
{
	ListCell *stripeMetadataCell = NULL;
	RelFileLocator relfilelocator = rel->rd_locator;
	StringInfo infoBuf = makeStringInfo();

	int compressionStats[COMPRESSION_COUNT] = { 0 };
	uint64_t totalStripeLength = 0;
	uint64_t tupleCount = 0;
	uint64_t chunkCount = 0;
	TupleDesc tupdesc = RelationGetDescr(rel);
	uint64_t droppedChunksWithData = 0;
	uint64_t totalDecompressedLength = 0;

	List *stripeList = StripesForRelfilenode(relfilelocator, ForwardScanDirection);
	int stripeCount = list_length(stripeList);

	MemoryContext relation_stats_ctx =
		AllocSetContextCreate(CurrentMemoryContext, "Vacuum Relation Stats Context",
							  ALLOCSET_SMALL_SIZES);

	{
		MemoryContextScope ctx_guard{relation_stats_ctx};

		for (auto stripe : PList<StripeMetadata>(stripeList))
		{
			StripeSkipList *skiplist = ReadStripeSkipList(relfilelocator, stripe->id,
														  RelationGetDescr(rel),
														  stripe->chunkCount,
														  GetTransactionSnapshot());
			for (uint32_t column = 0; column < skiplist->columnCount; column++)
			{
				bool attrDropped = TupleDescAttr(tupdesc, column)->attisdropped;
				for (uint32_t chunk = 0; chunk < skiplist->chunkCount; chunk++)
				{
					ColumnChunkSkipNode *skipnode =
						&skiplist->chunkSkipNodeArray[column][chunk];

					/* ignore zero length chunks for dropped attributes */
					if (skipnode->valueLength > 0)
					{
						compressionStats[skipnode->valueCompressionType]++;
						chunkCount++;

						if (attrDropped)
						{
							droppedChunksWithData++;
						}
					}

					/*
					 * We don't compress exists buffer, so its compressed & decompressed
					 * lengths are the same.
					 */
					totalDecompressedLength += skipnode->existsLength;
					totalDecompressedLength += skipnode->decompressedValueSize;
				}
			}

			tupleCount += stripe->rowCount;
			totalStripeLength += stripe->dataLength;

			MemoryContextReset(relation_stats_ctx);
		}
	}

	uint64_t relPages = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);
	RelationCloseSmgr(rel);

	Datum storageId = DirectFunctionCall1(bioseq_relation_storageid,
										  ObjectIdGetDatum(RelationGetRelid(rel)));

	double compressionRate = totalStripeLength ?
							 (double) totalDecompressedLength / totalStripeLength :
							 1.0;

	appendStringInfo(infoBuf, "storage id: %ld\n", DatumGetInt64(storageId));
	appendStringInfo(infoBuf, "total file size: %ld, total data size: %ld\n",
					 relPages * BLCKSZ, totalStripeLength);
	appendStringInfo(infoBuf, "compression rate: %.2fx\n", compressionRate);
	appendStringInfo(infoBuf,
					 "total row count: %ld, stripe count: %d, "
					 "average rows per stripe: %ld\n",
					 tupleCount, stripeCount,
					 stripeCount ? tupleCount / stripeCount : 0);
	appendStringInfo(infoBuf,
					 "chunk count: %ld"
					 ", containing data for dropped columns: %ld",
					 chunkCount, droppedChunksWithData);
	for (int compressionType = 0; compressionType < COMPRESSION_COUNT; compressionType++)
	{
		const char *compressionName = CompressionTypeStr(static_cast<CompressionType>(compressionType));

		/* skip if this compression algorithm has not been compiled */
		if (compressionName == NULL)
		{
			continue;
		}

		/* skip if no chunks use this compression type */
		if (compressionStats[compressionType] == 0)
		{
			continue;
		}

		appendStringInfo(infoBuf,
						 ", %s compressed: %d",
						 compressionName,
						 compressionStats[compressionType]);
	}
	appendStringInfoString(infoBuf, "\n");

	bioseq_log_pg(elevel, "statistics for \"{}\":\n{}", RelationGetRelationName(rel),
		   infoBuf->data);
}


/*
 * TruncateColumnar truncates the unused space at the end of main fork for
 * a columnar table. This unused space can be created by aborted transactions.
 *
 * This implementation is based on heap_vacuum_rel in vacuumlazy.c with some
 * changes so it suits columnar store relations.
 */
static void
TruncateColumnar(Relation rel, int elevel)
{
	PGRUsage ru0;

	pg_rusage_init(&ru0);

	/* Report that we are now truncating */
	pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
								 PROGRESS_VACUUM_PHASE_TRUNCATE);


	/*
	 * We need access exclusive lock on the relation in order to do
	 * truncation. If we can't get it, give up rather than waiting --- we
	 * don't want to block other backends, and we don't want to deadlock
	 * (which is quite possible considering we already hold a lower-grade
	 * lock).
	 *
	 * The decisions for AccessExclusiveLock and conditional lock with
	 * a timeout is based on lazy_truncate_heap in vacuumlazy.c.
	 */
	if (!ConditionalLockRelationWithTimeout(rel, AccessExclusiveLock,
											VACUUM_TRUNCATE_LOCK_TIMEOUT,
											VACUUM_TRUNCATE_LOCK_WAIT_INTERVAL,
											false))
	{
		/*
		 * We failed to establish the lock in the specified number of
		 * retries. This means we give up truncating.
		 */
		bioseq_log_pg(elevel, "\"{}\": stopping truncate due to conflicting lock request",
			   RelationGetRelationName(rel));
		return;
	}

	bool stripesTruncated = TruncateAndCombineColumnarStripes(rel, elevel);

	/*
	 * If we didn't truncate and combine tail stripes we could have
	 * stituation where we need to truncate storage at the end.
	 */
	if (!stripesTruncated)
	{

		/*
		* Due to the AccessExclusive lock there's no danger that
		* new stripes be added beyond highestPhysicalAddress while
		* we're truncating.
		*/
		uint64_t newDataReservation = std::max<uint64_t>(GetHighestUsedAddress(rel->rd_locator) + 1,
										ColumnarFirstLogicalOffset);

		BlockNumber old_rel_pages = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);

		if (!ColumnarStorageTruncate(rel, newDataReservation))
		{
			UnlockRelation(rel, AccessExclusiveLock);
			return;
		}

		BlockNumber new_rel_pages = smgrnblocks(rel->rd_smgr, MAIN_FORKNUM);

		bioseq_report_pg_lazy(elevel, [&] {
			bioseq_report_pg(elevel)
				.msg("\"{}\": truncated {} to {} pages",
					 RelationGetRelationName(rel),
					 old_rel_pages, new_rel_pages)
				.detail("{}", pg_rusage_show(&ru0));
		});
	}

	/*
	 * We can release the exclusive lock as soon as we have truncated.
	 * Other backends can't safely access the relation until they have
	 * processed the smgr invalidation that smgrtruncate sent out ... but
	 * that should happen as part of standard invalidation processing once
	 * they acquire lock on the relation.
	 */
	UnlockRelation(rel, AccessExclusiveLock);
}

/*
 * ConditionalLockRelationWithTimeout tries to acquire a relation lock until
 * it either succeeds or timesout. It doesn't enter wait queue and instead it
 * sleeps between lock tries.
 *
 * This is based on the lock loop in lazy_truncate_heap().
 */
static bool
ConditionalLockRelationWithTimeout(Relation rel, LOCKMODE lockMode, int timeout,
								   int retryInterval, bool acquire)
{
	int lock_retry = 0;

	while (true)
	{
		if (ConditionalLockRelation(rel, lockMode))
		{
			break;
		}

		/*
		 * Check for interrupts while trying to (re-)acquire the lock
		 */
		CHECK_FOR_INTERRUPTS();

		if (!acquire && (++lock_retry > (timeout / retryInterval)))
		{
			return false;
		}

		pg_usleep(retryInterval * 1000L);
	}

	return true;
}


static bool
bioseq_scan_analyze_next_block(TableScanDesc scan,
								 ReadStream *stream)
{
	/*
	 * Our access method is not pages based, i.e. tuples are not confined
	 * to pages boundaries. So not much to do here. We return true anyway
	 * so acquire_sample_rows() in analyze.c would call our
	 * bioseq_scan_analyze_next_tuple() callback.
	 */
	return true;
}


static bool
bioseq_scan_analyze_next_tuple(TableScanDesc scan, TransactionId OldestXmin,
								 double *liverows, double *deadrows,
								 TupleTableSlot *slot)
{
	/* Disable the page cache while ANALYZE collects sample rows. */
	PageCacheDisableScope cache_guard{bioseq_enable_page_cache};

	/*
	 * Currently we don't do anything smart to reduce number of rows returned
	 * for ANALYZE. The TableAM API's ANALYZE functions are designed for page
	 * based access methods where it chooses random pages, and then reads
	 * tuples from those pages.
	 *
	 * We could do something like that here by choosing sample stripes or chunks,
	 * but getting that correct might need quite some work. Since bioseq_fdw's
	 * ANALYZE scanned all rows, as a starter we do the same here and scan all
	 * rows.
	 */
	if (bioseq_getnextslot(scan, ForwardScanDirection, slot))
	{
		(*liverows)++;
		return true;
	}

	return false;
}


static double
bioseq_index_build_range_scan(Relation columnarRelation,
								Relation indexRelation,
								IndexInfo *indexInfo,
								bool allow_sync,
								bool anyvisible,
								bool progress,
								BlockNumber start_blockno,
								BlockNumber numblocks,
								IndexBuildCallback callback,
								void *callback_state,
								TableScanDesc scan)
{
	if (start_blockno != 0 || numblocks != InvalidBlockNumber)
	{
		/*
		 * Columnar utility hook already errors out for BRIN indexes on columnar
		 * tables, but be on the safe side.
		 */
		bioseq_log<ERR, PG>( "BRIN indexes on columnar tables are not supported");
	}

	if (scan)
	{
		/*
		 * Parallel scans on columnar tables are already discardad by
		 * ColumnarGetRelationInfoHook but be on the safe side.
		 */
		bioseq_log<ERR, PG>( "parallel scans on columnar are not supported");
	}

	/* Disable the page cache for the duration of the index build. */
	PageCacheDisableScope cache_guard{bioseq_enable_page_cache};

	/*
	 * In a normal index build, we use SnapshotAny to retrieve all tuples. In
	 * a concurrent build or during bootstrap, we take a regular MVCC snapshot
	 * and index whatever's live according to that.
	 */
	TransactionId OldestXmin = InvalidTransactionId;
	if (!IsBootstrapProcessingMode() && !indexInfo->ii_Concurrent)
	{
		/* ignore lazy VACUUM's */
		OldestXmin = GetOldestNonRemovableTransactionId_compat(columnarRelation,
															   PROCARRAY_FLAGS_VACUUM);
	}

	Snapshot snapshot = { 0 };
	bool snapshotRegisteredByUs = false;

	/*
	 * For serial index build, we begin our own scan. We may also need to
	 * register a snapshot whose lifetime is under our direct control.
	 */
	if (!TransactionIdIsValid(OldestXmin))
	{
		snapshot = RegisterSnapshot(GetTransactionSnapshot());
		snapshotRegisteredByUs = true;
	}
	else
	{
		snapshot = SnapshotAny;
	}

	int nkeys = 0;
	ScanKeyData *scanKey = NULL;
	bool allowAccessStrategy = true;
	scan = table_beginscan_strat(columnarRelation, snapshot, nkeys, scanKey,
								 allowAccessStrategy, allow_sync);

	if (progress)
	{
		ColumnarReportTotalVirtualBlocks(columnarRelation, snapshot,
										 PROGRESS_SCAN_BLOCKS_TOTAL);
	}

	/*
	 * Set up execution state for predicate, if any.
	 * Note that this is only useful for partial indexes.
	 */
	EState *estate = CreateExecutorState();
	ExprContext *econtext = GetPerTupleExprContext(estate);
	econtext->ecxt_scantuple = table_slot_create(columnarRelation, NULL);
	ExprState *predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);

	double reltuples = ColumnarReadRowsIntoIndex(scan, indexRelation, indexInfo,
												 progress, callback, callback_state,
												 estate, predicate);
	table_endscan(scan);

	if (progress)
	{
		/* report the last "virtual" block as "done" */
		ColumnarReportTotalVirtualBlocks(columnarRelation, snapshot,
										 PROGRESS_SCAN_BLOCKS_DONE);
	}

	if (snapshotRegisteredByUs)
	{
		UnregisterSnapshot(snapshot);
	}

	ExecDropSingleTupleTableSlot(econtext->ecxt_scantuple);
	FreeExecutorState(estate);
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NULL;

	return reltuples;
}


/*
 * ColumnarReportTotalVirtualBlocks reports progress for index build based on
 * number of "virtual" blocks that given relation has.
 * "progressArrIndex" argument determines which entry in st_progress_param
 * array should be updated. In this case, we only expect PROGRESS_SCAN_BLOCKS_TOTAL
 * or PROGRESS_SCAN_BLOCKS_DONE to specify whether we want to report calculated
 * number of blocks as "done" or as "total" number of "virtual" blocks to scan.
 */
static void
ColumnarReportTotalVirtualBlocks(Relation relation, Snapshot snapshot,
								 int progressArrIndex)
{
	/*
	 * Indeed, columnar tables might have gaps between row numbers, e.g
	 * due to aborted transactions etc. Also, ItemPointer BlockNumber's
	 * for columnar tables don't actually correspond to actual disk blocks
	 * as in heapAM. For this reason, we call them as "virtual" blocks. At
	 * the moment, we believe it is better to report our progress based on
	 * this "virtual" block concept instead of doing nothing.
	 */
	Assert(progressArrIndex == PROGRESS_SCAN_BLOCKS_TOTAL ||
		   progressArrIndex == PROGRESS_SCAN_BLOCKS_DONE);
	BlockNumber nvirtualBlocks =
		ColumnarGetNumberOfVirtualBlocks(relation, snapshot);
	pgstat_progress_update_param(progressArrIndex, nvirtualBlocks);
}


/*
 * ColumnarGetNumberOfVirtualBlocks returns total number of "virtual" blocks
 * that given columnar table has based on based on ItemPointer BlockNumber's.
 */
static BlockNumber
ColumnarGetNumberOfVirtualBlocks(Relation relation, Snapshot snapshot)
{
	ItemPointerData highestItemPointer =
		ColumnarGetHighestItemPointer(relation, snapshot);
	if (!ItemPointerIsValid(&highestItemPointer))
	{
		/* table is empty according to our snapshot */
		return 0;
	}

	/*
	 * Since BlockNumber is 0-based, increment it by 1 to find the total
	 * number of "virtual" blocks.
	 */
	return ItemPointerGetBlockNumber(&highestItemPointer) + 1;
}


/*
 * ColumnarGetHighestItemPointer returns ItemPointerData for the tuple with
 * highest tid for given relation.
 * If given relation is empty, then returns invalid item pointer.
 */
static ItemPointerData
ColumnarGetHighestItemPointer(Relation relation, Snapshot snapshot)
{
	StripeMetadata *stripeWithHighestRowNumber =
		FindStripeWithHighestRowNumber(relation, snapshot);
	if (stripeWithHighestRowNumber == NULL ||
		StripeGetHighestRowNumber(stripeWithHighestRowNumber) == 0)
	{
		/* table is empty according to our snapshot */
		ItemPointerData invalidItemPtr;
		ItemPointerSetInvalid(&invalidItemPtr);
		return invalidItemPtr;
	}

	uint64_t highestRowNumber = StripeGetHighestRowNumber(stripeWithHighestRowNumber);
	return row_number_to_tid(highestRowNumber);
}


/*
 * ColumnarReadRowsIntoIndex builds indexRelation tuples by reading the
 * actual relation based on given "scan" and returns number of tuples
 * scanned to build the indexRelation.
 */
static double
ColumnarReadRowsIntoIndex(TableScanDesc scan, Relation indexRelation,
						  IndexInfo *indexInfo, bool progress,
						  IndexBuildCallback indexCallback,
						  void *indexCallbackState, EState *estate,
						  ExprState *predicate)
{
	double reltuples = 0;

	BlockNumber lastReportedBlockNumber = InvalidBlockNumber;

	ExprContext *econtext = GetPerTupleExprContext(estate);
	TupleTableSlot *slot = econtext->ecxt_scantuple;
	while (bioseq_getnextslot(scan, ForwardScanDirection, slot))
	{
		CHECK_FOR_INTERRUPTS();

		BlockNumber currentBlockNumber = ItemPointerGetBlockNumber(&slot->tts_tid);
		if (progress && lastReportedBlockNumber != currentBlockNumber)
		{
			/*
			 * bioseq_getnextslot guarantees that returned tuple will
			 * always have a greater ItemPointer than the ones we fetched
			 * before, so we directly use BlockNumber to report our progress.
			 */
			Assert(lastReportedBlockNumber == InvalidBlockNumber ||
				   currentBlockNumber >= lastReportedBlockNumber);
			pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE,
										 currentBlockNumber);
			lastReportedBlockNumber = currentBlockNumber;
		}

		MemoryContextReset(econtext->ecxt_per_tuple_memory);

		if (predicate != NULL && !ExecQual(predicate, econtext))
		{
			/* for partial indexes, discard tuples that don't satisfy the predicate */
			continue;
		}

		Datum indexValues[INDEX_MAX_KEYS];
		bool indexNulls[INDEX_MAX_KEYS];
		FormIndexDatum(indexInfo, slot, estate, indexValues, indexNulls);

		ItemPointerData itemPointerData = slot->tts_tid;

		/* currently, columnar tables can't have dead tuples */
		bool tupleIsAlive = true;
		indexCallback(indexRelation, &itemPointerData, indexValues, indexNulls,
					  tupleIsAlive, indexCallbackState);

		reltuples++;
	}

	return reltuples;
}


static void
bioseq_index_validate_scan(Relation columnarRelation,
							 Relation indexRelation,
							 IndexInfo *indexInfo,
							 Snapshot snapshot,
							 ValidateIndexState *
							 validateIndexState)
{
	ColumnarReportTotalVirtualBlocks(columnarRelation, snapshot,
									 PROGRESS_SCAN_BLOCKS_TOTAL);

	/*
	 * Set up execution state for predicate, if any.
	 * Note that this is only useful for partial indexes.
	 */
	EState *estate = CreateExecutorState();
	ExprContext *econtext = GetPerTupleExprContext(estate);
	econtext->ecxt_scantuple = table_slot_create(columnarRelation, NULL);
	ExprState *predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);

	int nkeys = 0;
	ScanKeyData *scanKey = NULL;
	bool allowAccessStrategy = true;
	bool allowSyncScan = false;
	TableScanDesc scan = table_beginscan_strat(columnarRelation, snapshot, nkeys, scanKey,
											   allowAccessStrategy, allowSyncScan);

	ColumnarReadMissingRowsIntoIndex(scan, indexRelation, indexInfo, estate,
									 predicate, validateIndexState);

	table_endscan(scan);

	/* report the last "virtual" block as "done" */
	ColumnarReportTotalVirtualBlocks(columnarRelation, snapshot,
									 PROGRESS_SCAN_BLOCKS_DONE);

	ExecDropSingleTupleTableSlot(econtext->ecxt_scantuple);
	FreeExecutorState(estate);
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NULL;
}


/*
 * ColumnarReadMissingRowsIntoIndex inserts the tuples that are not in
 * the index yet by reading the actual relation based on given "scan".
 */
static void
ColumnarReadMissingRowsIntoIndex(TableScanDesc scan, Relation indexRelation,
								 IndexInfo *indexInfo, EState *estate,
								 ExprState *predicate,
								 ValidateIndexState *validateIndexState)
{
	BlockNumber lastReportedBlockNumber = InvalidBlockNumber;

	bool indexTupleSortEmpty = false;
	ItemPointerData indexedItemPointerData;
	ItemPointerSetInvalid(&indexedItemPointerData);

	ExprContext *econtext = GetPerTupleExprContext(estate);
	TupleTableSlot *slot = econtext->ecxt_scantuple;
	while (bioseq_getnextslot(scan, ForwardScanDirection, slot))
	{
		CHECK_FOR_INTERRUPTS();

		ItemPointer columnarItemPointer = &slot->tts_tid;
		BlockNumber currentBlockNumber = ItemPointerGetBlockNumber(columnarItemPointer);
		if (lastReportedBlockNumber != currentBlockNumber)
		{
			/*
			 * bioseq_getnextslot guarantees that returned tuple will
			 * always have a greater ItemPointer than the ones we fetched
			 * before, so we directly use BlockNumber to report our progress.
			 */
			Assert(lastReportedBlockNumber == InvalidBlockNumber ||
				   currentBlockNumber >= lastReportedBlockNumber);
			pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE,
										 currentBlockNumber);
			lastReportedBlockNumber = currentBlockNumber;
		}

		validateIndexState->htups += 1;

		if (!indexTupleSortEmpty &&
			(!ItemPointerIsValid(&indexedItemPointerData) ||
			 ItemPointerCompare(&indexedItemPointerData, columnarItemPointer) < 0))
		{
			/*
			 * Skip indexed item pointers until we find or pass the current
			 * columnar relation item pointer.
			 */
			indexedItemPointerData =
				TupleSortSkipSmallerItemPointers(validateIndexState->tuplesort,
												 columnarItemPointer);
			indexTupleSortEmpty = !ItemPointerIsValid(&indexedItemPointerData);
		}

		if (!indexTupleSortEmpty &&
			ItemPointerCompare(&indexedItemPointerData, columnarItemPointer) == 0)
		{
			/* tuple is already covered by the index, skip */
			continue;
		}
		Assert(indexTupleSortEmpty ||
			   ItemPointerCompare(&indexedItemPointerData, columnarItemPointer) > 0);

		MemoryContextReset(econtext->ecxt_per_tuple_memory);

		if (predicate != NULL && !ExecQual(predicate, econtext))
		{
			/* for partial indexes, discard tuples that don't satisfy the predicate */
			continue;
		}

		Datum indexValues[INDEX_MAX_KEYS];
		bool indexNulls[INDEX_MAX_KEYS];
		FormIndexDatum(indexInfo, slot, estate, indexValues, indexNulls);

		Relation columnarRelation = scan->rs_rd;
		IndexUniqueCheck indexUniqueCheck =
			indexInfo->ii_Unique ? UNIQUE_CHECK_YES : UNIQUE_CHECK_NO;
		index_insert_compat(indexRelation, indexValues, indexNulls, columnarItemPointer,
							columnarRelation, indexUniqueCheck, false, indexInfo);

		validateIndexState->tups_inserted += 1;
	}
}


/*
 * TupleSortSkipSmallerItemPointers iterates given tupleSort until finding an
 * ItemPointer that is greater than or equal to given targetItemPointer and
 * returns that ItemPointer.
 * If such an ItemPointer does not exist, then returns invalid ItemPointer.
 *
 * Note that this function assumes given tupleSort doesn't have any NULL
 * Datum's.
 */
static ItemPointerData
TupleSortSkipSmallerItemPointers(Tuplesortstate *tupleSort, ItemPointer targetItemPointer)
{
	ItemPointerData tsItemPointerData;
	ItemPointerSetInvalid(&tsItemPointerData);

	while (!ItemPointerIsValid(&tsItemPointerData) ||
		   ItemPointerCompare(&tsItemPointerData, targetItemPointer) < 0)
	{
		bool forwardDirection = true;
		Datum *abbrev = NULL;
		Datum tsDatum;
		bool tsDatumIsNull;
		if (!tuplesort_getdatum_compat(tupleSort, forwardDirection, false,
									   &tsDatum, &tsDatumIsNull, abbrev))
		{
			ItemPointerSetInvalid(&tsItemPointerData);
			break;
		}

		Assert(!tsDatumIsNull);
		itemptr_decode(&tsItemPointerData, DatumGetInt64(tsDatum));

#ifndef USE_FLOAT8_BYVAL

		/*
		 * If int8 is pass-by-ref, we need to free Datum memory.
		 * See tuplesort_getdatum function's comment.
		 */
		pfree(DatumGetPointer(tsDatum));
#endif
	}

	return tsItemPointerData;
}


static uint64
bioseq_relation_size(Relation rel, ForkNumber forkNumber)
{
	uint64 nblocks = 0;

	/* InvalidForkNumber indicates returning the size for all forks */
	if (forkNumber == InvalidForkNumber)
	{
		for (int i = 0; i < MAX_FORKNUM; i++)
		{
			nblocks += smgrnblocks(RelationGetSmgr(rel), static_cast<ForkNumber>(i));
		}
	}
	else
	{
		nblocks = smgrnblocks(RelationGetSmgr(rel), forkNumber);
	}

	return nblocks * BLCKSZ;
}


static bool
bioseq_relation_needs_toast_table(Relation rel)
{
	return false;
}


static void
bioseq_estimate_rel_size(Relation rel, int32_t *attr_widths,
						   BlockNumber *pages, double *tuples,
						   double *allvisfrac)
{
	*pages = smgrnblocks(RelationGetSmgr(rel), MAIN_FORKNUM);
	*tuples = ColumnarTableRowCount(rel);

	/*
	 * Append-only, so everything is visible except in-progress or rolled-back
	 * transactions.
	 */
	*allvisfrac = 1.0;

	get_rel_data_width(rel, attr_widths);
}


static bool
bioseq_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate)
{
	bioseq_log<ERR, PG>( "bioseq_scan_sample_next_block not implemented");
}


static bool
bioseq_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate,
								TupleTableSlot *slot)
{
	bioseq_log<ERR, PG>( "bioseq_scan_sample_next_tuple not implemented");
}


static void
ColumnarXactCallback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_PREPARE:
		{
			/* nothing to do */
			break;
		}

		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
		{
			DiscardWriteStateForAllRels(GetCurrentSubTransactionId(), 0);
			CleanupReadStateCache(GetCurrentSubTransactionId());
			break;
		}

		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PARALLEL_PRE_COMMIT:
		case XACT_EVENT_PRE_PREPARE:
		{
			FlushWriteStateForAllRels(GetCurrentSubTransactionId(), 0);
			CleanupReadStateCache(GetCurrentSubTransactionId());
			break;
		}
	}
}


static void
ColumnarSubXactCallback(SubXactEvent event, SubTransactionId mySubid,
						SubTransactionId parentSubid, void *arg)
{
	switch (event)
	{
		case SUBXACT_EVENT_START_SUB:
		case SUBXACT_EVENT_COMMIT_SUB:
		{
			/* nothing to do */
			break;
		}

		case SUBXACT_EVENT_ABORT_SUB:
		{
			DiscardWriteStateForAllRels(mySubid, parentSubid);
			CleanupReadStateCache(mySubid);
			break;
		}

		case SUBXACT_EVENT_PRE_COMMIT_SUB:
		{
			FlushWriteStateForAllRels(mySubid, parentSubid);
			CleanupReadStateCache(mySubid);
			break;
		}
	}
}


export void
bioseq_tableam_init()
{
	bioseq_log<DBG>("[bioseq] pg: bioseq_tableam_init() entered");
	ColumnarTableSetOptions_hook_type **ColumnarTableSetOptions_hook_ptr =
		reinterpret_cast<ColumnarTableSetOptions_hook_type **>(find_rendezvous_variable(
			COLUMNAR_SETOPTIONS_HOOK_SYM));
	*ColumnarTableSetOptions_hook_ptr = &ColumnarTableSetOptions_hook;

	RegisterXactCallback(ColumnarXactCallback, NULL);
	RegisterSubXactCallback(ColumnarSubXactCallback, NULL);

	PrevObjectAccessHook = object_access_hook;
	object_access_hook = ColumnarTableAMObjectAccessHook;

	PrevProcessUtilityHook = ProcessUtility_hook ?
							 ProcessUtility_hook :
							 standard_ProcessUtility;
	ProcessUtility_hook = ColumnarProcessUtility;

	bioseq_customscan_init();

	TTSOpsColumnar = TTSOpsVirtual;
	TTSOpsColumnar.copy_heap_tuple = ColumnarSlotCopyHeapTuple;
}


/*
 * Get the number of chunks filtered out during the given scan.
 */
int64_t
ColumnarScanChunkGroupsFiltered(ColumnarScanDesc columnarScanDesc)
{
	ColumnarReadState *readState = columnarScanDesc->cs_readState;

	/* readState is initialized lazily */
	if (readState != NULL)
	{
		return ColumnarReadChunkGroupsFiltered(readState);
	}
	else
	{
		return 0;
	}
}


/*
 * Implementation of TupleTableSlotOps.copy_heap_tuple for TTSOpsColumnar.
 */
static HeapTuple
ColumnarSlotCopyHeapTuple(TupleTableSlot *slot)
{
	bioseq_log<DBG>("[bioseq] pg: ColumnarSlotCopyHeapTuple() entered");
	Assert(!TTS_EMPTY(slot));

	HeapTuple tuple = heap_form_tuple(slot->tts_tupleDescriptor,
									  slot->tts_values,
									  slot->tts_isnull);

	/* slot->tts_tid is filled in bioseq_getnextslot */
	tuple->t_self = slot->tts_tid;

	return tuple;
}


/*
 * ColumnarTableDropHook
 *
 * Clean-up resources for columnar tables.
 */
static void
ColumnarTableDropHook(Oid relid)
{
	bioseq_log<DBG>("[bioseq] pg: ColumnarTableDropHook() entered");
	/*
	 * Lock relation to prevent it from being dropped and to avoid
	 * race conditions in the next if block.
	 */
	LockRelationOid(relid, AccessShareLock);

	if (IsColumnarTableAmTable(relid))
	{
		/*
		 * Drop metadata. No need to drop storage here since for
		 * tableam tables storage is managed by postgres.
		 */
		Relation rel = table_open(relid, AccessExclusiveLock);
		RelFileLocator relfilelocator = rel->rd_locator;
		DeleteMetadataRows(relfilelocator);
		DeleteColumnarTableOptions(rel->rd_id, true);
		MarkRelfilenodeDropped(relfilelocator.relNumber, GetCurrentSubTransactionId());

		/* DOC0260 Phase A — cascade-drop sub-tables and sidecars under
		 * this relation's (db, spc, relfilenode) triple. */
		bioseq::storage::bioseq_default_storage_manager::drop_sub_tables_for_main(
			relfilelocator.dbOid, relfilelocator.spcOid,
			relfilelocator.relNumber);

		/* keep the lock since we did physical changes to the relation */
		table_close(rel, NoLock);
	}
}


/*
 * Reject AFTER ... FOR EACH ROW triggers on columnar tables.
 */
static void
ColumnarTriggerCreateHook(Oid tgid)
{
	bioseq_log<DBG>("[bioseq] pg: ColumnarTriggerCreateHook() entered");
	/*
	 * Fetch the pg_trigger tuple by the Oid of the trigger
	 */
	ScanKeyData skey[1];
	Relation tgrel = table_open(TriggerRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_trigger_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(tgid));

	SysScanDesc tgscan = systable_beginscan(tgrel, TriggerOidIndexId, true,
											SnapshotSelf, 1, skey);

	HeapTuple tgtup = systable_getnext(tgscan);

	if (!HeapTupleIsValid(tgtup))
	{
		table_close(tgrel, AccessShareLock);
		return;
	}

	Form_pg_trigger tgrec = reinterpret_cast<Form_pg_trigger>(GETSTRUCT(tgtup));

	Oid tgrelid = tgrec->tgrelid;
	int16_t tgtype = tgrec->tgtype;

	systable_endscan(tgscan);
	table_close(tgrel, AccessShareLock);

	if (TRIGGER_FOR_ROW(tgtype) && TRIGGER_FOR_AFTER(tgtype) &&
		IsColumnarTableAmTable(tgrelid))
	{
		bioseq_report<ERR, PG>().code(ERRCODE_FEATURE_NOT_SUPPORTED)
			.msg("Foreign keys and AFTER ROW triggers are not supported for columnar tables")
			.hint("Consider an AFTER STATEMENT trigger instead.");
	}
}


/*
 * Capture create/drop events and dispatch to the proper action.
 */
static void
ColumnarTableAMObjectAccessHook(ObjectAccessType access, Oid classId, Oid objectId,
								int subId, void *arg)
{
	if (PrevObjectAccessHook)
	{
		PrevObjectAccessHook(access, classId, objectId, subId, arg);
	}

	/* dispatch to the proper action */
	if (access == OAT_DROP && classId == RelationRelationId && !OidIsValid(subId))
	{
		ColumnarTableDropHook(objectId);
	}
	else if (access == OAT_POST_CREATE && classId == TriggerRelationId)
	{
		ColumnarTriggerCreateHook(objectId);
	}
}


/*
 * Utility hook for columnar tables.
 */
static void
ColumnarProcessUtility(PlannedStmt *pstmt,
					   const char *queryString,
					   bool readOnlyTree,
					   ProcessUtilityContext context,
					   ParamListInfo params,
					   QueryEnvironment *queryEnv,
					   DestReceiver *dest,
					   QueryCompletionCompat *completionTag)
{
	if (readOnlyTree)
	{
		pstmt = copyObject(pstmt);
	}

	Node *parsetree = pstmt->utilityStmt;

	if (IsA(parsetree, IndexStmt))
	{
		IndexStmt *indexStmt = reinterpret_cast<IndexStmt *>(parsetree);

		Relation rel = relation_openrv(indexStmt->relation,
									   indexStmt->concurrent ? ShareUpdateExclusiveLock :
									   ShareLock);

		if (rel->rd_tableam == GetColumnarTableAmRoutine())
		{
			if (!ColumnarSupportsIndexAM(indexStmt->accessMethod))
			{
				bioseq_report<ERR, PG>().code(ERRCODE_FEATURE_NOT_SUPPORTED)
					.msg("unsupported access method for the index on columnar table {} ({})",
						 RelationGetRelationName(rel), indexStmt->accessMethod);
			}
		}

		RelationClose(rel);
	}

	PrevProcessUtilityHook_compat(pstmt, queryString, false, context,
								  params, queryEnv, dest, completionTag);
}


/*
 * ColumnarSupportsIndexAM returns true if indexAM with given name is
 * supported by columnar tables.
 */
bool
ColumnarSupportsIndexAM(char *indexAMName)
{
	bioseq_log<DBG>("[bioseq] pg: ColumnarSupportsIndexAM() entered");
	return strncmp(indexAMName, "btree", NAMEDATALEN) == 0 ||
				strncmp(indexAMName, "hash", NAMEDATALEN) == 0 ||
				strncmp(indexAMName, "gin", NAMEDATALEN) == 0 ||
				strncmp(indexAMName, "gist", NAMEDATALEN) == 0 ||
				strncmp(indexAMName, "spgist", NAMEDATALEN) == 0 ||
				strncmp(indexAMName, "rum", NAMEDATALEN) == 0;
}


// /*
//  * IsColumnarTableAmTable returns true if relation has bioseq_tableam
//  * access method. This can be called before extension creation.
//  */
// bool
// IsColumnarTableAmTable(Oid relationId)
// {
// 	bioseq_log<DBG>("[bioseq] pg: IsColumnarTableAmTable() entered");
// 	if (!OidIsValid(relationId))
// 	{
// 		return false;
// 	}

// 	/*
// 	 * Lock relation to prevent it from being dropped &
// 	 * avoid race conditions.
// 	 */
// 	Relation rel = safe_relation_open(relationId, AccessShareLock);
// 	bool result = rel->rd_tableam == GetColumnarTableAmRoutine();
// 	relation_close(rel, NoLock);

// 	return result;
// }


export const TableAmRoutine bioseq_am_methods = {
	.type = T_TableAmRoutine,

	.slot_callbacks = bioseq_slot_callbacks,

	.scan_begin = bioseq_beginscan,
	.scan_end = bioseq_endscan,
	.scan_rescan = bioseq_rescan,
	.scan_getnextslot = bioseq_getnextslot,

	.parallelscan_estimate = bioseq_parallelscan_estimate,
	.parallelscan_initialize = bioseq_parallelscan_initialize,
	.parallelscan_reinitialize = bioseq_parallelscan_reinitialize,

	.index_fetch_begin = bioseq_index_fetch_begin,
	.index_fetch_reset = bioseq_index_fetch_reset,
	.index_fetch_end = bioseq_index_fetch_end,
	.index_fetch_tuple = bioseq_index_fetch_tuple,

	.tuple_fetch_row_version = bioseq_fetch_row_version,
	.tuple_get_latest_tid = bioseq_get_latest_tid,
	.tuple_tid_valid = bioseq_tuple_tid_valid,
	.tuple_satisfies_snapshot = bioseq_tuple_satisfies_snapshot,
	.index_delete_tuples = bioseq_index_delete_tuples,

	.tuple_insert = bioseq_tuple_insert,
	.tuple_insert_speculative = bioseq_tuple_insert_speculative,
	.tuple_complete_speculative = bioseq_tuple_complete_speculative,
	.multi_insert = bioseq_multi_insert,
	.tuple_delete = bioseq_tuple_delete,
	.tuple_update = bioseq_tuple_update,
	.tuple_lock = bioseq_tuple_lock,
	.finish_bulk_insert = bioseq_finish_bulk_insert,

	.relation_set_new_filelocator = bioseq_relation_set_new_filenode,
	.relation_nontransactional_truncate = bioseq_relation_nontransactional_truncate,
	.relation_copy_data = bioseq_relation_copy_data,
	.relation_copy_for_cluster = bioseq_relation_copy_for_cluster,
	.relation_vacuum = bioseq_vacuum_rel,
	.scan_analyze_next_block = bioseq_scan_analyze_next_block,
	.scan_analyze_next_tuple = bioseq_scan_analyze_next_tuple,
	.index_build_range_scan = bioseq_index_build_range_scan,
	.index_validate_scan = bioseq_index_validate_scan,

	.relation_size = bioseq_relation_size,
	.relation_needs_toast_table = bioseq_relation_needs_toast_table,

	.relation_estimate_size = bioseq_estimate_rel_size,

/*
 * PG-18 dropped the per-block callback (scan_bitmap_next_block) from the
 * TableAmRoutine when bitmap-heap-scan was reorganised; only the per-tuple
 * callback remains. Keep the PG-17 field for backwards compatibility.
 */
#if PG_VERSION_NUM < PG_VERSION_18
	.scan_bitmap_next_block = NULL,
#endif
	.scan_bitmap_next_tuple = NULL,
	.scan_sample_next_block = bioseq_scan_sample_next_block,
	.scan_sample_next_tuple = bioseq_scan_sample_next_tuple
};


const TableAmRoutine *
GetColumnarTableAmRoutine(void)
{
	bioseq_log<DBG>("[bioseq] pg: GetColumnarTableAmRoutine() entered");
	return &bioseq_am_methods;
}

POSTGRES_FN(bioseq_handler)
{
	bioseq_log<DBG>("[bioseq] pg: bioseq_handler() entered");
	PG_RETURN_POINTER(&bioseq_am_methods);
}


/*
 * detoast_values
 *
 * Detoast and decompress all values. If there's no work to do, return
 * original pointer; otherwise return a newly-allocated values array. Should
 * be called in per-tuple context.
 */
static Datum *
detoast_values(TupleDesc tupleDesc, Datum *orig_values, bool *isnull)
{
	int natts = tupleDesc->natts;

	/* copy on write to optimize for case where nothing is toasted */
	Datum *values = orig_values;

	for (int i = 0; i < tupleDesc->natts; i++)
	{
		if (!isnull[i] && TupleDescAttr(tupleDesc, i)->attlen == -1 &&
			VARATT_IS_EXTENDED(values[i]))
		{
			/* make a copy */
			if (values == orig_values)
			{
				values = make_array<Datum>(natts);
				memcpy(values, orig_values, sizeof(Datum) * natts);
			}

			/* will be freed when per-tuple context is reset */
			varlena *new_value = reinterpret_cast<varlena *>(DatumGetPointer(values[i]));
			new_value = detoast_attr(new_value);
			values[i] = PointerGetDatum(new_value);
		}
	}

	return values;
}


/*
 * ColumnarCheckLogicalReplication throws an error if the relation is
 * part of any publication. This should be called before any write to
 * a columnar table, because columnar changes are not replicated with
 * logical replication (similar to a row table without a replica
 * identity).
 */
static void
ColumnarCheckLogicalReplication(Relation rel)
{
	bool pubActionInsert = false;

	if (!is_publishable_relation(rel))
	{
		return;
	}

	{
		PublicationDesc pubdesc;

		RelationBuildPublicationDesc(rel, &pubdesc);
		pubActionInsert = pubdesc.pubactions.pubinsert;
	}

	if (pubActionInsert)
	{
		bioseq_report<ERR, PG>().code(ERRCODE_FEATURE_NOT_SUPPORTED)
			.msg("cannot insert into columnar table that is a part of a publication");
	}
}


/*
 * alter_bioseq_table_set is a UDF exposed in postgres to change settings on a columnar
 * table. Calling this function on a non-columnar table gives an error.
 *
 * sql syntax:
 *   pg_catalog.alter_bioseq_table_set(
 *        table_name regclass,
 *        chunk_group_row_limit int DEFAULT NULL,
 *        stripe_row_limit int DEFAULT NULL,
 *        compression name DEFAULT null)
 *
 * All arguments except the table name are optional. The UDF is supposed to be called
 * like:
 *   SELECT alter_bioseq_table_set('table', compression => 'pglz');
 *
 * This will only update the compression of the table, keeping all other settings the
 * same. Multiple settings can be changed at the same time by providing multiple
 * arguments. Calling the argument with the NULL value will be interperted as not having
 * provided the argument.
 */
POSTGRES_FN(alter_bioseq_table_set)
{
	Oid relationId = PG_GETARG_OID(0);

	Relation rel = table_open(relationId, AccessExclusiveLock); /* ALTER TABLE LOCK */
	if (!IsColumnarTableAmTable(relationId))
	{
		bioseq_log<ERR, PG>( "table {} is not a columnar table",
			   quote_identifier(RelationGetRelationName(rel)));
	}
	if (!object_ownercheck(RelationRelationId, relationId, GetUserId()))
	{
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE,
					   get_rel_name(relationId));
	}

	ColumnarOptions options = { 0 };
	if (!ReadColumnarOptions(relationId, &options))
	{
		bioseq_log<ERR, PG>( "unable to read current options for table");
	}

	/* chunk_group_row_limit => not null */
	if (!PG_ARGISNULL(1))
	{
		options.chunkRowCount = PG_GETARG_INT32(1);
		if (options.chunkRowCount < CHUNK_ROW_COUNT_MINIMUM ||
			options.chunkRowCount > CHUNK_ROW_COUNT_MAXIMUM)
		{
			bioseq_report<ERR, PG>()
				.msg("chunk group row count limit out of range")
				.hint("chunk group row count limit must be between {} and {}",
					  static_cast<uint64_t>(CHUNK_ROW_COUNT_MINIMUM),
					  static_cast<uint64_t>(CHUNK_ROW_COUNT_MAXIMUM));
		}
		bioseq_log_pg(DEBUG1, "updating chunk row count to {}", options.chunkRowCount);
	}

	/* stripe_row_limit => not null */
	if (!PG_ARGISNULL(2))
	{
		options.stripeRowCount = PG_GETARG_INT32(2);
		if (options.stripeRowCount < STRIPE_ROW_COUNT_MINIMUM ||
			options.stripeRowCount > STRIPE_ROW_COUNT_MAXIMUM)
		{
			bioseq_report<ERR, PG>()
				.msg("stripe row count limit out of range")
				.hint("stripe row count limit must be between {} and {}",
					  static_cast<uint64_t>(STRIPE_ROW_COUNT_MINIMUM),
					  static_cast<uint64_t>(STRIPE_ROW_COUNT_MAXIMUM));
		}
		bioseq_log_pg(DEBUG1, "updating stripe row count to {}", options.stripeRowCount);
	}

	/* compression => not null */
	if (!PG_ARGISNULL(3))
	{
		Name compressionName = PG_GETARG_NAME(3);
		options.compressionType = ParseCompressionType(NameStr(*compressionName));
		if (options.compressionType == COMPRESSION_TYPE_INVALID)
		{
			bioseq_log<ERR, PG>( "unknown compression type for columnar table: {}",
				   quote_identifier(NameStr(*compressionName)));
		}
		bioseq_log_pg(DEBUG1, "updating compression to {}",
			   CompressionTypeStr(options.compressionType));
	}

	/* compression_level => not null */
	if (!PG_ARGISNULL(4))
	{
		options.compressionLevel = PG_GETARG_INT32(4);
		if (options.compressionLevel < COMPRESSION_LEVEL_MIN ||
			options.compressionLevel > COMPRESSION_LEVEL_MAX)
		{
			bioseq_report<ERR, PG>()
				.msg("compression level out of range")
				.hint("compression level must be between {} and {}",
					  COMPRESSION_LEVEL_MIN,
					  COMPRESSION_LEVEL_MAX);
		}

		bioseq_log_pg(DEBUG1, "updating compression level to {}", options.compressionLevel);
	}

	if (ColumnarTableSetOptions_hook != NULL)
	{
		ColumnarTableSetOptions_hook(relationId, options);
	}

	SetColumnarOptions(relationId, &options);

	table_close(rel, NoLock);

	PG_RETURN_VOID();
}


/*
 * alter_bioseq_table_reset is a UDF exposed in postgres to reset the settings on a
 * columnar table. Calling this function on a non-columnar table gives an error.
 *
 * sql syntax:
 *   pg_catalog.alter_bioseq_table_re
 *   teset(
 *        table_name regclass,
 *        chunk_group_row_limit bool DEFAULT FALSE,
 *        stripe_row_limit bool DEFAULT FALSE,
 *        compression bool DEFAULT FALSE)
 *
 * All arguments except the table name are optional. The UDF is supposed to be called
 * like:
 *   SELECT alter_bioseq_table_set('table', compression => true);
 *
 * All options set to true will be reset to the default system value.
 */
POSTGRES_FN(alter_bioseq_table_reset)
{
	Oid relationId = PG_GETARG_OID(0);

	Relation rel = table_open(relationId, AccessExclusiveLock); /* ALTER TABLE LOCK */
	if (!IsColumnarTableAmTable(relationId))
	{
		bioseq_log<ERR, PG>( "table {} is not a columnar table",
			   quote_identifier(RelationGetRelationName(rel)));
	}

	if (!object_ownercheck(RelationRelationId, relationId, GetUserId()))
	{
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE,
					   get_rel_name(relationId));
	}

	ColumnarOptions options = { 0 };
	if (!ReadColumnarOptions(relationId, &options))
	{
		bioseq_log<ERR, PG>( "unable to read current options for table");
	}

	/* chunk_group_row_limit => true */
	if (!PG_ARGISNULL(1) && PG_GETARG_BOOL(1))
	{
		options.chunkRowCount = bioseq_chunk_group_row_limit;
		bioseq_log_pg(DEBUG1, "resetting chunk row count to {}", options.chunkRowCount);
	}

	/* stripe_row_limit => true */
	if (!PG_ARGISNULL(2) && PG_GETARG_BOOL(2))
	{
		options.stripeRowCount = bioseq_stripe_row_limit;
		bioseq_log_pg(DEBUG1, "resetting stripe row count to {}", options.stripeRowCount);
	}

	/* compression => true */
	if (!PG_ARGISNULL(3) && PG_GETARG_BOOL(3))
	{
		options.compressionType = static_cast<CompressionType>(bioseq_compression);
		bioseq_log_pg(DEBUG1, "resetting compression to {}",
			   CompressionTypeStr(options.compressionType));
	}

	/* compression_level => true */
	if (!PG_ARGISNULL(4) && PG_GETARG_BOOL(4))
	{
		options.compressionLevel = bioseq_compression_level;
		bioseq_log_pg(DEBUG1, "reseting compression level to {}", bioseq_compression_level);
	}

	if (ColumnarTableSetOptions_hook != NULL)
	{
		ColumnarTableSetOptions_hook(relationId, options);
	}

	SetColumnarOptions(relationId, &options);

	table_close(rel, NoLock);

	PG_RETURN_VOID();
}


/*
 * upgrade_bioseq_storage - upgrade columnar storage to the current
 * version.
 *
 * DDL:
 *   CREATE OR REPLACE FUNCTION upgrade_bioseq_storage(rel regclass)
 *     RETURNS VOID
 *     STRICT
 *     LANGUAGE c AS 'MODULE_PATHNAME', 'upgrade_bioseq_storage';
 */
POSTGRES_FN(upgrade_bioseq_storage)
{
	Oid relid = PG_GETARG_OID(0);

	/*
	 * ACCESS EXCLUSIVE LOCK is not required by the low-level routines, so we
	 * can take only an ACCESS SHARE LOCK. But all access to non-current
	 * columnar tables will fail anyway, so it's better to take ACCESS
	 * EXCLUSIVE LOCK now.
	 */
	Relation rel = table_open(relid, AccessExclusiveLock);
	if (!IsColumnarTableAmTable(relid))
	{
		bioseq_log<ERR, PG>( "table {} is not a columnar table",
			   quote_identifier(RelationGetRelationName(rel)));
	}

	ColumnarStorageUpdateIfNeeded(rel, true);

	table_close(rel, AccessExclusiveLock);
	PG_RETURN_VOID();
}


/*
 * downgrade_bioseq_storage - downgrade columnar storage to the
 * current version.
 *
 * DDL:
 *   CREATE OR REPLACE FUNCTION downgrade_bioseq_storage(rel regclass)
 *     RETURNS VOID
 *     STRICT
 *     LANGUAGE c AS 'MODULE_PATHNAME', 'downgrade_bioseq_storage';
 */
POSTGRES_FN(downgrade_bioseq_storage)
{
	Oid relid = PG_GETARG_OID(0);

	/*
	 * ACCESS EXCLUSIVE LOCK is not required by the low-level routines, so we
	 * can take only an ACCESS SHARE LOCK. But all access to non-current
	 * columnar tables will fail anyway, so it's better to take ACCESS
	 * EXCLUSIVE LOCK now.
	 */
	Relation rel = table_open(relid, AccessExclusiveLock);
	if (!IsColumnarTableAmTable(relid))
	{
		bioseq_log<ERR, PG>( "table {} is not a columnar table",
			   quote_identifier(RelationGetRelationName(rel)));
	}

	ColumnarStorageUpdateIfNeeded(rel, false);

	table_close(rel, AccessExclusiveLock);
	PG_RETURN_VOID();
}

struct StripeHole
{
	uint64_t fileOffset;
	uint64_t dataLength;
};

/*
 * HolesForRelation returns a list of holes in the Relation id in the current
 * MemoryContext.
 */
static List *HolesForRelation(Relation rel)
{
	List *holes = NIL;

	/* Get current columnar options */
	ColumnarOptions columnarOptions = { 0 };
	ReadColumnarOptions(rel->rd_id, &columnarOptions);

	ListCell *lc = NULL;

	List *stripeMetadataList = StripesForRelfilenode(rel->rd_locator, ForwardScanDirection);
	uint64_t lastMinimalOffset = ColumnarFirstLogicalOffset;

	for (auto stripeMetadata : PList<StripeMetadata>(stripeMetadataList))
	{
		/* Arbitrary check to see if the offset will be large enough, 10000 bytes is what was chosen. */
		if (stripeMetadata->fileOffset == lastMinimalOffset || stripeMetadata->fileOffset - lastMinimalOffset < 10000) {
			lastMinimalOffset = stripeMetadata->fileOffset + stripeMetadata->dataLength;
			continue;
		} else {
			StripeHole * hole = make0<StripeHole>();
			hole->fileOffset = lastMinimalOffset;
			hole->dataLength = stripeMetadata->fileOffset + stripeMetadata->dataLength - lastMinimalOffset;
			lastMinimalOffset = stripeMetadata->fileOffset + stripeMetadata->dataLength;

			holes = lappend(holes, hole);
		}
	}

	return holes;
}



/*
 * vacuum_signal_handler - catch any signals sent during the UDF vacuum, and
 * attempt to clean up as much as we can and bail as cleanly as possible.
 */
static void vacuum_signal_handler(int signal)
{
	bioseq_log_pg(DEBUG3, "Received signal {} during a vacuum request", signal);

	/* Set up the ability to bail from the vacuum, with needed information. */
	need_to_bail = true;
	last_signal = signal;
}

/*
 * vacuum_bioseq_table
 */
struct StripeVacuumCandidate
{
	uint64_t stripeId;
	int32_t stripeMetadataIndex;
	uint32_t candidateTotalSize;
	uint32_t activeRows;
	StripeMetadata *stripeMetadata;
};

POSTGRES_FN(vacuum_bioseq_table)
{
	Oid relid = PG_GETARG_OID(0);
	Relation rel = RelationIdGetRelation(relid);
	TupleDesc tupleDesc = RelationGetDescr(rel);
	MemoryContext oldcontext = CurrentMemoryContext;
	uint32_t stripeCount = PG_GETARG_UINT32(1);
	uint32_t progress = 0;
	bool completelyDone = false;
	sact action;

	/*
	 * Disable the page cache for the duration of the vacuum. The guard
	 * restores the previous state on every exit path — including the
	 * early PG_RETURN_* returns scattered through the signal-handling
	 * branches below — so individual restore calls are no longer needed.
	 */
	PageCacheDisableScope cache_guard{bioseq_enable_page_cache};

	/*
	 * Set up signal handlers for any incoming signals during the vacuum,
	 * killing during a write could cause corruption.  Give us time to
	 * clean as much as we can up before letting the signal pass on.
	 */
	need_to_bail = false;
	last_signal = 0;

	action.sa_handler = vacuum_signal_handler;
	sigemptyset (&action.sa_mask);
	action.sa_flags = 0;

	sigaction(SIGINT, &action, &int_action);
	sigaction(SIGTERM, &action, &trm_action);
	sigaction(SIGABRT, &action, &abt_action);
	sigaction(SIGKILL, &action, &kil_action);

	MemoryContext vacuum_context = AllocSetContextCreate(CurrentMemoryContext,
						"Columnar Vacuum Context",
						ALLOCSET_SMALL_SIZES);

	oldcontext = MemoryContextSwitchTo(vacuum_context);

	if (tupleDesc->natts == 0)
	{
		bioseq_log_pg(INFO, "\"{}\": stopping vacuum due to zero column table",
			   RelationGetRelationName(rel));

		MemoryContextSwitchTo(oldcontext);
		PG_RETURN_VOID();
	}

	LockRelation(rel, ExclusiveLock);

	/* Get current columnar options */
	ColumnarOptions columnarOptions = { 0 };
	ReadColumnarOptions(rel->rd_id, &columnarOptions);

	/* Get all stripes in order. */
	List *stripeMetadataList = StripesForRelfilenode(rel->rd_locator, ForwardScanDirection);
	List *vacuumCandidatesStripeList = NIL;

	/* Empty table nothing to do */
	if (stripeMetadataList == NIL)
	{
		bioseq_log_pg(INFO, "\"{}\": stopping vacuum due to empty table",
			   RelationGetRelationName(rel));

		/* Close the relation as we are done with it */
		RelationClose(rel);

		MemoryContextSwitchTo(oldcontext);
		PG_RETURN_VOID();
	}


	int stripeMetadataIndex = 0;

	bioseq_log_pg(DEBUG3, "Beginning combination of stripes");

	/*
	 * Get a list of all stripes that can be combined into larger stripes.
	 */
	for (auto stripeMetadata : PList<StripeMetadata>(stripeMetadataList))
	{
		/* We should not consider the last stripe. */
		if (stripeMetadata == static_cast<StripeMetadata*>(llast(stripeMetadataList)))
		{
			break;
		}

		uint32_t stripeDeletedRows = DeletedRowsForStripe(rel->rd_locator,
												 		stripeMetadata->chunkCount,
														stripeMetadata->id);
		float percentageOfDeleteRows =
			(float)stripeDeletedRows / (float)(stripeMetadata->rowCount);

		/*
		 * If inspected stripe has less than 0.5 percent of maximum strip row size
		 * or percentage of deleted rows is less than 20% we will skip this stripe
		 * for vacuum.
		*/
		if ((stripeMetadata->rowCount > columnarOptions.stripeRowCount * 0.5) &&
			percentageOfDeleteRows <= 0.2f)
		{
			continue;
		}

		StripeVacuumCandidate *vacuumCandidate = make0<StripeVacuumCandidate>();
		vacuumCandidate->stripeMetadataIndex = stripeMetadataIndex;
		vacuumCandidate->candidateTotalSize = stripeMetadata->rowCount - stripeDeletedRows;
		vacuumCandidate->stripeMetadata = stripeMetadata;
		vacuumCandidate->activeRows = stripeMetadata->rowCount - stripeDeletedRows;

		vacuumCandidatesStripeList = lappend(vacuumCandidatesStripeList, vacuumCandidate);
	}

	/* We need all columns. */
	int natts = rel->rd_att->natts;
	PBitmapset attr_needed = PBitmapset::full_range(natts);

	/* No quals for table rewrite */
	List *scanQual = NIL;

	/* Use SnapshotAny when re-writing table as heapAM does. */
	Snapshot snapshot = SnapshotAny;

	MemoryContext scanContext = CreateColumnarScanMemoryContext();
	bool randomAccess = true;

	ColumnarWriteState *writeState = ColumnarBeginWrite(rel->rd_locator,
											columnarOptions,
											tupleDesc);

	/*
	 * Combine the vacuum candidates into their own stripes appended to the rel, this
	 * should clear out any space from partial stripes to make space to move stripes into.
	 */
	for (auto vacuumCandidate : PList<StripeVacuumCandidate>(vacuumCandidatesStripeList))
	{
		MemoryContext combineContext = AllocSetContextCreate(CurrentMemoryContext,
						"Stripe Combine Context", ALLOCSET_DEFAULT_SIZES);
		MemoryContext previousContext = MemoryContextSwitchTo(combineContext);

		ColumnarReadState *readState = init_bioseq_read_state(rel, tupleDesc,
															attr_needed, scanQual,
															scanContext, snapshot,
															randomAccess,
															NULL);


		ColumnarSetStripeReadState(readState,
								vacuumCandidate->stripeMetadata);

		Datum *values = make_array<Datum>(tupleDesc->natts);
		bool *nulls = make_array<bool>(tupleDesc->natts);

		int32_t rowCount = 0;

		while (rowCount < vacuumCandidate->activeRows && ColumnarReadNextRow(readState, values, nulls, NULL))
		{
			ColumnarWriteRow(writeState, values, nulls);
			rowCount++;
		}

		DeleteMetadataRowsForStripeId(rel->rd_locator, vacuumCandidate->stripeMetadata->id);
		ColumnarEndRead(readState);

		pfree(values);
		pfree(nulls);

		progress++;

		/* Check if a signal has been sent, if so close out and deal with it. */
		if (need_to_bail)
		{
			ColumnarEndWrite(writeState);
			UnlockRelation(rel, ExclusiveLock);
			relation_close(rel, NoLock);

			need_to_bail = 0;
			/* Reset the signal handlers back to their original state. */
			sigaction(SIGINT, &int_action, NULL);
			sigaction(SIGTERM, &trm_action, NULL);
			sigaction(SIGABRT, &abt_action, NULL);
			sigaction(SIGKILL, &kil_action, NULL);

			/* Call any missed signal handlers. */
			if (last_signal == SIGABRT && abt_action.sa_handler)
			{
				abt_action.sa_handler(SIGABRT);
			} else if (last_signal == SIGTERM && trm_action.sa_handler)
			{
				trm_action.sa_handler(SIGTERM);
			} else if (last_signal == SIGINT && int_action.sa_handler)
			{
				int_action.sa_handler(SIGINT);
			} else if (last_signal == SIGKILL && kil_action.sa_handler)
			{
				kil_action.sa_handler(SIGKILL);
			}
			PG_RETURN_NULL();
		}

		if (stripeCount && progress >= stripeCount)
		{
			completelyDone = true;
			break;
		}

		MemoryContextSwitchTo(previousContext);
		MemoryContextDelete(combineContext);
	}
	/*
	 * We have finished the first route of writes, let's drop our lock until we are
	 * ready for the next stage: compaction.
	 */
	ColumnarEndWrite(writeState);

	bioseq_log_pg(DEBUG3, "Combined {} stripes", progress);

	if (completelyDone)
	{
		relation_close(rel, NoLock);
		TruncateColumnar(rel, DEBUG3);
		UnlockRelation(rel, ExclusiveLock);
		PG_RETURN_UINT32(progress);
	}

	bioseq_log_pg(DEBUG3, "Beginning reorganization");
	/*
	 * Continually iterate through the holes, finding where we can place
	 * old stripes.
	 */
	bool done = false;

	uint32_t relocationProgress = 0;

	while (!done)
	{
		MemoryContext rewriteContext = AllocSetContextCreate(CurrentMemoryContext,
					"Stripe Rewrite Context", ALLOCSET_DEFAULT_SIZES);
		MemoryContext previousContext = MemoryContextSwitchTo(rewriteContext);

		/* Check if a signal has been sent, if so close out and deal with it. */
		if (need_to_bail)
		{
			relation_close(rel, NoLock);
			TruncateColumnar(rel, DEBUG3);
			UnlockRelation(rel, ExclusiveLock);
			ForceSyncCommit();

			need_to_bail = 0;
			/* Reset the signal handlers back to their original state. */
			sigaction(SIGINT, &int_action, NULL);
			sigaction(SIGTERM, &trm_action, NULL);
			sigaction(SIGABRT, &abt_action, NULL);
			sigaction(SIGKILL, &kil_action, NULL);

			/* Call any missed signal handlers. */
			if (last_signal == SIGABRT && abt_action.sa_handler)
			{
				abt_action.sa_handler(SIGABRT);
			} else if (last_signal == SIGTERM && trm_action.sa_handler)
			{
				trm_action.sa_handler(SIGTERM);
			} else if (last_signal == SIGINT && int_action.sa_handler)
			{
				int_action.sa_handler(SIGINT);
			} else if (last_signal == SIGKILL && kil_action.sa_handler)
			{
				kil_action.sa_handler(SIGKILL);
			}
			PG_RETURN_NULL();
		}

		/*
		 * Get a List of empty spaces to fill with later stripes.
		 */
		List *holes = HolesForRelation(rel);

		if (list_length(holes) == 0)
		{
			done = true;
			continue;
		}

		/*
		 * Iterate through the holes, moving later slices into the holes.
		 */
		for (auto hole : PList<StripeHole>(holes))
		{
			/* Reload the metadata. */
			stripeMetadataList = StripesForRelfilenode(rel->rd_locator, ForwardScanDirection);
			for (auto stripe : PList<StripeMetadata>(stripeMetadataList))
			{
				/* If we are marked done, we should simply drop out. */
				if (done)
				{
					break;
				}

				/* Find one that will fit, and move it. */
				if (hole->fileOffset && stripe->dataLength < hole->dataLength && stripe->fileOffset > hole->fileOffset)
				{
					/* Read a copy of the old row. */
					char * data = make1<char>(stripe->dataLength);
					ColumnarStorageRead(rel, stripe->fileOffset, data, stripe->dataLength);

					/* Write the data to the new offset. */
					ColumnarStorageWrite(rel, hole->fileOffset, data, stripe->dataLength);

					/* Update the stripe metadata for the moved stripe. */
					RewriteStripeMetadataRowWithNewValues(rel, stripe->id, stripe->dataLength, hole->fileOffset, stripe->rowCount, stripe->chunkCount);

					relocationProgress++;

					pfree(data);

					if (relocationProgress >= 1)
					{
						done = true;
					}

					break;
				}
			}


			if (relocationProgress >= 1)
			{
				done = true;
			}

			holes = HolesForRelation(rel);
		}

		MemoryContextSwitchTo(previousContext);
		MemoryContextDelete(rewriteContext);
	}

	bioseq_log_pg(DEBUG3, "Ending reorganization");

	relation_close(rel, NoLock);

	TruncateColumnar(rel, DEBUG3);
	UnlockRelation(rel, ExclusiveLock);

	MemoryContextSwitchTo(oldcontext);

	/* Reset the signal handlers back to their original state. */
	sigaction(SIGINT, &int_action, NULL);
	sigaction(SIGTERM, &trm_action, NULL);
	sigaction(SIGABRT, &abt_action, NULL);
	sigaction(SIGKILL, &kil_action, NULL);
	PG_RETURN_UINT32(progress + relocationProgress);
}

/*
 * Data storage for columnar stats.
 */
struct ColumnarStats
{
	uint64_t stripeId;
	uint64_t fileOffset;
	uint32_t rowCount;
	uint32_t deletedRows;
	uint32_t chunkCount;
	uint32_t dataLength;
};

/* We return 6 columns. */
constexpr int STATS_INFO_NATTS = 6;

POSTGRES_FN(bioseq_stats)
{
	FuncCallContext *funcctx;
	int call_cntr;
	int max_calls;
	TupleDesc tupdesc;
	AttInMetadata *attinmeta;


	/* If this is the first call in, set up the data. */
	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;

		Oid relid = PG_GETARG_OID(0);
		Relation rel = RelationIdGetRelation(relid);

		/* Function context for persistance. */
		funcctx = SRF_FIRSTCALL_INIT();

		/* Use the SRF memory context */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Retrieve the stripe metadata. */
		List *stripeMetadataList = StripesForRelfilenode(rel->rd_locator, ForwardScanDirection);
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
				bioseq_report<ERR, PG>().code(ERRCODE_FEATURE_NOT_SUPPORTED)
					.msg("function returning record called in context "
						 "that cannot accept type record");

		attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funcctx->attinmeta = attinmeta;

		/* Set up the stats. */
		ColumnarStats *stats = make_array<ColumnarStats>(list_length(stripeMetadataList));
		funcctx->max_calls = list_length(stripeMetadataList);

		/* Iterate through the stripes to get a copy of the important data. */
		for (int i = 0; i < funcctx->max_calls; i++)
		{
			StripeMetadata *data = static_cast<StripeMetadata*>(list_nth(stripeMetadataList, i));

			stats[i].stripeId = data->id;
			stats[i].fileOffset = data->fileOffset;
			stats[i].rowCount = data->rowCount;
			stats[i].chunkCount = data->chunkCount;
			stats[i].dataLength = data->dataLength;
			stats[i].deletedRows = DeletedRowsForStripe(rel->rd_locator,
												 		data->chunkCount,
														data->id);
		}

		funcctx->user_fctx = stats;

		table_close(rel, NoLock);

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();

	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;
	ColumnarStats *stats = static_cast<ColumnarStats*>(funcctx->user_fctx);

	if (call_cntr < max_calls)
	{
		Datum result;

		Datum values[STATS_INFO_NATTS] = { 0 };
		bool nulls[STATS_INFO_NATTS] = { 0 };

		get_call_result_type(fcinfo, NULL, &tupdesc);

		values[0] = Int64GetDatum(stats[call_cntr].stripeId);
		values[1] = Int64GetDatum(stats[call_cntr].fileOffset);
		values[2] = Int32GetDatum(stats[call_cntr].rowCount);
		values[3] = Int32GetDatum(stats[call_cntr].deletedRows);
		values[4] = Int32GetDatum(stats[call_cntr].chunkCount);
		values[5] = Int32GetDatum(stats[call_cntr].dataLength);

		HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);
		result = HeapTupleGetDatum(tuple);

		SRF_RETURN_NEXT(funcctx, result);
	}
	else
	{
		SRF_RETURN_DONE(funcctx);
	}
}

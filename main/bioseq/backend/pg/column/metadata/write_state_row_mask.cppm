/*-------------------------------------------------------------------------
 *
 * write_state_row_mask.cppm
 *
 * Transaction-scoped management of RowMaskWriteStateEntry records used to
 * aggregate row-mask updates (DELETEs against columnar stripes). Commits
 * flush the pending mask deltas into bioseq.row_mask via FlushRowMaskCache.
 *
 * Structurally mirrors write_state_management: backing storage is a
 * pg_utils::TxnScopedHtab; per-subtransaction state lives on a linked list
 * of frames hanging off each map entry.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"


export module bioseq.storage.metadata:write_state_row_mask;

import :core;            // FlushRowMaskCache, UpdateChunkGroupDeletedRows, BioseqColumnRowMask*RelationId
import bioseq.config;           // DOC0322 §6.5 — Anum_bioseq_row_mask_* (was config.h macros via columnar.h)
import bioseq.common.mlogging;
import bioseq.pg_utils;

/*
 * Per-subtransaction node. rowMaskWriteStateEntryList accumulates the
 * pending row-mask deltas; lastRowMaskWriteStateEntry is a one-slot cache
 * so repeated lookups for the same row number hit the fast path.
 */
struct RowMaskSubXidWriteFrame
{
	SubTransactionId subXid;
	PList<RowMaskWriteStateEntry> rowMaskWriteStateEntryList;
	RowMaskWriteStateEntry* lastRowMaskWriteStateEntry;
	RowMaskSubXidWriteFrame* next;
};

/*
 * Per-relfilelocator entry. dropped/dropSubXid mirror the pattern used in
 * write_state_management so drops performed inside a subtransaction can be
 * elevated or reverted on commit/abort.
 */
struct RowMaskWriteStateMapEntry
{
	Oid relfilelocator;

	bool dropped;
	SubTransactionId dropSubXid;

	RowMaskSubXidWriteFrame* writeStateStack;
};

static TxnScopedHtab<RowMaskWriteStateMapEntry> RowMaskStateCache{"row mask cache map"};


/*
 * InitRowMaskEntry allocates a fresh RowMaskWriteStateEntry, copies the
 * caller-supplied mask (so we own the storage) and zero-initialises the
 * remaining fields. Caller is responsible for filling
 * startRowNumber/endRowNumber/chunkId/stripeId/deletedRows.
 */
static RowMaskWriteStateEntry*
InitRowMaskEntry(uint64_t storage_id, bytea* mask)
{
	RowMaskWriteStateEntry* row_mask = make0<RowMaskWriteStateEntry>();

	row_mask->storageId = storage_id;
	row_mask->mask = reinterpret_cast<bytea*>(palloc0(VARSIZE(mask) + VARHDRSZ));
	memcpy(row_mask->mask, mask, VARSIZE(mask) + VARHDRSZ);

	return row_mask;
}

/*
 * RowMaskFlushPendingWriteState iterates a list of accumulated entries and
 * flushes each one into bioseq.row_mask. Also updates chunk-group deleted
 * counters so query planning stays in sync.
 */
static void
RowMaskFlushPendingWriteState(const PList<RowMaskWriteStateEntry>& row_mask_write_state_list)
{
	for (auto* row_mask_cache : row_mask_write_state_list)
	{
		FlushRowMaskCache(row_mask_cache);
		UpdateChunkGroupDeletedRows(row_mask_cache->storageId, row_mask_cache->stripeId,
									row_mask_cache->chunkId, row_mask_cache->deletedRows);
		pfree(row_mask_cache->mask);
	}
}


/*
 * RowMaskInitWriteState locates (or creates) the stack entry for the given
 * relation/subtransaction and appends a new RowMaskWriteStateEntry built
 * from the supplied mask. Returns the entry; caller fills remaining fields
 * (chunk/stripe identifiers, row range) before the entry is flushed.
 */
export RowMaskWriteStateEntry*
RowMaskInitWriteState(Oid relfilelocator,
					  uint64 storage_id,
					  SubTransactionId current_sub_xid,
					  bytea* row_mask)
{
	bioseq_log<DBG>("[bioseq] pg: RowMaskInitWriteState() relfilelocator={} storage_id={} sub_xid={}", relfilelocator, storage_id, current_sub_xid);

	bool found_existing = false;
	RowMaskWriteStateMapEntry* hash_entry =
		RowMaskStateCache.find_or_insert(&relfilelocator, &found_existing);

	if (!found_existing)
	{
		hash_entry->writeStateStack = nullptr;
		hash_entry->dropped = false;
	}

	Assert(!hash_entry->dropped);

	RowMaskSubXidWriteFrame* stack_entry = nullptr;
	if (hash_entry->writeStateStack != nullptr &&
		hash_entry->writeStateStack->subXid == current_sub_xid)
	{
		stack_entry = hash_entry->writeStateStack;
	}

	if (stack_entry == nullptr)
	{
		stack_entry = RowMaskStateCache.alloc_in_context<RowMaskSubXidWriteFrame>();
		stack_entry->subXid = current_sub_xid;
		stack_entry->next = hash_entry->writeStateStack;
		stack_entry->lastRowMaskWriteStateEntry = nullptr;
		hash_entry->writeStateStack = stack_entry;
	}

	RowMaskWriteStateEntry* row_mask_write_state = nullptr;
	{
		MemoryContextScope context_guard(RowMaskStateCache.context());
		row_mask_write_state = InitRowMaskEntry(storage_id, row_mask);
		stack_entry->rowMaskWriteStateEntryList.append(row_mask_write_state);
	}

	return row_mask_write_state;
}

/*
 * RowMaskFlushWriteStateForRelfilenode flushes and clears the accumulated
 * row-mask entries for the given relation in the current subtransaction.
 * Leaves the stack entry in place (without its list) so subsequent
 * operations can continue accumulating.
 */
export void RowMaskFlushWriteStateForRelfilenode(Oid relfilelocator,
										  SubTransactionId current_sub_xid) {
	RowMaskWriteStateMapEntry* entry = RowMaskStateCache.find(&relfilelocator);
	Assert(!entry || !entry->dropped);

	if (entry != nullptr && entry->writeStateStack != nullptr)
	{
		RowMaskSubXidWriteFrame* stack_entry = entry->writeStateStack;
		if (stack_entry->subXid == current_sub_xid)
		{
			RowMaskFlushPendingWriteState(stack_entry->rowMaskWriteStateEntryList);
			stack_entry->rowMaskWriteStateEntryList.clear();
		}
	}
}


/*
 * RowMaskFindWriteState returns the accumulated row-mask entry that
 * contains the given row id, scanning the per-subtransaction list and
 * consulting lastRowMaskWriteStateEntry first as a fast path.
 */
export RowMaskWriteStateEntry*
RowMaskFindWriteState(Oid relfilelocator, SubTransactionId current_sub_xid,
					  uint64 row_id)
{
	RowMaskWriteStateMapEntry* hash_entry = RowMaskStateCache.find(&relfilelocator);
	if (hash_entry == nullptr)
	{
		return nullptr;
	}

	for (auto* stack_frame : IntrusiveList<RowMaskSubXidWriteFrame>(hash_entry->writeStateStack))
	{
		if (stack_frame->subXid != current_sub_xid)
		{
			continue;
		}
		if (stack_frame->lastRowMaskWriteStateEntry != nullptr &&
			stack_frame->lastRowMaskWriteStateEntry->startRowNumber <= row_id &&
			stack_frame->lastRowMaskWriteStateEntry->endRowNumber >= row_id)
		{
			return stack_frame->lastRowMaskWriteStateEntry;
		}

		for (auto* row_mask : stack_frame->rowMaskWriteStateEntryList)
		{
			if (row_mask->startRowNumber <= row_id && row_mask->endRowNumber >= row_id)
			{
				stack_frame->lastRowMaskWriteStateEntry = row_mask;
				return row_mask;
			}
		}
	}

	return nullptr;
}


/*
 * RowMaskPopWriteStateForAllRels pops the top-of-stack entry for every
 * relation whose top matches the given subtransaction. Flushes pending
 * mask writes on commit; drops are propagated up or reverted per commit.
 */
export void
RowMaskPopWriteStateForAllRels(SubTransactionId current_sub_xid,
							   SubTransactionId parent_sub_xid,
							   bool commit)
{
	RowMaskStateCache.for_each([&](RowMaskWriteStateMapEntry* entry) {
		if (entry->writeStateStack == nullptr)
		{
			return;
		}

		if (entry->dropped)
		{
			if (entry->dropSubXid == current_sub_xid)
			{
				if (commit) {
					entry->dropSubXid = parent_sub_xid;
				} else {
					entry->dropped = false;
				}
			}
			return;
		}

		RowMaskSubXidWriteFrame* stack_head = entry->writeStateStack;
		if (stack_head->subXid == current_sub_xid)
		{
			if (commit)
			{
				RowMaskFlushPendingWriteState(stack_head->rowMaskWriteStateEntryList);
			}
			entry->writeStateStack = stack_head->next;
		}
	});
}


/*
 * RowMaskMarkRelfilenodeDropped records that the given relation has been
 * dropped in the given subtransaction.
 */
export void
RowMaskMarkRelfilenodeDropped(Oid relfilelocator, SubTransactionId current_sub_xid)
{
	RowMaskWriteStateMapEntry* entry = RowMaskStateCache.find(&relfilelocator);
	if (entry == nullptr || entry->dropped)
	{
		return;
	}

	entry->dropped = true;
	entry->dropSubXid = current_sub_xid;
}


/*
 * RowMaskNonTransactionDrop removes the entry for a relation dropped
 * outside of subtransaction lifecycle (TRUNCATE).
 */
export void
RowMaskNonTransactionDrop(Oid relfilelocator)
{
	RowMaskStateCache.remove(&relfilelocator);
}


/*
 * RowMaskPendingWritesInUpperTransactions returns true when an ancestor
 * subtransaction still has unflushed row-mask entries for the relation.
 */
export bool
RowMaskPendingWritesInUpperTransactions(Oid relfilelocator, SubTransactionId current_sub_xid)
{
	RowMaskWriteStateMapEntry* entry = RowMaskStateCache.find(&relfilelocator);
	if (entry == nullptr || entry->writeStateStack == nullptr)
	{
		return false;
	}

	for (auto* stack_entry : IntrusiveList<RowMaskSubXidWriteFrame>(entry->writeStateStack))
	{
		if (stack_entry->subXid != current_sub_xid &&
			!stack_entry->rowMaskWriteStateEntryList.empty())
		{
			return true;
		}
	}

	return false;
}


/*
 * GetRowMaskWriteStateContextForDebug exposes the internal context to the
 * columnar_debug SRF for memory-usage reporting.
 */
export MemoryContext
GetRowMaskWriteStateContextForDebug()
{
	return RowMaskStateCache.context();
}


/*
 * UpdateRowMask flips the deletion bit for the given row number. It lives
 * here (relocated from storage_metadata:core, DOC0322 §7.1) because it
 * drives the row-mask write-state cache via RowMaskFindWriteState /
 * RowMaskInitWriteState, which are local to this partition. The heap-side
 * lookup helpers (BioseqColumnRowMask{,Index}RelationId, FlushRowMaskCache) come
 * from :core; the Anum_bioseq_row_mask_* attribute numbers come from `config`.
 */
export bool
UpdateRowMask(RelFileLocator relfilelocator, uint64_t storageId,
			  Snapshot snapshot, uint64_t rowNumber)
{
	bytea *rowMask = NULL;

	RowMaskWriteStateEntry *rowMaskEntry =
		RowMaskFindWriteState(relfilelocator.relNumber, GetCurrentSubTransactionId(), rowNumber);

	if (rowMaskEntry == NULL)
	{
		HeapTuple rowMaskHeapTuple;
		ScanKeyData scanKey[3];

		Oid bioseqColumnRowMaskOid = BioseqColumnRowMaskRelationId();
		Relation bioseqColumnRowMask = table_open(bioseqColumnRowMaskOid, AccessShareLock);
		TupleDesc tupleDescriptor = RelationGetDescr(bioseqColumnRowMask);

		Relation index = index_open(BioseqColumnRowMaskIndexRelationId(), AccessShareLock);

		ScanKeyInit(&scanKey[0], Anum_bioseq_row_mask_storage_id,
					BTEqualStrategyNumber, F_INT8EQ, UInt64GetDatum(storageId));
		ScanKeyInit(&scanKey[1], Anum_bioseq_row_mask_start_row_number,
					BTLessEqualStrategyNumber, F_INT8LE, UInt64GetDatum(rowNumber));
		ScanKeyInit(&scanKey[2], Anum_bioseq_row_mask_end_row_number,
					BTGreaterEqualStrategyNumber, F_INT8GE, UInt64GetDatum(rowNumber));

		SysScanDesc scanDescriptor = systable_beginscan_ordered(bioseqColumnRowMask,
																index,
																NULL, 3, scanKey);

		rowMaskHeapTuple = systable_getnext_ordered(scanDescriptor, BackwardScanDirection);

		if (HeapTupleIsValid(rowMaskHeapTuple))
		{
			bool isnull;

			Datum mask =
					fastgetattr(rowMaskHeapTuple, Anum_bioseq_row_mask_mask,
								tupleDescriptor, &isnull);

			rowMaskEntry = RowMaskInitWriteState(relfilelocator.relNumber,
												 storageId,
												 GetCurrentSubTransactionId(),
												 DatumGetByteaP(mask));
			/*
			 * Populate row mask cache with values from heap table
			 */
			rowMaskEntry->id = DatumGetUInt64(
					fastgetattr(rowMaskHeapTuple, Anum_bioseq_row_mask_id,
								tupleDescriptor, &isnull));

			rowMaskEntry->storageId = DatumGetUInt64(
					fastgetattr(rowMaskHeapTuple, Anum_bioseq_row_mask_storage_id,
								tupleDescriptor, &isnull));

			rowMaskEntry->stripeId = DatumGetUInt64(
					fastgetattr(rowMaskHeapTuple, Anum_bioseq_row_mask_stripe_id,
								tupleDescriptor, &isnull));

			rowMaskEntry->chunkId = DatumGetInt32(
					fastgetattr(rowMaskHeapTuple, Anum_bioseq_row_mask_chunk_id,
								tupleDescriptor, &isnull));

			rowMaskEntry->startRowNumber = DatumGetInt64(
					fastgetattr(rowMaskHeapTuple, Anum_bioseq_row_mask_start_row_number,
								tupleDescriptor, &isnull));

			rowMaskEntry->deletedRows = DatumGetInt32(
					fastgetattr(rowMaskHeapTuple, Anum_bioseq_row_mask_deleted_rows,
								tupleDescriptor, &isnull));

			rowMaskEntry->endRowNumber = DatumGetInt64(
					fastgetattr(rowMaskHeapTuple, Anum_bioseq_row_mask_end_row_number,
								tupleDescriptor, &isnull));

			rowMask = rowMaskEntry->mask;
		}
		else
		{
			/*
			* If the heap tuple is invalid, that likely means that we have
			* encountered a speculative insert.
			*/
			systable_endscan_ordered(scanDescriptor);
			index_close(index, AccessShareLock);
			table_close(bioseqColumnRowMask, AccessShareLock);
			return false;
		}

		systable_endscan_ordered(scanDescriptor);
		index_close(index, AccessShareLock);
		table_close(bioseqColumnRowMask, AccessShareLock);
	}
	else
	{
		rowMask = rowMaskEntry->mask;
	}

	int16_t rowByteMask = rowNumber - rowMaskEntry->startRowNumber;

	/*
	 * IF we have been blocked by advisory lock for storage, maybe row
	 * was delete by some other transaction.
	 */
	if (VARDATA(rowMask)[rowByteMask / 8] & (1 << (rowByteMask % 8)))
		return false;

	VARDATA(rowMask)[rowByteMask / 8] |= 1 << (rowByteMask % 8);

	rowMaskEntry->deletedRows++;

	CommandCounterIncrement();

	return true;
}

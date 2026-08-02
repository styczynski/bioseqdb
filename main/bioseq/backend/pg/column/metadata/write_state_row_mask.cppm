/*-------------------------------------------------------------------------
 *
 * write_state_row_mask.cppm
 *
 * Transaction-scoped management of row-mask catalog records used to
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

#include <new>

export module bioseq.storage.metadata:write_state_row_mask;

import :core;            // FlushRowMaskCache, UpdateChunkGroupDeletedRows, BioseqColumnRowMask*RelationId
import bioseq.common.mlogging;
import bioseq.pg_utils;
import bioseq.pg_support;
import bioseq.storage.metadata.sql_row_codecs;

using row_mask_codec =
	bioseq::storage::metadata::sql_row_codecs::row_mask_codec;
export using row_mask_row = row_mask_codec::row_type;

/*
 * Per-subtransaction node. row_mask_rows accumulates the pending row-mask
 * deltas; last_row_mask is a one-slot cache
 * so repeated lookups for the same row number hit the fast path.
 */
struct RowMaskSubXidWriteFrame
{
	SubTransactionId subXid;
	PList<row_mask_row> row_mask_rows;
	row_mask_row* last_row_mask;
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
 * InitRowMaskEntry allocates a fresh catalog row, copies the
 * caller-supplied mask (so we own the storage) and zero-initialises the
 * remaining fields. The caller fills the row range, stripe and chunk
 * identifiers, and deleted-row count.
 */
static row_mask_row*
InitRowMaskEntry(uint64_t storage_id, bytea* mask)
{
	const Size mask_size = VARSIZE(mask);
	bytea* owned_mask = reinterpret_cast<bytea*>(palloc(mask_size));
	memcpy(owned_mask, mask, mask_size);
	void* storage = palloc(sizeof(row_mask_row));
	row_mask_row* row_mask = new (storage) row_mask_row{
		.storage_id = static_cast<std::int64_t>(storage_id),
		.mask = owned_mask,
	};

	return row_mask;
}

/*
 * RowMaskFlushPendingWriteState iterates a list of accumulated entries and
 * flushes each one into bioseq.row_mask. Also updates chunk-group deleted
 * counters so query planning stays in sync.
 */
static void
RowMaskFlushPendingWriteState(const PList<row_mask_row>& row_mask_write_state_list)
{
	for (auto* row_mask_cache : row_mask_write_state_list)
	{
		FlushRowMaskCache(row_mask_cache);
		UpdateChunkGroupDeletedRows(
			static_cast<std::uint64_t>(row_mask_cache->storage_id),
			static_cast<std::uint64_t>(row_mask_cache->stripe_id),
			static_cast<std::uint32_t>(row_mask_cache->chunk_id),
			row_mask_cache->deleted_rows);
		Assert(row_mask_cache->mask.has_value());
		pfree(*row_mask_cache->mask);
	}
}


/*
 * RowMaskInitWriteState locates (or creates) the stack entry for the given
 * relation/subtransaction and appends a new row-mask catalog record built
 * from the supplied mask. Returns the entry; caller fills remaining fields
 * (chunk/stripe identifiers, row range) before the entry is flushed.
 */
export row_mask_row*
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
		stack_entry->last_row_mask = nullptr;
		hash_entry->writeStateStack = stack_entry;
	}

	row_mask_row* row_mask_write_state = nullptr;
	{
		bioseq::pg_support::MemoryContextScope context_guard(
			RowMaskStateCache.context());
		row_mask_write_state = InitRowMaskEntry(storage_id, row_mask);
		stack_entry->row_mask_rows.append(row_mask_write_state);
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
			RowMaskFlushPendingWriteState(stack_entry->row_mask_rows);
			stack_entry->last_row_mask = nullptr;
			stack_entry->row_mask_rows.clear();
		}
	}
}


/*
 * RowMaskFindWriteState returns the accumulated row-mask entry that
 * contains the given row id, scanning the per-subtransaction list and
 * consulting last_row_mask first as a fast path.
 */
export row_mask_row*
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
		if (stack_frame->last_row_mask != nullptr &&
			stack_frame->last_row_mask->start_row_number
				<= static_cast<std::int64_t>(row_id) &&
			stack_frame->last_row_mask->end_row_number
				>= static_cast<std::int64_t>(row_id))
		{
			return stack_frame->last_row_mask;
		}

		for (auto* row_mask : stack_frame->row_mask_rows)
		{
			if (row_mask->start_row_number
					<= static_cast<std::int64_t>(row_id)
				&& row_mask->end_row_number
					>= static_cast<std::int64_t>(row_id))
			{
				stack_frame->last_row_mask = row_mask;
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
	RowMaskStateCache.for_each(
		[current_sub_xid, parent_sub_xid, commit](
				RowMaskWriteStateMapEntry* entry) {
		/* The drop marker outlives the final row-mask write frame.  Process it
		 * first so an ERROR during DROP followed by transaction abort restores
		 * the relation's backend-local visibility state. */
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

		if (entry->writeStateStack == nullptr)
		{
			return;
		}

		RowMaskSubXidWriteFrame* stack_head = entry->writeStateStack;
		if (stack_head->subXid == current_sub_xid)
		{
			if (commit)
			{
				RowMaskFlushPendingWriteState(stack_head->row_mask_rows);
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
			!stack_entry->row_mask_rows.empty())
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
 * UpdateRowMask records a row deletion in the transaction-scoped cache,
 * loading the persisted mask when necessary.
 */
export bool
UpdateRowMask(RelFileLocator relfilelocator, uint64_t storageId,
			  Snapshot snapshot, uint64_t rowNumber)
{
	bytea *rowMask = NULL;

	row_mask_row *rowMaskEntry =
		RowMaskFindWriteState(relfilelocator.relNumber, GetCurrentSubTransactionId(), rowNumber);

	if (rowMaskEntry == NULL)
	{
		HeapTuple rowMaskHeapTuple;
		ScanKeyData scanKey[3];

		Oid bioseqColumnRowMaskOid = BioseqColumnRowMaskRelationId();
		Relation bioseqColumnRowMask = table_open(bioseqColumnRowMaskOid, AccessShareLock);
		TupleDesc tupleDescriptor = RelationGetDescr(bioseqColumnRowMask);

		Relation index = index_open(BioseqColumnRowMaskIndexRelationId(), AccessShareLock);

		scanKey[0] =
			row_mask_codec::scan_key<&row_mask_row::storage_id>(
				static_cast<std::int64_t>(storageId));
		scanKey[1] =
			row_mask_codec::scan_key<
				&row_mask_row::start_row_number,
				bioseq::pg_utils::scan_comparison::less_equal>(
					static_cast<std::int64_t>(rowNumber));
		scanKey[2] =
			row_mask_codec::scan_key<
				&row_mask_row::end_row_number,
				bioseq::pg_utils::scan_comparison::greater_equal>(
					static_cast<std::int64_t>(rowNumber));

		SysScanDesc scanDescriptor = systable_beginscan_ordered(bioseqColumnRowMask,
																index,
																NULL, 3, scanKey);

		rowMaskHeapTuple = systable_getnext_ordered(scanDescriptor, BackwardScanDirection);

		if (HeapTupleIsValid(rowMaskHeapTuple))
		{
			const row_mask_row stored_row = row_mask_codec::deform(
				rowMaskHeapTuple,
				tupleDescriptor,
				CurrentMemoryContext);
			if (!stored_row.mask.has_value()
				|| *stored_row.mask == nullptr)
			{
				bioseq_log<ERR, PG>("row mask catalog entry has a NULL mask");
			}

			rowMaskEntry = RowMaskInitWriteState(relfilelocator.relNumber,
												 storageId,
												 GetCurrentSubTransactionId(),
												 *stored_row.mask);
			pfree(*stored_row.mask);
			/*
			 * Populate row mask cache with values from heap table
			 */
			rowMaskEntry->id = stored_row.id;
			rowMaskEntry->storage_id = stored_row.storage_id;
			rowMaskEntry->stripe_id = stored_row.stripe_id;
			rowMaskEntry->chunk_id = stored_row.chunk_id;
			rowMaskEntry->start_row_number =
				stored_row.start_row_number;
			rowMaskEntry->deleted_rows = stored_row.deleted_rows;
			rowMaskEntry->end_row_number = stored_row.end_row_number;

			rowMask = *rowMaskEntry->mask;
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
		Assert(rowMaskEntry->mask.has_value());
		rowMask = *rowMaskEntry->mask;
	}

	int16_t rowByteMask = static_cast<int16_t>(
		rowNumber
		- static_cast<std::uint64_t>(
			rowMaskEntry->start_row_number));

	/*
	 * IF we have been blocked by advisory lock for storage, maybe row
	 * was delete by some other transaction.
	 */
	if (VARDATA(rowMask)[rowByteMask / 8] & (1 << (rowByteMask % 8)))
		return false;

	VARDATA(rowMask)[rowByteMask / 8] |= 1 << (rowByteMask % 8);

	rowMaskEntry->deleted_rows++;

	CommandCounterIncrement();

	return true;
}

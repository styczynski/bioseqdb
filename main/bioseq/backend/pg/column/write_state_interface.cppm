/*-------------------------------------------------------------------------
 *
 * write_state_interface.cppm
 *
 * Top-level dispatcher that forwards transaction-lifecycle events
 * (flush/discard on subtransaction commit or abort, relfilenode drop,
 * pending-write checks) to both the columnar write-state module and the
 * row-mask write-state module. Exists so higher layers only have to call
 * one function per event.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

export module storage_metadata:write_state_interface;

import :write_state;            // BioseqColumnPopWriteStateForAllRels, FlushWriteStateForRelfilenode, ...
import :write_state_row_mask;   // RowMaskPopWriteStateForAllRels, RowMaskMarkRelfilenodeDropped, ...
import pg_utils;

/*
 * FlushWriteStateWithNewSnapshot flushes pending writes for the given
 * relfilenode in the current subtransaction. If the caller supplied an
 * MVCC snapshot, it is replaced with a copy whose curcid equals the
 * current command id so the just-flushed rows are visible to subsequent
 * reads. Uses output parameters rather than a return struct because the
 * function is declared with a matching C prototype in columnar.h that
 * external callers depend on.
 */
export void
FlushWriteStateWithNewSnapshot(Oid relfilenode,
							   Snapshot* snapshot,
							   bool* snapshot_registered_by_us)
{
	FlushWriteStateForRelfilenode(relfilenode, GetCurrentSubTransactionId());

	if (*snapshot == InvalidSnapshot || !IsMVCCSnapshot(*snapshot))
	{
		return;
	}

	/*
	 * Ensure the flushed writes are visible to us by bumping the active
	 * snapshot's curcid. We push a copy only so UpdateActiveSnapshotCommandId
	 * is legal; we pop it immediately because BioseqColumnReadState holds the
	 * reference and RegisterSnapshot keeps it alive.
	 */
	PushCopiedSnapshot(*snapshot);

	UpdateActiveSnapshotCommandId();
	Snapshot refreshed_snapshot = GetActiveSnapshot();
	RegisterSnapshot(refreshed_snapshot);

	PopActiveSnapshot();

	*snapshot = refreshed_snapshot;
	*snapshot_registered_by_us = true;
}


/*
 * FlushWriteStateForAllRels is the commit-side lifecycle hook. It flushes
 * pending writes for every relation that the current subtransaction touched
 * (both column writes and row-mask writes).
 */
export void
FlushWriteStateForAllRels(SubTransactionId current_sub_xid, SubTransactionId parent_sub_xid)
{
	BioseqColumnPopWriteStateForAllRels(current_sub_xid, parent_sub_xid, true);
	RowMaskPopWriteStateForAllRels(current_sub_xid, parent_sub_xid, true);
}


/*
 * DiscardWriteStateForAllRels is the abort-side lifecycle hook. It drops
 * pending writes for every relation that the current subtransaction touched.
 */
export void
DiscardWriteStateForAllRels(SubTransactionId current_sub_xid, SubTransactionId parent_sub_xid)
{
	BioseqColumnPopWriteStateForAllRels(current_sub_xid, parent_sub_xid, false);
	RowMaskPopWriteStateForAllRels(current_sub_xid, parent_sub_xid, false);
}


/*
 * MarkRelfilenodeDropped is called when the given relfilenode has been
 * dropped in the current subtransaction. The write-state modules record the
 * drop so later lifecycle hooks skip the relation.
 */
export void
MarkRelfilenodeDropped(Oid relfilenode, SubTransactionId current_sub_xid)
{
	BioseqColumnMarkRelfilenodeDroppedBioseqColumn(relfilenode, current_sub_xid);
	RowMaskMarkRelfilenodeDropped(relfilenode, current_sub_xid);
}


/*
 * NonTransactionDropWriteState handles the TRUNCATE-style drop path where
 * the relfilenode disappears without going through subtransaction commit.
 */
export void
NonTransactionDropWriteState(Oid relfilenode)
{
	BioseqColumnNonTransactionDropWriteState(relfilenode);
	RowMaskNonTransactionDrop(relfilenode);
}

/*
 * PendingWritesInUpperTransactions returns true if a higher-level
 * subtransaction still has pending writes for the given relation. Used to
 * detect situations where we'd be bypassing uncommitted writes on read.
 */
export bool
PendingWritesInUpperTransactions(Oid relfilenode, SubTransactionId current_sub_xid)
{
	return (BioseqColumnPendingWritesInUpperTransactions(relfilenode, current_sub_xid) ||
			RowMaskPendingWritesInUpperTransactions(relfilenode, current_sub_xid));
}

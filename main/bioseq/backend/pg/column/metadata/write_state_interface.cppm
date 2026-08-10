/*-------------------------------------------------------------------------
 *
 * write_state_interface.cppm
 *
 * Top-level dispatcher that forwards transaction-lifecycle events
 * (flush/discard on subtransaction commit or abort, relation-file drop,
 * pending-write checks) to both the columnar write-state module and the
 * row-mask write-state module. Exists so higher layers only have to call
 * one function per event.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <bioseq/pg_compat.h>

export module bioseq.postgres.storage.metadata:write_state_interface;

import :write_state;            // pop_all_column_write_states, flush_relation_write_state, ...
import :write_state_row_mask;   // pop_all_row_mask_write_states, mark_row_mask_relation_dropped, ...
import bioseq.postgres.support;

/** @brief Flushes relation writes and refreshes an owned MVCC snapshot.
 *
 * If the caller supplied an MVCC snapshot, replaces it with a registered copy
 * whose curcid includes the just-flushed rows.
 */
export void
flush_write_state_with_fresh_snapshot(Oid relation_file_number,
							   Snapshot* snapshot,
							   bool* snapshot_registered_by_us)
{
	flush_relation_write_state(relation_file_number, GetCurrentSubTransactionId());

	if (*snapshot == InvalidSnapshot || !IsMVCCSnapshot(*snapshot))
	{
		return;
	}

	/*
	 * Ensure the flushed writes are visible to us by bumping the active
	 * snapshot's curcid. We push a copy only so UpdateActiveSnapshotCommandId
	 * is legal; we pop it immediately because column_read_state holds the
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


/** @brief Commits pending column and row-mask writes for a subtransaction.
 */
export void
flush_all_relation_write_states(SubTransactionId current_sub_xid, SubTransactionId parent_sub_xid)
{
	pop_all_column_write_states(current_sub_xid, parent_sub_xid, true);
	pop_all_row_mask_write_states(current_sub_xid, parent_sub_xid, true);
}


/** @brief Discards pending column and row-mask writes for a subtransaction.
 */
export void
discard_all_relation_write_states(SubTransactionId current_sub_xid, SubTransactionId parent_sub_xid)
{
	pop_all_column_write_states(current_sub_xid, parent_sub_xid, false);
	pop_all_row_mask_write_states(current_sub_xid, parent_sub_xid, false);
}


/** @brief Marks both write-state stores as dropped for a relation.
 */
export void
mark_write_state_relation_dropped(Oid relation_file_number, SubTransactionId current_sub_xid)
{
	mark_column_write_state_relation_dropped(relation_file_number, current_sub_xid);
	mark_row_mask_relation_dropped(relation_file_number, current_sub_xid);
}


/** @brief Removes relation state outside subtransaction lifecycle callbacks.
 */
export void
drop_write_state_outside_transaction(Oid relation_file_number)
{
	drop_column_write_state_outside_transaction(relation_file_number);
	drop_row_mask_state_outside_transaction(relation_file_number);
}

/** @brief Tests whether an ancestor subtransaction owns pending writes.
 */
export bool
writes_pending_in_parent_transactions(
	Oid relation_file_number,
	SubTransactionId current_sub_xid)
{
	return column_writes_pending_in_parent_transactions(
			relation_file_number, current_sub_xid)
		|| row_mask_writes_pending_in_parent_transactions(
			relation_file_number, current_sub_xid);
}

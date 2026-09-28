/**
 * @file write_state_interface.cppm
 * @brief Handle column and deletion-mask state at transaction boundaries.
 *
 * Callers use one function per commit, abort or relation drop. These helpers
 * pass the event to both stores and check both for pending parent writes.
 */
module;

#include <bioseq/pg_compat.h>

export module bioseq.postgres.storage.metadata:write_state_interface;

import :write_state;            // pop_all_column_write_states, flush_relation_write_state, ...
import :write_state_row_mask;   // pop_all_row_mask_write_states, mark_row_mask_relation_dropped, ...
import bioseq.postgres.support;

/** @brief Flush  a relation's column writes and refresh its reader snapshot.
 *
 * For an MVCC snapshot, register a copy with the vurrent command id so the
 * reader can see rows just flushed by this transaction.
 *
 * @param[in] relation_file_number Relation file number with pending writes.
 * @param[in,out] snapshot Replaced only when it is an MVCC snapshot.
 * @param[in,out] snapshot_registered_by_us Set to true when a copy is registered.
 * The caller must unregister that copy
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

	/* Update the command id on a temporary active copy. Registration keeps
	 * that copy alive after it is removed from the active-snapshot stack. */
	PushCopiedSnapshot(*snapshot);

	UpdateActiveSnapshotCommandId();
	Snapshot refreshed_snapshot = GetActiveSnapshot();
	RegisterSnapshot(refreshed_snapshot);

	PopActiveSnapshot();

	*snapshot = refreshed_snapshot;
	*snapshot_registered_by_us = true;
}


/**
 * @brief Flush  a subtransaction's column and deletion-mask writes.
 * @param[in] current_sub_xid Subtransaction being completed.
 * @param[in] parent_sub_xid Parent that inherits committed DROP markers.
 */
export void
flush_all_relation_write_states(SubTransactionId current_sub_xid, SubTransactionId parent_sub_xid)
{
	pop_all_column_write_states(current_sub_xid, parent_sub_xid, true);
	pop_all_row_mask_write_states(current_sub_xid, parent_sub_xid, true);
}


/**
 * @brief Discard an aborted subtransaction's column and deletion-mask writes.
 * @param[in] current_sub_xid Aborted subtransaction.
 * @param[in] parent_sub_xid Parent id passed to both cleanup functions.
 */
export void
discard_all_relation_write_states(SubTransactionId current_sub_xid, SubTransactionId parent_sub_xid)
{
	pop_all_column_write_states(current_sub_xid, parent_sub_xid, false);
	pop_all_row_mask_write_states(current_sub_xid, parent_sub_xid, false);
}


/**
 * @brief Mark a relation as dropped in both write-state caches.
 * @param[in] relation_file_number Relation file number.
 * @param[in] current_sub_xid Subtransaction that owns the DROP markers.
 */
export void
mark_write_state_relation_dropped(Oid relation_file_number, SubTransactionId current_sub_xid)
{
	mark_column_write_state_relation_dropped(relation_file_number, current_sub_xid);
	mark_row_mask_relation_dropped(relation_file_number, current_sub_xid);
}


/**
 * @brief Remove a relation from both caches outside transaction callbacks.
 * @param[in] relation_file_number Relation file number to remove.
 */
export void
drop_write_state_outside_transaction(Oid relation_file_number)
{
	drop_column_write_state_outside_transaction(relation_file_number);
	drop_row_mask_state_outside_transaction(relation_file_number);
}

/**
 * @brief Check for pending column or deletion-mask writes in parent subtransactions.
 * @param[in] relation_file_number Relation file number.
 * @param[in] current_sub_xid Current subtransaction to exclude.
 * @return true if  either, cache contains pending parent writes.
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

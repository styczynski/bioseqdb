/**
 * @file write_state_row_mask.cppm
 * @brief Transaction-scoped write state for row-mask catalog records.
 *
 * Manages row-mask catalog records used to
 * aggregate row-mask updates (DELETEs against columnar stripes). Commits
 * flush the pending mask deltas into bioseq.row_mask via flush_row_mask_cache.
 *
 * The transaction_scoped_hash_table owns one linked stack of subtransaction
 * frames for each relation file number.
 */
module;

#include <bioseq/pg_compat.h>

#include <cstdint>
#include <cstring>
#include <new>

export module bioseq.postgres.storage.metadata:write_state_row_mask;

import :core;
import :row_mask;
import :stripe;
import bioseq.common.logging;
import bioseq.postgres.support;
import bioseq.postgres.storage.metadata.sql_row_codecs;

using row_mask_codec =
	bioseq::postgres::storage::metadata::sql_row_codecs::row_mask_codec;
export using row_mask_row = row_mask_codec::row_type;

/*
 * Per-subtransaction node. row_mask_rows accumulates the pending row-mask
 * deltas; last_row_mask is a one-slot cache
 * so repeated lookups for the same row number hit the fast path.
 */
struct row_mask_subtransaction_write_frame
{
	SubTransactionId subtransaction_id;
	List* row_mask_rows;
	row_mask_row* last_row_mask;
	row_mask_subtransaction_write_frame* next;
};

/*
 * Per-relation-file entry. A transactional drop marker propagates on commit
 * and is reverted on abort.
 */
struct row_mask_write_state_map_entry
{
	Oid relation_file_number;

	bool dropped;
	SubTransactionId drop_subtransaction_id;

	row_mask_subtransaction_write_frame* write_state_stack;
};

static bioseq::postgres::support::transaction_scoped_hash_table<
	row_mask_write_state_map_entry> row_mask_state_cache{"row mask state map"};


/** @brief Allocates a pending row-mask catalog entry and owns a mask copy.
 *
 * Copies the supplied mask into the row-mask cache context. The caller fills
 * the row range, stripe and chunk identifiers, and deleted-row count.
 */
static row_mask_row*
allocate_row_mask_entry(uint64_t storage_id, bytea* mask)
{
	const Size mask_size = bioseq::postgres::support::varlena_4byte_size(mask);
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
 * flush_pending_row_mask_writes iterates a list of accumulated entries and
 * flushes each one into bioseq.row_mask. Also updates chunk-group deleted
 * counters so query planning stays in sync.
 */
static void
flush_pending_row_mask_writes(List* row_mask_write_state_list)
{
	for (row_mask_row* row_mask_cache :
		 bioseq::postgres::support::postgres_list<row_mask_row>{
			 row_mask_write_state_list})
	{
		flush_row_mask_cache(row_mask_cache);
		update_chunk_group_deleted_rows(
			static_cast<std::uint64_t>(row_mask_cache->storage_id),
			static_cast<std::uint64_t>(row_mask_cache->stripe_id),
			static_cast<std::uint32_t>(row_mask_cache->chunk_id),
			row_mask_cache->deleted_rows);
		Assert(row_mask_cache->mask.has_value());
		pfree(*row_mask_cache->mask);
	}
}


/** @brief Appends a pending row-mask write to a subtransaction frame.
 *
 * Returns the new entry so the caller can fill its identifiers and row range
 * before the frame is flushed.
 */
export row_mask_row*
append_row_mask_write(Oid relation_file_number,
					  uint64 storage_id,
					  SubTransactionId current_sub_xid,
					  bytea* row_mask)
{
	bioseq_log<logging_level::debug>("[bioseq] pg: append_row_mask_write() relation_file_number={} storage_id={} sub_xid={}", relation_file_number, storage_id, current_sub_xid);

	const bioseq::postgres::support::transaction_hash_insert_result<
		row_mask_write_state_map_entry> insertion =
			row_mask_state_cache.find_or_insert(&relation_file_number);
	row_mask_write_state_map_entry* hash_entry = insertion.entry;

	if (!insertion.was_present)
	{
		hash_entry->write_state_stack = nullptr;
		hash_entry->dropped = false;
	}

	Assert(!hash_entry->dropped);

	row_mask_subtransaction_write_frame* stack_entry = nullptr;
	if (hash_entry->write_state_stack != nullptr &&
		hash_entry->write_state_stack->subtransaction_id == current_sub_xid)
	{
		stack_entry = hash_entry->write_state_stack;
	}

	if (stack_entry == nullptr)
	{
		stack_entry = row_mask_state_cache
			.allocate_in_context<row_mask_subtransaction_write_frame>();
		stack_entry->subtransaction_id = current_sub_xid;
		stack_entry->next = hash_entry->write_state_stack;
		stack_entry->last_row_mask = nullptr;
		hash_entry->write_state_stack = stack_entry;
	}

	row_mask_row* row_mask_write_state = nullptr;
	{
		bioseq::postgres::support::memory_context_scope context_guard(
			row_mask_state_cache.get_memory_context());
		row_mask_write_state = allocate_row_mask_entry(storage_id, row_mask);
		bioseq::postgres::support::postgres_list<row_mask_row> pending_rows{
			stack_entry->row_mask_rows};
		pending_rows.append(row_mask_write_state);
		stack_entry->row_mask_rows = pending_rows.release();
	}

	return row_mask_write_state;
}

/** @brief Flushes pending row masks for one relation and subtransaction.
 *
 * Leaves the empty stack frame in place so subsequent operations in the same
 * subtransaction can continue accumulating.
 */
export void flush_row_mask_write_state_for_relation_file(Oid relation_file_number,
										  SubTransactionId current_sub_xid) {
	row_mask_write_state_map_entry* entry = row_mask_state_cache.find(&relation_file_number);
	Assert(!entry || !entry->dropped);

	if (entry != nullptr && entry->write_state_stack != nullptr)
	{
		row_mask_subtransaction_write_frame* stack_entry = entry->write_state_stack;
		if (stack_entry->subtransaction_id == current_sub_xid)
		{
			flush_pending_row_mask_writes(stack_entry->row_mask_rows);
			stack_entry->last_row_mask = nullptr;
			list_free(stack_entry->row_mask_rows);
			stack_entry->row_mask_rows = nullptr;
		}
	}
}


/** @brief Finds the pending mask containing a row identifier.
 *
 * Consults last_row_mask before scanning the current subtransaction's list.
 */
export row_mask_row*
find_pending_row_mask(Oid relation_file_number, SubTransactionId current_sub_xid,
					  uint64 row_id)
{
	row_mask_write_state_map_entry* hash_entry = row_mask_state_cache.find(&relation_file_number);
	if (hash_entry == nullptr)
	{
		return nullptr;
	}

	for (row_mask_subtransaction_write_frame* stack_frame :
		 bioseq::postgres::support::intrusive_list<
			 row_mask_subtransaction_write_frame>{hash_entry->write_state_stack})
	{
		if (stack_frame->subtransaction_id != current_sub_xid)
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

		for (row_mask_row* row_mask :
			 bioseq::postgres::support::postgres_list<row_mask_row>{
				 stack_frame->row_mask_rows})
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


/** @brief Finalizes pending row-mask writes for one subtransaction.
 *
 * Flushes matching frames on commit and discards them on abort. Drop markers
 * propagate to the parent on commit and are reverted on abort.
 */
export void
pop_all_row_mask_write_states(SubTransactionId current_sub_xid,
							   SubTransactionId parent_sub_xid,
							   bool commit)
{
	row_mask_state_cache.for_each(
		[current_sub_xid, parent_sub_xid, commit](
				row_mask_write_state_map_entry* entry) {
		/* The drop marker outlives the final row-mask write frame.  Process it
		 * first so an ERROR during DROP followed by transaction abort restores
		 * the relation's backend-local visibility state. */
		if (entry->dropped)
		{
			if (entry->drop_subtransaction_id == current_sub_xid)
			{
				if (commit) {
					entry->drop_subtransaction_id = parent_sub_xid;
				} else {
					entry->dropped = false;
				}
			}
			return;
		}

		if (entry->write_state_stack == nullptr)
		{
			return;
		}

		row_mask_subtransaction_write_frame* stack_head = entry->write_state_stack;
		if (stack_head->subtransaction_id == current_sub_xid)
		{
			if (commit)
			{
				flush_pending_row_mask_writes(stack_head->row_mask_rows);
			}
			entry->write_state_stack = stack_head->next;
		}
	});
}


/** @brief Marks a row-mask state as dropped in a subtransaction.
 */
export void
mark_row_mask_relation_dropped(Oid relation_file_number, SubTransactionId current_sub_xid)
{
	row_mask_write_state_map_entry* entry = row_mask_state_cache.find(&relation_file_number);
	if (entry == nullptr || entry->dropped)
	{
		return;
	}

	entry->dropped = true;
	entry->drop_subtransaction_id = current_sub_xid;
}


/** @brief Removes row-mask state for a relation dropped outside transaction cleanup.
 */
export void
drop_row_mask_state_outside_transaction(Oid relation_file_number)
{
	row_mask_state_cache.remove(&relation_file_number);
}


/** @brief Tests whether an ancestor owns pending row-mask writes.
 */
export bool
row_mask_writes_pending_in_parent_transactions(
	Oid relation_file_number,
	SubTransactionId current_sub_xid)
{
	row_mask_write_state_map_entry* entry = row_mask_state_cache.find(&relation_file_number);
	if (entry == nullptr || entry->write_state_stack == nullptr)
	{
		return false;
	}

	for (row_mask_subtransaction_write_frame* stack_entry :
		 bioseq::postgres::support::intrusive_list<
			 row_mask_subtransaction_write_frame>{entry->write_state_stack})
	{
		if (stack_entry->subtransaction_id != current_sub_xid &&
			!bioseq::postgres::support::postgres_list<row_mask_row>{
				 stack_entry->row_mask_rows}.is_empty())
		{
			return true;
		}
	}

	return false;
}


/// @brief Returns the context owning transaction-scoped row-mask writes.
export MemoryContext
row_mask_write_memory_context()
{
	return row_mask_state_cache.get_memory_context();
}


/// @brief Records a row deletion, loading its persisted mask on first access.
export bool
update_row_mask(RelFileLocator relfilelocator, uint64_t storage_id,
			  Snapshot snapshot, uint64_t row_number)
{
	bytea *row_mask = nullptr;

	row_mask_row *row_mask_entry =
		find_pending_row_mask(relfilelocator.relNumber, GetCurrentSubTransactionId(), row_number);

	if (row_mask_entry == nullptr)
	{
		HeapTuple row_mask_tuple;
		ScanKeyData scan_keys[3];

		Oid row_mask_relation_oid = column_row_mask_relation_oid();
		Relation row_mask_relation = table_open(row_mask_relation_oid, AccessShareLock);
		TupleDesc tuple_descriptor = RelationGetDescr(row_mask_relation);

		Relation index = index_open(column_row_mask_index_oid(), AccessShareLock);

		scan_keys[0] =
			row_mask_codec::scan_key<&row_mask_row::storage_id>(
				static_cast<std::int64_t>(storage_id));
		scan_keys[1] =
			row_mask_codec::scan_key<
				&row_mask_row::start_row_number,
				bioseq::postgres::support::scan_comparison::less_equal>(
					static_cast<std::int64_t>(row_number));
		scan_keys[2] =
			row_mask_codec::scan_key<
				&row_mask_row::end_row_number,
				bioseq::postgres::support::scan_comparison::greater_equal>(
					static_cast<std::int64_t>(row_number));

		SysScanDesc scan_descriptor = systable_beginscan_ordered(row_mask_relation,
																index,
														nullptr, 3, scan_keys);

		row_mask_tuple = systable_getnext_ordered(scan_descriptor, BackwardScanDirection);

		if (HeapTupleIsValid(row_mask_tuple))
		{
			const row_mask_row stored_row = row_mask_codec::deform(
				row_mask_tuple,
				tuple_descriptor,
				CurrentMemoryContext);
			if (!stored_row.mask.has_value()
				|| *stored_row.mask == nullptr)
			{
				bioseq_log<logging_level::error, logging_target::postgres>("row mask catalog entry has a NULL mask");
			}

			row_mask_entry = append_row_mask_write(relfilelocator.relNumber,
												 storage_id,
												 GetCurrentSubTransactionId(),
												 *stored_row.mask);
			pfree(*stored_row.mask);
			/*
			 * Populate row mask cache with values from heap table
			 */
			row_mask_entry->id = stored_row.id;
			row_mask_entry->storage_id = stored_row.storage_id;
			row_mask_entry->stripe_id = stored_row.stripe_id;
			row_mask_entry->chunk_id = stored_row.chunk_id;
			row_mask_entry->start_row_number =
				stored_row.start_row_number;
			row_mask_entry->deleted_rows = stored_row.deleted_rows;
			row_mask_entry->end_row_number = stored_row.end_row_number;

			row_mask = *row_mask_entry->mask;
		}
		else
		{
			/*
			* If the heap tuple is invalid, that likely means that we have
			* encountered a speculative insert.
			*/
			systable_endscan_ordered(scan_descriptor);
			index_close(index, AccessShareLock);
			table_close(row_mask_relation, AccessShareLock);
			return false;
		}

		systable_endscan_ordered(scan_descriptor);
		index_close(index, AccessShareLock);
		table_close(row_mask_relation, AccessShareLock);
	}
	else
	{
		Assert(row_mask_entry->mask.has_value());
		row_mask = *row_mask_entry->mask;
	}

	int16_t row_byte_offset = static_cast<int16_t>(
		row_number
		- static_cast<std::uint64_t>(
			row_mask_entry->start_row_number));

	/* Another writer may have deleted the row while this backend waited. */
	if (VARDATA(row_mask)[row_byte_offset / 8] & (1 << (row_byte_offset % 8)))
		return false;

	VARDATA(row_mask)[row_byte_offset / 8] |= 1 << (row_byte_offset % 8);

	row_mask_entry->deleted_rows++;

	CommandCounterIncrement();

	return true;
}

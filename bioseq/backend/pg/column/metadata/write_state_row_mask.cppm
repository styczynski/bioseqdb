/**
 * @file write_state_row_mask.cppm
 * @brief Buffer deletion-mask changes within each subtransaction.
 *
 * Repeated DELETEs updat cached masks. Flushing saves them in
 * bioseq_internal.row_mask and updates chunk-group deletion counts.
 * Each relation file has a stack of subtransaction frames so abort can
 * discard pending changes.
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

/* Pending masks for one subtransaction. Remember the last mask so nearby
 * row deletions can reuse it without searching the list. */
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


/** @brief Copy a mask into a new pending catalog entry.
 * Allocate  in the current context, which the caller sets to the mask cache
 * The caller fills the row range, ids and deleted-row count.
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

/** @brief Save pending masks and their chunk-group deletion counts.
 * Free each copied mask after its catalog update.
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


/** @brief Add a copied mask to the current subtransaction's pending writes.
 * Return its entry so the caller can fill the catalog id, row range and counts.
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

/** @brief Save a relation's pending masks for the current subtransaction.
 * Clear the saved entries and last-mask lookup, keeping the  frame for later writes.
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


/** @brief Find a pending mask covering a row number, or return nullptr.
 * Check the most recently used mask before searching this subtransaction's list.
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


/** @brief Finish a subtransaction's pending deletion-mask writes.
 * Flush on cmmit and discard on abort. Commit passes DROP markers to the
 * parent, while abort clears them.
 */
export void
pop_all_row_mask_write_states(SubTransactionId current_sub_xid,
							   SubTransactionId parent_sub_xid,
							   bool commit)
{
	row_mask_state_cache.for_each(
		[current_sub_xid, parent_sub_xid, commit](
				row_mask_write_state_map_entry* entry) {
		/* Handle DROP even when no write frame remains. Aborting a failed DROP
		 * must restore this backend's relation state. */
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


/** @brief Mark an existing relation entry as dropped by this subtransaction. */
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


/** @brief Remove a relation's mask-cache entry  outside transaction callbacks. */
export void
drop_row_mask_state_outside_transaction(Oid relation_file_number)
{
	row_mask_state_cache.remove(&relation_file_number);
}


/** @brief Check, for pending masks in frames owned by parent subtransactions. */
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


/// @brief Return the memory context that owns pending masks.
/// @return A context reset at transaction end, or nullptr before the first entry
export MemoryContext
row_mask_write_memory_context()
{
	return row_mask_state_cache.get_memory_context();
}


/// @brief Mark a row deleted in its cached mask, loading the mask when needed.
/// Return false if the row has no stored mask or is already marked deleted.
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
			/* Keep the catalog identifiers and counts with the copied mask. */
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
			/* A speculative insert may have no persisted row mask yet. */
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

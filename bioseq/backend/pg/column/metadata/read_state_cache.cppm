/**
 * @file read_state_cache.cppm
 * @brief Reuse column readers within a transaction.
 *
 * Row-version fetches can arrive through separate executor or SPI calls.
 * Cache  their readers by relation file number and subtransaction so repeated
 * fetches can reuse the prepared state. The hash table lives in a child of
 * TopTransactionContext and is cleared when the transaction ends.
 */
module;

#include <bioseq/pg_compat.h>

export module bioseq.postgres.storage.metadata:read_state_cache;

import :core;
import bioseq.common.logging;
import bioseq.postgres.support;

// Let the reader supply its cleanup function without importing its implementation.
using end_read_hook_type = void (*)(column_read_state*);
static end_read_hook_type end_read_hook = nullptr;

/// @brief Set the cleanup function called when a cached reader is released.
/// @param[in] end_read Callback that remains valid for the backend process
export void
register_column_read_state_hooks(end_read_hook_type end_read)
{
	end_read_hook = end_read;
}

/**
 * @brief A subtransaction's cached reader for one relation.
 *
 * The cache owns this frame. The caoler fills read_state through the, returned
 * slot, and the release hook closes that reader at  the subtransaction boundary.
 */
struct subtransaction_read_frame
{
	SubTransactionId subtransaction_id;
	column_read_state* read_state;
	subtransaction_read_frame* next;
};

/** @brief Stores the newest-first subtransaction frames for one physical relation. */
struct read_state_map_entry
{
	Oid relation_file_number;
	subtransaction_read_frame* read_state_stack;
};

static bioseq::postgres::support::transaction_scoped_hash_table<
	read_state_map_entry> read_state_cache{"column read state map"};


/** @brief Get or add the current subtransaction's reader slot.
 *
 * The caller stores a new column_read_state in an empty slot. Repeated calls
 * for the current stack head return the same slot.
 *
 * @param[in] relation Relation whose file number identifies the cache entry.
 * @param[in] current_sub_xid Current subtransaction.
 * @return Slot valid until subtransaction release or transaction reset.
 */
export column_read_state**
acquire_column_read_state_slot(Relation relation, SubTransactionId current_sub_xid)
{
	bioseq_log<logging_level::debug>("[bioseq] pg: acquire_column_read_state_slot() sub_xid={}", current_sub_xid);

	const bioseq::postgres::support::transaction_hash_insert_result<
		read_state_map_entry> insertion =
			read_state_cache.find_or_insert(&relation->rd_locator.relNumber);
	read_state_map_entry* hash_entry = insertion.entry;

	if (!insertion.was_present)
	{
		hash_entry->read_state_stack = nullptr;
	}

	if (hash_entry->read_state_stack != nullptr &&
		hash_entry->read_state_stack->subtransaction_id == current_sub_xid)
	{
		return &hash_entry->read_state_stack->read_state;
	}

	subtransaction_read_frame* stack_entry =
		read_state_cache.allocate_in_context<subtransaction_read_frame>();
	stack_entry->subtransaction_id = current_sub_xid;
	stack_entry->next = hash_entry->read_state_stack;
	hash_entry->read_state_stack = stack_entry;

	return &stack_entry->read_state;
}


/** @brief Find a cached reader slot without creating one.
 * @param[in] relation Relation whose file number identifies the cache entry.
 * @param[in] current_sub_xid Subtransaction to find in the reader stack.
 * @return A replaceable  reader slot, or nullptr if no frame matches.
 */
export column_read_state**
find_column_read_state_slot(Relation relation, SubTransactionId current_sub_xid)
{
	read_state_map_entry* hash_entry =
		read_state_cache.find(&relation->rd_locator.relNumber);
	if (hash_entry == nullptr)
	{
		return nullptr;
	}

	for (subtransaction_read_frame* stack_frame :
		 bioseq::postgres::support::intrusive_list<subtransaction_read_frame>{
			 hash_entry->read_state_stack})
	{
		if (stack_frame->subtransaction_id == current_sub_xid)
		{
			return &stack_frame->read_state;
		}
	}

	return nullptr;
}


/**
 * @brief Close and remove each relation's reader for a completed subtransaction.
 * @param[in] current_sub_xid Subtransaction at the head of the reader stacks.
 *
 * Call this as subtransactions finish, before releasing their parents.
 */
export void
release_subtransaction_read_states(SubTransactionId current_sub_xid)
{
	bioseq_log<logging_level::debug>("[bioseq] pg: release_subtransaction_read_states() sub_xid={}", current_sub_xid);

	read_state_cache.for_each([current_sub_xid](read_state_map_entry* entry) {
		subtransaction_read_frame* stack_head = entry->read_state_stack;
		if (stack_head == nullptr)
		{
			return;
		}
		if (stack_head->subtransaction_id == current_sub_xid)
		{
			if (end_read_hook != nullptr)
			{
				end_read_hook(stack_head->read_state);
			}
			entry->read_state_stack = stack_head->next;
		}
	});
}


/// @brief Return the memory context used by the transaction's cached readers.
/// @return A context reset at transaction end, or nullptr before the first cache entry.
export MemoryContext column_read_state_memory_context()
{
	return read_state_cache.get_memory_context();
}

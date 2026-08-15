/**
 * @file read_state_cache.cppm
 * @brief Transaction-scoped cache for reusable column read states.
 *
 * Caches column_read_state pointers keyed by relation
 * file number. When tuple_fetch_row_version is invoked across separate
 * SPI/executor calls we need to reuse the prior read state instead of
 * building a new one, because read-state setup is not shared between calls.
 *
 * The underlying hash table lives in a TopTransactionContext child;
 * transaction_scoped_hash_table installs the reset callback that clears it at
 * transaction end.
 */
module;

#include <bioseq/pg_compat.h>

export module bioseq.postgres.storage.metadata:read_state_cache;

import :core;
import bioseq.common.logging;
import bioseq.postgres.support;

// The reader registers teardown without introducing a dependency back to its
// implementation module.
using end_read_hook_type = void (*)(column_read_state*);
static end_read_hook_type end_read_hook = nullptr;

/// @brief Registers the callback that releases an owned column read state.
export void
register_column_read_state_hooks(end_read_hook_type end_read)
{
	end_read_hook = end_read;
}

/*
 * Per-subtransaction node in the stack hanging off each map entry. The
 * read_state pointer is owned by the caller via the ** returned from
 * acquire_column_read_state_slot.
 */
struct subtransaction_read_frame
{
	SubTransactionId subtransaction_id;
	column_read_state* read_state;
	subtransaction_read_frame* next;
};

/*
 * Per-relation-file entry. read_state_stack is a singly-linked stack of
 * per-subtransaction frames ordered top-of-stack first.
 */
struct read_state_map_entry
{
	Oid relation_file_number;
	subtransaction_read_frame* read_state_stack;
};

static bioseq::postgres::support::transaction_scoped_hash_table<
	read_state_map_entry> read_state_cache{"column read state map"};


/** @brief Acquires the read-state slot for a relation and subtransaction.
 *
 * The caller writes a newly constructed column_read_state through the returned
 * slot; subsequent lookups retrieve that same owned pointer.
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


/** @brief Finds an existing read-state slot without allocating it.
 *
 * Returns null when the relation/subtransaction has no entry. Returning a
 * pointer to the slot lets callers replace the cached state.
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


/** @brief Releases cached read states created in one subtransaction.
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


/// @brief Returns the context owning transaction-scoped read-state entries.
export MemoryContext column_read_state_memory_context()
{
	return read_state_cache.get_memory_context();
}

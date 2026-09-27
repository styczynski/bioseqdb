/**
 * @file write_state.cppm
 * @brief Keep pending column writes separate for each subtransaction.
 *
 * Each relation file has a stack of writer states. Completing a subtransaction
 * flushes or discards its state, leaving its parent's state available.
 * A transaction-scoped hash table owns the entries and clears them at the
 * end of the top-level transaction.
 */
module;

#include <bioseq/pg_compat.h>

export module bioseq.postgres.storage.metadata:write_state;

import :core;
import :options;
import bioseq.postgres.support;
import bioseq.common.logging;

// Writer and sequence-storage modules supply these callbacks to avoid circular imports.
using begin_write_hook_type =
	column_write_state* (*)(RelFileLocator, column_options, TupleDesc);
using end_write_hook_type = void (*)(column_write_state*);
using flush_writes_hook_type = void (*)(column_write_state*);
using contains_pending_hook_type = bool (*)(column_write_state*);
using close_sequence_caches_hook_type = void (*)(void*, bool);

static begin_write_hook_type begin_write_hook = nullptr;
static end_write_hook_type end_write_hook = nullptr;
static flush_writes_hook_type flush_writes_hook = nullptr;
static contains_pending_hook_type contains_pending_hook = nullptr;
static close_sequence_caches_hook_type close_sequence_caches_hook = nullptr;

/// @brief Set the callbacks used to manage buffered writes.
/// @param[in] begin_write Create a writer in the transaction's memory context.
/// @param[in] end_write Flush and release a completed writer.
/// @param[in] flush_writes Flush pending writes while keeping the writer open.
/// @param[in] contains_pending Check whether a writer has buffered data.
/// @param[in] close_sequence_caches Close sequence caches, saving or discarding pending changes.
export void
register_column_write_state_hooks(begin_write_hook_type begin_write,
						   end_write_hook_type end_write,
						   flush_writes_hook_type flush_writes,
						   contains_pending_hook_type contains_pending,
						   close_sequence_caches_hook_type close_sequence_caches)
{
	begin_write_hook      = begin_write;
	end_write_hook        = end_write;
	flush_writes_hook     = flush_writes;
	contains_pending_hook = contains_pending;
	close_sequence_caches_hook = close_sequence_caches;
}

/** @brief Links one subtransaction to the column writes it owns. */
struct subtransaction_write_frame
{
	SubTransactionId subtransaction_id;
	column_write_state* write_state;
	subtransaction_write_frame* next;
};


/** @brief Writer stack and DROP state for one relation file. */
struct write_state_map_entry
{
	Oid relation_file_number;

	bool dropped;
	SubTransactionId drop_subtransaction_id;

	subtransaction_write_frame* write_state_stack;
};


static bioseq::postgres::support::transaction_scoped_hash_table<
	write_state_map_entry> write_state_cache{"column store write state map"};

/**
 * @brief Complete one subtransaction's writes or update its DROP marker.
 * @param[in,out] entry Relation entry, or nullptr if none exists.
 * @param[in] current_sub_xid Subtransaction being completed.
 * @param[in] parent_sub_xid Parent that inherits a committed DROP marker.
 * @param[in] commit Flush writes on commit, or discard them on abort.
 */
static void
pop_current_write_state_entry(write_state_map_entry* entry,
							SubTransactionId current_sub_xid,
							SubTransactionId parent_sub_xid,
							bool commit)
{
	if (entry == nullptr)
	{
		return;
	}

	/* A DROP marker can remain after the last writer frame is removed.
	 * Handle it first so abort restores the relation's state even when
	 * the stack is empty. Commit transfers the marker to the parent. */
	if (entry->dropped)
	{
		if (entry->drop_subtransaction_id == current_sub_xid)
		{
			if (commit)
			{
				entry->drop_subtransaction_id = parent_sub_xid;
			}
			else
			{
				entry->dropped = false;
			}
		}
		return;
	}

	if (entry->write_state_stack == nullptr)
	{
		return;
	}

	subtransaction_write_frame* stack_head = entry->write_state_stack;
	if (stack_head->subtransaction_id != current_sub_xid)
	{
		return;
	}

	// Commit or discard sequence-cache deltas before ending the write state.
	if (stack_head->write_state->seq_caches && close_sequence_caches_hook)
	{
		close_sequence_caches_hook(stack_head->write_state->seq_caches, commit);
		stack_head->write_state->seq_caches = nullptr;
	}
	if (commit)
	{
		end_write_hook(stack_head->write_state);
	}
	entry->write_state_stack = stack_head->next;
}

/** @brief Reuse or create a relation writer for the current subtransaction.
 *
 * Table rewrites copy options from source_relation_oid while writing to the
 * destination relation. Ordinary inserts use the same relation for both.
 *
 * @param[in] relation Open destination relation.
 * @param[in] tuple_descriptor Tuple layout valid while the writer is alive.
 * @param[in] source_relation_oid Relation whose options control this write.
 * @param[in] current_sub_xid Subtransaction that owns the writer.
 * @return Writer owned by this relation's current transaction frame.
 */
export column_write_state*
initialize_column_write_state(Relation relation, TupleDesc tuple_descriptor,
						Oid source_relation_oid, SubTransactionId current_sub_xid)
{
	bioseq_log<logging_level::debug>("[bioseq] pg: initialize_column_write_state() sub_xid={} tup_slot_rel={}", current_sub_xid, source_relation_oid);

	const bioseq::postgres::support::transaction_hash_insert_result<
		write_state_map_entry> insertion =
			write_state_cache.find_or_insert(&relation->rd_locator.relNumber);
	write_state_map_entry* hash_entry = insertion.entry;

	if (!insertion.was_present)
	{
		hash_entry->write_state_stack = nullptr;
		hash_entry->dropped = false;
	}

	Assert(!hash_entry->dropped);

	if (hash_entry->write_state_stack != nullptr &&
		hash_entry->write_state_stack->subtransaction_id == current_sub_xid)
	{
		return hash_entry->write_state_stack->write_state;
	}

	column_options bioseq_column_options = { 0 };
	/* Preserve the source table's options when a rewrite uses a new destination. */
	bioseq_column_options =
		read_column_options(source_relation_oid)
			.value_or(column_options{});

	column_write_state* fresh_write_state =
		bioseq::postgres::support::invoke_pg_as_cpp(
		[relation, bioseq_column_options, tuple_descriptor] {
		const MemoryContext previous =
			MemoryContextSwitchTo(write_state_cache.get_memory_context());
		column_write_state* result = begin_write_hook(
			relation->rd_locator, bioseq_column_options, tuple_descriptor);
		MemoryContextSwitchTo(previous);
		return result;
	});
	// Sequence caches are initialised lazily by the tuple rewrite path.
	fresh_write_state->seq_caches = nullptr;

	subtransaction_write_frame* stack_entry =
		write_state_cache.allocate_in_context<subtransaction_write_frame>();
	stack_entry->write_state = fresh_write_state;
	stack_entry->subtransaction_id = current_sub_xid;
	stack_entry->next = hash_entry->write_state_stack;
	hash_entry->write_state_stack = stack_entry;

	return stack_entry->write_state;
}


/** @brief Flush this subtransaction's writer while keeping it open.
 * @param[in] relation_file_number Relation file number.
 * @param[in] current_sub_xid Subtransaction that must own the stack head.
 *
 * Return without action if no matching writer exists.
 */
export void
flush_relation_write_state(Oid relation_file_number, SubTransactionId current_sub_xid)
{
	write_state_map_entry* entry = write_state_cache.find(&relation_file_number);
	Assert(!entry || !entry->dropped);

	if (entry != nullptr && entry->write_state_stack != nullptr)
	{
		subtransaction_write_frame* stack_entry = entry->write_state_stack;
		if (stack_entry->subtransaction_id == current_sub_xid)
		{
			flush_writes_hook(stack_entry->write_state);
		}
	}
}


/** @brief Flush or discard completed writers across all relations.
 *
 * Commit transfers DROP markers to the parent. Abort clears them.
 *
 * @param[in] current_sub_xid Subtransaction being completed.
 * @param[in] parent_sub_xid Parent that inherits committed DROP markers.
 * @param[in] commit Flush on commit, or discard on abort.
 */
export void
pop_all_column_write_states(SubTransactionId current_sub_xid,
								SubTransactionId parent_sub_xid,
								bool commit)
{
	write_state_cache.for_each(
		[current_sub_xid, parent_sub_xid, commit](
				write_state_map_entry* entry) {
			pop_current_write_state_entry(
				entry, current_sub_xid, parent_sub_xid, commit);
		});
}

/**
 * @brief Complete one relation's writer for a subtransaction.
 * @param[in] relation_file_number Relation file number.
 * @param[in] current_sub_xid Subtransaction being completed.
 * @param[in] parent_sub_xid Parent that inherits a committed DROP marker.
 * @param[in] commit Flush on commit, or discard on abort.
 */
export void
pop_relation_column_write_state(Oid relation_file_number,
									SubTransactionId current_sub_xid,
									SubTransactionId parent_sub_xid,
									bool commit)
{
	write_state_map_entry* entry = write_state_cache.find(&relation_file_number);
	pop_current_write_state_entry(entry, current_sub_xid, parent_sub_xid, commit);
}


/**
 * @brief Mark an existing relation entry as dropped by this subtransaction.
 * @param[in] relation_file_number Relation file number.
 * @param[in] current_sub_xid Subtransaction whose abort can undo the marker.
 */
export void
mark_column_write_state_relation_dropped(Oid relation_file_number, SubTransactionId current_sub_xid)
{
	write_state_map_entry* entry = write_state_cache.find(&relation_file_number);
	if (entry == nullptr || entry->dropped)
	{
		return;
	}

	entry->dropped = true;
	entry->drop_subtransaction_id = current_sub_xid;
}


/**
 * @brief Remove a dropped relation's entry outside subtransaction callbacks.
 * @param[in] relation_file_number Relation file number to remove from the cache.
 */
export void
drop_column_write_state_outside_transaction(Oid relation_file_number)
{
	write_state_cache.remove(&relation_file_number);
}


/**
 * @brief Check whether parent subtransactions have buffered writes.
 * @param[in] relation_file_number Relation file number.
 * @param[in] current_sub_xid Current subtransaction to exclude.
 * @return true if another frame has pending column data.
 */
export bool
column_writes_pending_in_parent_transactions(
	Oid relation_file_number,
	SubTransactionId current_sub_xid)
{
	write_state_map_entry* entry = write_state_cache.find(&relation_file_number);
	if (entry == nullptr || entry->write_state_stack == nullptr)
	{
		return false;
	}

	for (subtransaction_write_frame* stack_entry :
		 bioseq::postgres::support::intrusive_list<subtransaction_write_frame>{
			 entry->write_state_stack})
	{
		if (stack_entry->subtransaction_id != current_sub_xid &&
			contains_pending_hook(stack_entry->write_state))
		{
			return true;
		}
	}

	return false;
}


/// @brief Return the memory context that owns pending column writes.
/// @return A context reset at transaction end, or nullptr before the first entry.
export MemoryContext
column_write_memory_context()
{
	return write_state_cache.get_memory_context();
}

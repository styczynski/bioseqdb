/**
 * @file write_state.cppm
 * @brief Transaction-scoped ownership of column write states.
 *
 * Manages column_write_state objects keyed by
 * relation file number, one entry holds a stack of per-subtransaction write states
 * so that nested subtransactions can commit / abort their writes
 * independently before the enclosing transaction sees them.
 *
 * Backing storage is a transaction_scoped_hash_table, which owns lazy HTAB
 * creation, its MemoryContext, and the transaction-end reset callback.
 */
module;

#include <bioseq/pg_compat.h>

export module bioseq.postgres.storage.metadata:write_state;

import :core;
import bioseq.postgres.support;
import bioseq.common.logging;

// Registered callbacks keep transaction-state ownership independent from the
// writer and sequence-storage implementations that it coordinates.
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

/// @brief Registers writer callbacks invoked by transaction-state lifecycle events.
/// @param[in] begin_write Creates a transaction-context state for a relation.
/// @param[in] end_write Flushes and releases a state committed by its subtransaction.
/// @param[in] flush_writes Makes pending writes visible without ending the state.
/// @param[in] contains_pending Tests whether a state still owns buffered writes.
/// @param[in] close_sequence_caches Commits or discards lazily created sequence caches.
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


/**
 * @brief Stores nested write frames and a transactional drop marker for one relation file.
 */
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
 * @brief Commits or discards the current frame and propagates its drop marker.
 * @param[in,out] entry Relation state, or `nullptr` when no writes were observed.
 * @param[in] current_sub_xid Subtransaction being completed.
 * @param[in] parent_sub_xid Parent that inherits committed drop state.
 * @param[in] commit Whether pending writes and cache deltas are committed.
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

	/*
	 * A relation can be dropped after its last write frame was popped by an
	 * earlier commit.  The drop marker is still transactional state and must be
	 * elevated or reverted even when there is no current write frame.  Checking
	 * write_state_stack first strands dropped=true after a failed DROP, making the
	 * restored relation appear permanently empty in this backend.
	 */
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

/** @brief Finds or creates a relation write state for one subtransaction.
 *
 * Returns the existing state when the stack head belongs to the current
 * subtransaction. Otherwise, pushes a state whose column options come from
 * source_relation_oid so table rewrites may write a different target relation.
 *
 * @param[in] relation Open destination relation borrowed for state creation.
 * @param[in] tuple_descriptor Tuple layout valid for the state lifetime.
 * @param[in] source_relation_oid Relation whose persisted options govern the write.
 * @param[in] current_sub_xid Subtransaction that owns the returned state.
 * @return Cached transaction-context state owned by the relation frame.
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
	/*
	 * In case of a table rewrite, we need to fetch table options based on the
	 * relation id of the source tuple slot. For normal writes this is the
	 * same as the target relation.
	 */
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


/** @brief Flushes the current subtransaction's relation write state in place.
 *
 * No-op if the relation has no state owned by the current subtransaction.
 *
 * @param[in] relation_file_number Physical relation key.
 * @param[in] current_sub_xid Subtransaction allowed to flush the stack head.
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


/** @brief Finalizes the current write frame for every touched relation.
 *
 * Flushes matching frames on commit and discards them on abort. Drop markers
 * owned by this subtransaction propagate to the parent on commit and are
 * reverted on abort.
 *
 * @param[in] current_sub_xid Subtransaction being completed.
 * @param[in] parent_sub_xid Parent that receives committed state.
 * @param[in] commit Whether frames are committed instead of discarded.
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
 * @brief Finalizes one relation's current write frame.
 * @param[in] relation_file_number Physical relation key.
 * @param[in] current_sub_xid Subtransaction being completed.
 * @param[in] parent_sub_xid Parent that receives committed drop state.
 * @param[in] commit Whether the frame is committed instead of discarded.
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
 * @brief Marks a relation write state as dropped in a subtransaction.
 * @param[in] relation_file_number Physical relation key.
 * @param[in] current_sub_xid Subtransaction that owns the reversible marker.
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
 * @brief Removes state for a relation dropped outside subtransaction cleanup.
 * @param[in] relation_file_number Physical relation key removed from the cache.
 */
export void
drop_column_write_state_outside_transaction(Oid relation_file_number)
{
	write_state_cache.remove(&relation_file_number);
}


/**
 * @brief Tests whether an ancestor subtransaction owns pending column writes.
 * @param[in] relation_file_number Physical relation key.
 * @param[in] current_sub_xid Current frame excluded from the ancestor search.
 * @return `true` when another frame still owns buffered writes.
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


/// @brief Returns the memory context owning transaction-scoped write states.
/// @return Context reset with the surrounding top-level transaction.
export MemoryContext
column_write_memory_context()
{
	return write_state_cache.get_memory_context();
}

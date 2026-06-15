/*-------------------------------------------------------------------------
 *
 * write_state_management.cppm
 *
 * Transaction-scoped management of BioseqColumnWriteState objects. Keyed by
 * relfilenode, one entry holds a stack of per-subtransaction write states
 * so that nested subtransactions can commit / abort their writes
 * independently before the enclosing transaction sees them.
 *
 * Backing storage is a pg_utils::TxnScopedHtab; the helper takes care of
 * lazy HTAB creation, MemoryContext ownership and the transaction-end
 * reset callback.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

export module storage_metadata:write_state;

import :core;           // ReadBioseqColumnOptions
import pg_utils;
import mlogging;

// DOC0322 §7.3 (hook realization) — the writer (BioseqColumnBeginWrite/EndWrite/
// FlushPendingWrites/ContainsPendingWrites) and the seq_caches teardown
// (bioseq_storage) used to be reached directly from this module, forming a
// cross-module cycle (storage_metadata <-> bioseq_table_io / bioseq_storage).
// We now invert those dependencies: the owning modules register function
// pointers at init (see register_write_state_hooks), and this partition calls
// through them. These statics are module-internal and default to nullptr until
// access_method wires them in bioseq_tableam_init.
using begin_write_fn      = BioseqColumnWriteState* (*)(RelFileLocator, BioseqColumnOptions, TupleDesc);
using end_write_fn        = void (*)(BioseqColumnWriteState*);
using flush_writes_fn     = void (*)(BioseqColumnWriteState*);
using contains_pending_fn = bool (*)(BioseqColumnWriteState*);
using seq_caches_close_fn = void (*)(void* seq_caches, bool commit);   // does close_all + destroy + null

static begin_write_fn      g_begin_write      = nullptr;
static end_write_fn        g_end_write        = nullptr;
static flush_writes_fn     g_flush_writes     = nullptr;
static contains_pending_fn g_contains_pending = nullptr;
static seq_caches_close_fn g_seq_caches_close = nullptr;

export void
register_write_state_hooks(begin_write_fn begin_write,
						   end_write_fn end_write,
						   flush_writes_fn flush_writes,
						   contains_pending_fn contains_pending,
						   seq_caches_close_fn seq_caches_close)
{
	g_begin_write      = begin_write;
	g_end_write        = end_write;
	g_flush_writes     = flush_writes;
	g_contains_pending = contains_pending;
	g_seq_caches_close = seq_caches_close;
}

/*
 * Per-subtransaction node. Contains the BioseqColumnWriteState for inserts
 * performed inside the given subtransaction.
 */
struct BioseqColumnSubXidWriteFrame
{
	SubTransactionId subXid;
	BioseqColumnWriteState* writeState;
	BioseqColumnSubXidWriteFrame* next;
};


/*
 * Per-relfilenode map entry. dropped/dropSubXid record that the relation
 * has been dropped inside a subtransaction so subsequent commit/abort of
 * that subtransaction either elevates or undoes the drop.
 */
struct WriteStateMapEntry
{
	Oid relfilenode;

	bool dropped;
	SubTransactionId dropSubXid;

	BioseqColumnSubXidWriteFrame* writeStateStack;
};


static TxnScopedHtab<WriteStateMapEntry> WriteStateCache{"column store write state map"};

static void
PopWriteStateEntryIfCurrent(WriteStateMapEntry* entry,
							SubTransactionId current_sub_xid,
							SubTransactionId parent_sub_xid,
							bool commit)
{
	if (entry == nullptr || entry->writeStateStack == nullptr)
	{
		return;
	}

	if (entry->dropped)
	{
		if (entry->dropSubXid == current_sub_xid)
		{
			if (commit)
			{
				entry->dropSubXid = parent_sub_xid;
			}
			else
			{
				entry->dropped = false;
			}
		}
		return;
	}

	BioseqColumnSubXidWriteFrame* stack_head = entry->writeStateStack;
	if (stack_head->subXid != current_sub_xid)
	{
		return;
	}

	// DOC0260 Phase A — flush per-(parent, column, bucket, shard)
	// bytes_written deltas via the lazily-allocated seq_caches. The teardown
	// (close_all + destroy + null) is owned by bioseq_storage and invoked
	// through the registered hook (DOC0322 §7.3) to avoid a module cycle.
	if (stack_head->writeState->seq_caches && g_seq_caches_close)
	{
		g_seq_caches_close(stack_head->writeState->seq_caches, commit);
		stack_head->writeState->seq_caches = nullptr;
	}
	if (commit)
	{
		g_end_write(stack_head->writeState);
	}
	entry->writeStateStack = stack_head->next;
}

/*
 * bioseq_init_write_state locates or creates a write state for the given
 * relation and subtransaction. Returns the existing state when the top of
 * the stack matches the current subtransaction; otherwise pushes a fresh
 * BioseqColumnWriteState that inherits columnar options from tupSlotRelationId
 * (to support table-rewrite scenarios where source and target differ).
 */
export BioseqColumnWriteState*
bioseq_init_write_state(Relation relation, TupleDesc tuple_descriptor,
						Oid tuple_slot_relation_id, SubTransactionId current_sub_xid)
{
	bioseq_log<DBG>("[bioseq] pg: bioseq_init_write_state() sub_xid={} tup_slot_rel={}", current_sub_xid, tuple_slot_relation_id);

	bool found_existing = false;
	WriteStateMapEntry* hash_entry =
		WriteStateCache.find_or_insert(&relation->rd_locator.relNumber, &found_existing);

	if (!found_existing)
	{
		hash_entry->writeStateStack = nullptr;
		hash_entry->dropped = false;
	}

	Assert(!hash_entry->dropped);

	if (hash_entry->writeStateStack != nullptr &&
		hash_entry->writeStateStack->subXid == current_sub_xid)
	{
		return hash_entry->writeStateStack->writeState;
	}

	BioseqColumnOptions bioseq_column_options = { 0 };
	/*
	 * In case of a table rewrite, we need to fetch table options based on the
	 * relation id of the source tuple slot. For normal writes this is the
	 * same as the target relation.
	 */
	ReadBioseqColumnOptions(tuple_slot_relation_id, &bioseq_column_options);

	BioseqColumnWriteState* fresh_write_state = nullptr;
	{
		MemoryContextScope context_guard(WriteStateCache.context());
		fresh_write_state = g_begin_write(relation->rd_locator,
										  bioseq_column_options,
										  tuple_descriptor);
		// DOC0260 Phase A — seq_caches is initialised lazily by the
		// TAM-side rewrite helper (rewrite_seq_columns_to_stubs in
		// access_method.cppm).
		fresh_write_state->seq_caches = nullptr;
	}

	BioseqColumnSubXidWriteFrame* stack_entry = WriteStateCache.alloc_in_context<BioseqColumnSubXidWriteFrame>();
	stack_entry->writeState = fresh_write_state;
	stack_entry->subXid = current_sub_xid;
	stack_entry->next = hash_entry->writeStateStack;
	hash_entry->writeStateStack = stack_entry;

	return stack_entry->writeState;
}


/*
 * FlushWriteStateForRelfilenode flushes (but does not pop) the top-of-stack
 * write state of the given relation if it belongs to the current
 * subtransaction. No-op if the relation has no pending writes.
 */
export void
FlushWriteStateForRelfilenode(Oid relfilenode, SubTransactionId current_sub_xid)
{
	WriteStateMapEntry* entry = WriteStateCache.find(&relfilenode);
	Assert(!entry || !entry->dropped);

	if (entry != nullptr && entry->writeStateStack != nullptr)
	{
		BioseqColumnSubXidWriteFrame* stack_entry = entry->writeStateStack;
		if (stack_entry->subXid == current_sub_xid)
		{
			g_flush_writes(stack_entry->writeState);
		}
	}
}


/*
 * BioseqColumnPopWriteStateForAllRels pops the top-of-stack write state for
 * every relation whose top matches the current subtransaction. Either
 * flushes them (commit == true) or discards them (commit == false). Drops
 * that occurred in the current subtransaction are either elevated to the
 * parent or reverted depending on commit.
 */
export void
BioseqColumnPopWriteStateForAllRels(SubTransactionId current_sub_xid,
								SubTransactionId parent_sub_xid,
								bool commit)
{
	WriteStateCache.for_each([&](WriteStateMapEntry* entry) {
		PopWriteStateEntryIfCurrent(entry, current_sub_xid, parent_sub_xid, commit);
	});
}

export void
BioseqColumnPopWriteStateForRelfilenode(Oid relfilenode,
									SubTransactionId current_sub_xid,
									SubTransactionId parent_sub_xid,
									bool commit)
{
	WriteStateMapEntry* entry = WriteStateCache.find(&relfilenode);
	PopWriteStateEntryIfCurrent(entry, current_sub_xid, parent_sub_xid, commit);
}


/*
 * BioseqColumnMarkRelfilenodeDroppedBioseqColumn records that the given relation
 * has been dropped inside the given subtransaction, so later lifecycle
 * hooks can decide whether to commit or revert the drop.
 */
export void
BioseqColumnMarkRelfilenodeDroppedBioseqColumn(Oid relfilenode, SubTransactionId current_sub_xid)
{
	WriteStateMapEntry* entry = WriteStateCache.find(&relfilenode);
	if (entry == nullptr || entry->dropped)
	{
		return;
	}

	entry->dropped = true;
	entry->dropSubXid = current_sub_xid;
}


/*
 * BioseqColumnNonTransactionDropWriteState removes the entry for a relation
 * that has been dropped outside of any subtransaction lifecycle (for
 * example TRUNCATE).
 */
export void
BioseqColumnNonTransactionDropWriteState(Oid relfilenode)
{
	WriteStateCache.remove(&relfilenode);
}


/*
 * BioseqColumnPendingWritesInUpperTransactions returns true if any ancestor
 * subtransaction (not the current one) has pending writes for the given
 * relation. Used to decide whether a read would bypass uncommitted rows.
 */
export bool
BioseqColumnPendingWritesInUpperTransactions(Oid relfilenode, SubTransactionId current_sub_xid)
{
	WriteStateMapEntry* entry = WriteStateCache.find(&relfilenode);
	if (entry == nullptr || entry->writeStateStack == nullptr)
	{
		return false;
	}

	for (auto* stack_entry : IntrusiveList<BioseqColumnSubXidWriteFrame>(entry->writeStateStack))
	{
		if (stack_entry->subXid != current_sub_xid &&
			g_contains_pending(stack_entry->writeState))
		{
			return true;
		}
	}

	return false;
}


/*
 * GetBioseqColumnWriteContextForDebug exposes the internal WriteStateContext
 * for diagnostics (e.g. the memory-statistics SRF in columnar_debug).
 */
export MemoryContext
GetBioseqColumnWriteContextForDebug()
{
	return WriteStateCache.context();
}

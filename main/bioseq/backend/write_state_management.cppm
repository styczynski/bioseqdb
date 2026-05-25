/*-------------------------------------------------------------------------
 *
 * write_state_management.cppm
 *
 * Transaction-scoped management of ColumnarWriteState objects. Keyed by
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
#include "columnar/columnar.h"

#include "columnar/columnar_tableam.h"

export module bioseq_write_state_management;

import bioseq_table;
import storage_metadata;
import bioseq_table_io;
import pg_utils;
import mlogging;

// DOC0260 Phase A — forward declarations of the small bioseq_storage
// surface we need at write-state teardown. Avoids `import bioseq_storage`
// here (which would otherwise create a cycle with bioseq_table because
// bioseq_table:access_method also imports bioseq_storage). The functions
// are defined inside `bioseq_storage:sequence_storage_manager` and have
// external (C++) linkage so the linker resolves them at final link time.
namespace bioseq::storage {
    struct seq_caches;
    void destroy_seq_caches(seq_caches* caches) noexcept;
}
extern "C++" {
    void bioseq_storage_seq_caches_close_all(bioseq::storage::seq_caches* c,
                                             bool commit) noexcept;
}

extern "C++" {

/*
 * Per-subtransaction node. Contains the ColumnarWriteState for inserts
 * performed inside the given subtransaction.
 */
struct ColumnarSubXidWriteFrame
{
	SubTransactionId subXid;
	ColumnarWriteState* writeState;
	ColumnarSubXidWriteFrame* next;
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

	ColumnarSubXidWriteFrame* writeStateStack;
};


static TxnScopedHtab<WriteStateMapEntry> WriteStateCache{"column store write state map"};


/*
 * bioseq_init_write_state locates or creates a write state for the given
 * relation and subtransaction. Returns the existing state when the top of
 * the stack matches the current subtransaction; otherwise pushes a fresh
 * ColumnarWriteState that inherits columnar options from tupSlotRelationId
 * (to support table-rewrite scenarios where source and target differ).
 */
ColumnarWriteState*
bioseq_init_write_state(Relation relation, TupleDesc tuple_descriptor,
						Oid tuple_slot_relation_id, SubTransactionId current_sub_xid)
{
	JustLog("[bioseq] pg: bioseq_init_write_state() sub_xid={} tup_slot_rel={}", current_sub_xid, tuple_slot_relation_id);

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

	ColumnarOptions columnar_options = { 0 };
	/*
	 * In case of a table rewrite, we need to fetch table options based on the
	 * relation id of the source tuple slot. For normal writes this is the
	 * same as the target relation.
	 */
	ReadColumnarOptions(tuple_slot_relation_id, &columnar_options);

	ColumnarWriteState* fresh_write_state = nullptr;
	{
		MemoryContextScope context_guard(WriteStateCache.context());
		fresh_write_state = ColumnarBeginWrite(relation->rd_locator,
											   columnar_options,
											   tuple_descriptor);
		// DOC0260 Phase A — seq_caches is initialised lazily by the
		// TAM-side rewrite helper (rewrite_seq_columns_to_stubs in
		// access_method.cppm). Leave it null here so we do not force
		// `import bioseq_storage` in this TU (would create a cycle with
		// bioseq_table).
		fresh_write_state->seq_caches = nullptr;
	}

	ColumnarSubXidWriteFrame* stack_entry = WriteStateCache.alloc_in_context<ColumnarSubXidWriteFrame>();
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
void
FlushWriteStateForRelfilenode(Oid relfilenode, SubTransactionId current_sub_xid)
{
	WriteStateMapEntry* entry = WriteStateCache.find(&relfilenode);
	Assert(!entry || !entry->dropped);

	if (entry != nullptr && entry->writeStateStack != nullptr)
	{
		ColumnarSubXidWriteFrame* stack_entry = entry->writeStateStack;
		if (stack_entry->subXid == current_sub_xid)
		{
			ColumnarFlushPendingWrites(stack_entry->writeState);
		}
	}
}


/*
 * ColumnarPopWriteStateForAllRels pops the top-of-stack write state for
 * every relation whose top matches the current subtransaction. Either
 * flushes them (commit == true) or discards them (commit == false). Drops
 * that occurred in the current subtransaction are either elevated to the
 * parent or reverted depending on commit.
 */
void
ColumnarPopWriteStateForAllRels(SubTransactionId current_sub_xid,
								SubTransactionId parent_sub_xid,
								bool commit)
{
	WriteStateCache.for_each([&](WriteStateMapEntry* entry) {
		if (entry->writeStateStack == nullptr)
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

		ColumnarSubXidWriteFrame* stack_head = entry->writeStateStack;
		if (stack_head->subXid == current_sub_xid)
		{
			// DOC0260 Phase A — flush per-(parent, column, bucket, shard)
			// bytes_written deltas via the lazily-allocated seq_caches.
			// Forward-declared symbols are resolved at link time
			// (definitions live in bioseq_storage:sequence_storage_manager).
			if (stack_head->writeState->seq_caches != nullptr)
			{
				auto* caches = static_cast<bioseq::storage::seq_caches*>(
					stack_head->writeState->seq_caches);
				bioseq_storage_seq_caches_close_all(caches, commit);
				bioseq::storage::destroy_seq_caches(caches);
				stack_head->writeState->seq_caches = nullptr;
			}
			if (commit)
			{
				ColumnarEndWrite(stack_head->writeState);
			}
			entry->writeStateStack = stack_head->next;
		}
	});
}


/*
 * ColumnarMarkRelfilenodeDroppedColumnar records that the given relation
 * has been dropped inside the given subtransaction, so later lifecycle
 * hooks can decide whether to commit or revert the drop.
 */
void
ColumnarMarkRelfilenodeDroppedColumnar(Oid relfilenode, SubTransactionId current_sub_xid)
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
 * ColumnarNonTransactionDropWriteState removes the entry for a relation
 * that has been dropped outside of any subtransaction lifecycle (for
 * example TRUNCATE).
 */
void
ColumnarNonTransactionDropWriteState(Oid relfilenode)
{
	WriteStateCache.remove(&relfilenode);
}


/*
 * ColumnarPendingWritesInUpperTransactions returns true if any ancestor
 * subtransaction (not the current one) has pending writes for the given
 * relation. Used to decide whether a read would bypass uncommitted rows.
 */
bool
ColumnarPendingWritesInUpperTransactions(Oid relfilenode, SubTransactionId current_sub_xid)
{
	WriteStateMapEntry* entry = WriteStateCache.find(&relfilenode);
	if (entry == nullptr || entry->writeStateStack == nullptr)
	{
		return false;
	}

	for (auto* stack_entry : IntrusiveList<ColumnarSubXidWriteFrame>(entry->writeStateStack))
	{
		if (stack_entry->subXid != current_sub_xid &&
			ContainsPendingWrites(stack_entry->writeState))
		{
			return true;
		}
	}

	return false;
}


/*
 * GetColumnarWriteContextForDebug exposes the internal WriteStateContext
 * for diagnostics (e.g. the memory-statistics SRF in columnar_debug).
 */
extern MemoryContext
GetColumnarWriteContextForDebug()
{
	return WriteStateCache.context();
}

}

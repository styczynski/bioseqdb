/*-------------------------------------------------------------------------
 *
 * columnar_read_state_cache.cppm
 *
 * Transaction-scoped cache of ColumnarReadState pointers keyed by
 * relfilelocator. When tuple_fetch_row_version is invoked across separate
 * SPI/executor calls we need to reuse the prior read state instead of
 * building a new one, because read-state setup is not shared between calls.
 *
 * The underlying hash table lives in a TopTransactionContext child; the
 * TxnScopedHtab helper from pg_utils installs the reset callback so we
 * drop to a clean slate at transaction end.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include "columnar/columnar_tableam.h"

export module bioseq_read_state_cache;

import mlogging;
import bioseq_table_io;
import pg_utils;
import pg_utils.lists;

extern "C++" {

/*
 * Per-subtransaction node in the stack hanging off each map entry. The
 * readState pointer is owned by the caller via the ** returned from
 * InitColumnarReadStateCache.
 */
struct ColumnarSubXidReadFrame
{
	SubTransactionId subXid;
	ColumnarReadState* readState;
	ColumnarSubXidReadFrame* next;
};

/*
 * Per-relfilelocator entry. readStateStack is a singly-linked stack of
 * per-subtransaction frames ordered top-of-stack first.
 */
struct ColumnarReadStateMapEntry
{
	Oid relfilelocator;
	ColumnarSubXidReadFrame* readStateStack;
};

static TxnScopedHtab<ColumnarReadStateMapEntry> ReadStateCache{"columnar read state map"};


/*
 * InitColumnarReadStateCache locates (or creates) the read-state slot for
 * the given relation/subtransaction and returns a ColumnarReadState**. The
 * caller writes the freshly-constructed ColumnarReadState* through the
 * returned pointer; subsequent FindReadStateCache calls retrieve it.
 */
ColumnarReadState**
InitColumnarReadStateCache(Relation relation, SubTransactionId current_sub_xid)
{
	JustLog("[bioseq] pg: InitColumnarReadStateCache() sub_xid={}", current_sub_xid);

	bool found_existing = false;
	ColumnarReadStateMapEntry* hash_entry =
		ReadStateCache.find_or_insert(&relation->rd_locator.relNumber, &found_existing);

	if (!found_existing)
	{
		hash_entry->readStateStack = nullptr;
	}

	if (hash_entry->readStateStack != nullptr &&
		hash_entry->readStateStack->subXid == current_sub_xid)
	{
		return &hash_entry->readStateStack->readState;
	}

	ColumnarSubXidReadFrame* stack_entry = ReadStateCache.alloc_in_context<ColumnarSubXidReadFrame>();
	stack_entry->subXid = current_sub_xid;
	stack_entry->next = hash_entry->readStateStack;
	hash_entry->readStateStack = stack_entry;

	return &stack_entry->readState;
}


/*
 * FindReadStateCache returns a pointer to the slot holding the cached
 * ColumnarReadState* for the given relation/subtransaction, or NULL when no
 * entry exists. Returning ** (not *) lets callers update the cached state.
 */
ColumnarReadState**
FindReadStateCache(Relation relation, SubTransactionId current_sub_xid)
{
	ColumnarReadStateMapEntry* hash_entry =
		ReadStateCache.find(&relation->rd_locator.relNumber);
	if (hash_entry == nullptr)
	{
		return nullptr;
	}

	for (auto* stack_frame : IntrusiveList<ColumnarSubXidReadFrame>(hash_entry->readStateStack))
	{
		if (stack_frame->subXid == current_sub_xid)
		{
			return &stack_frame->readState;
		}
	}

	return nullptr;
}


/*
 * CleanupReadStateCache closes every cached ColumnarReadState created in
 * the given subtransaction and pops the matching stack entries. Called at
 * subtransaction end so stale state is not reused later.
 */
void
CleanupReadStateCache(SubTransactionId current_sub_xid)
{
	JustLog("[bioseq] pg: CleanupReadStateCache() sub_xid={}", current_sub_xid);

	ReadStateCache.for_each([current_sub_xid](ColumnarReadStateMapEntry* entry) {
		ColumnarSubXidReadFrame* stack_head = entry->readStateStack;
		if (stack_head == nullptr)
		{
			return;
		}
		if (stack_head->subXid == current_sub_xid)
		{
			ColumnarEndRead(stack_head->readState);
			entry->readStateStack = stack_head->next;
		}
	});
}


/*
 * GetColumnarReadStateCache exposes the underlying memory context for
 * debugging helpers that want to inspect cache memory usage.
 */
MemoryContext GetColumnarReadStateCache()
{
	return ReadStateCache.context();
}

}

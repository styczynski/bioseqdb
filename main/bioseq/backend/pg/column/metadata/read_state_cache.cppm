/*-------------------------------------------------------------------------
 *
 * columnar_read_state_cache.cppm
 *
 * Transaction-scoped cache of BioseqColumnReadState pointers keyed by
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

export module bioseq.storage.metadata:read_state_cache;

import :core;        // DOC0322 §9.2 — BioseqColumnReadState (was types.h, now defined in :core)
import bioseq.common.mlogging;
import bioseq.pg_utils;

// DOC0322 §7.2/§7.3 (hook realization) — BioseqColumnEndRead lives in
// bioseq_table_io:reader, which imports storage_metadata; importing it here
// would close a cycle. Invert it: the reader registers its teardown at init
// (register_read_state_hooks) and CleanupReadStateCache calls through this
// module-internal pointer (nullptr until access_method wires it).
using end_read_fn = void (*)(BioseqColumnReadState*);
static end_read_fn g_end_read = nullptr;

export void
register_read_state_hooks(end_read_fn end_read)
{
	g_end_read = end_read;
}

/*
 * Per-subtransaction node in the stack hanging off each map entry. The
 * readState pointer is owned by the caller via the ** returned from
 * InitBioseqColumnReadStateCache.
 */
struct BioseqColumnSubXidReadFrame
{
	SubTransactionId subXid;
	BioseqColumnReadState* readState;
	BioseqColumnSubXidReadFrame* next;
};

/*
 * Per-relfilelocator entry. readStateStack is a singly-linked stack of
 * per-subtransaction frames ordered top-of-stack first.
 */
struct BioseqColumnReadStateMapEntry
{
	Oid relfilelocator;
	BioseqColumnSubXidReadFrame* readStateStack;
};

static TxnScopedHtab<BioseqColumnReadStateMapEntry> ReadStateCache{"columnar read state map"};


/*
 * InitBioseqColumnReadStateCache locates (or creates) the read-state slot for
 * the given relation/subtransaction and returns a BioseqColumnReadState**. The
 * caller writes the freshly-constructed BioseqColumnReadState* through the
 * returned pointer; subsequent FindReadStateCache calls retrieve it.
 */
export BioseqColumnReadState**
InitBioseqColumnReadStateCache(Relation relation, SubTransactionId current_sub_xid)
{
	bioseq_log<DBG>("[bioseq] pg: InitBioseqColumnReadStateCache() sub_xid={}", current_sub_xid);

	bool found_existing = false;
	BioseqColumnReadStateMapEntry* hash_entry =
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

	BioseqColumnSubXidReadFrame* stack_entry = ReadStateCache.alloc_in_context<BioseqColumnSubXidReadFrame>();
	stack_entry->subXid = current_sub_xid;
	stack_entry->next = hash_entry->readStateStack;
	hash_entry->readStateStack = stack_entry;

	return &stack_entry->readState;
}


/*
 * FindReadStateCache returns a pointer to the slot holding the cached
 * BioseqColumnReadState* for the given relation/subtransaction, or NULL when no
 * entry exists. Returning ** (not *) lets callers update the cached state.
 */
export BioseqColumnReadState**
FindReadStateCache(Relation relation, SubTransactionId current_sub_xid)
{
	BioseqColumnReadStateMapEntry* hash_entry =
		ReadStateCache.find(&relation->rd_locator.relNumber);
	if (hash_entry == nullptr)
	{
		return nullptr;
	}

	for (auto* stack_frame : IntrusiveList<BioseqColumnSubXidReadFrame>(hash_entry->readStateStack))
	{
		if (stack_frame->subXid == current_sub_xid)
		{
			return &stack_frame->readState;
		}
	}

	return nullptr;
}


/*
 * CleanupReadStateCache closes every cached BioseqColumnReadState created in
 * the given subtransaction and pops the matching stack entries. Called at
 * subtransaction end so stale state is not reused later.
 */
export void
CleanupReadStateCache(SubTransactionId current_sub_xid)
{
	bioseq_log<DBG>("[bioseq] pg: CleanupReadStateCache() sub_xid={}", current_sub_xid);

	ReadStateCache.for_each([current_sub_xid](BioseqColumnReadStateMapEntry* entry) {
		BioseqColumnSubXidReadFrame* stack_head = entry->readStateStack;
		if (stack_head == nullptr)
		{
			return;
		}
		if (stack_head->subXid == current_sub_xid)
		{
			if (g_end_read) g_end_read(stack_head->readState);
			entry->readStateStack = stack_head->next;
		}
	});
}


/*
 * GetBioseqColumnReadStateCache exposes the underlying memory context for
 * debugging helpers that want to inspect cache memory usage.
 */
export MemoryContext GetBioseqColumnReadStateCache()
{
	return ReadStateCache.context();
}

/*-------------------------------------------------------------------------
 *
 * table/io/columnar_cache.cppm
 *
 * In-memory cache of decompressed column chunks. Entries are keyed by
 * (relid, stripeId, chunkId, columnId) and live in a dedicated memory
 * context (see ColumnarCacheMemoryContext) so eviction and reset are
 * quick. The cache also tracks "in use" chunk groups so concurrent
 * scans don't evict each other's working set.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"
#include "types.h"

#include "columnar/columnar.h"

#include <time.h>
#include <unistd.h>

export module bioseq_table_io:cache;

import pg_utils;
import pg_utils.lists;

void ColumnarEndRead(ColumnarReadState *readState);
void ColumnarResetRead(ColumnarReadState *readState);


/*
 * Main caching MemoryContext.
 *
 * This MemoryContext is created at initialization, below 
 * TopMemoryContext, where all cache entries are stored.
 */
static MemoryContext columnarCacheContext = NULL;

/*
 * Cache entry. dlist_node must remain the first field because the
 * intrusive doubly-linked-list helpers (dlist_*) cast pointers between
 * `dlist_node*` and `ColumnarCacheEntry*` via dlist_container.
 */
struct ColumnarCacheEntry
{
	dlist_node list_node;
	uint64 relId;
	uint64 stripeId;
	uint64 chunkId;
	uint64 readCount;
	uint64 length;
	time_t creationTime;
	time_t lastAccessTime;
	void *store;
	uint32 columnId;
};

/*
 * Storage for the ColumnarCacheEntry linked list.
 */
static dlist_head *head = NULL;

/*
 * Storage for total length allocated.
 */
static uint64 totalAllocationLength = 0;

/*
 * Cache statistics.
 *
 * Hits and misses, and general cache statistics.
 */
export ColumnarCacheStatistics statistics = { 0 };

/*
 * Housekeeping of current chunk in use - so they are not evicted.
 */
struct ColumarCacheChunkGroupInUse
{
	uint64 relId;
	uint64 stripeId;
	uint64 chunkId;
};

static List * ChunkGroupsInUse = NIL;

/*
 * ColumnarCacheMemoryContext
 *
 * Returns the cache MemoryContext, initializing the cache MemoryContext
 * as a child of TopMemoryContext if it does not exist, also clears any
 * statistics gathered.
 */
export MemoryContext
ColumnarCacheMemoryContext(void)
{
	if (columnarCacheContext == NULL)
	{
		columnarCacheContext = 
			AllocSetContextCreate(TopMemoryContext, 
								  "Bioseq Decompression Cache", 
								  0, (uint64) (bioseq_page_cache_size * 1024 * 1024 * .1), 
								  bioseq_page_cache_size * 1024 * 1024);
		memset(&statistics, 0, sizeof(ColumnarCacheStatistics));
		head = NULL;
	}

	return columnarCacheContext;
}

/*
* ColumnarResetCache
*
* Deletes the caching memory context and sets it to NULL, thus removing the
* cache and all of its entries.
*/
export void
ColumnarResetCache(void)
{
	if (columnarCacheContext != NULL)
	{
		MemoryContextDelete(columnarCacheContext);
		columnarCacheContext = NULL;
		ChunkGroupsInUse = NIL;
	}

	totalAllocationLength = 0U;
	head = NULL;
}

/*
 * ColumnarFindInCache
 *
 * Searches the cache for an entry for a relation ID and a chunk ID.
 * If found, it increments the readCount, and returns the entry.	If
 * none are found, NULL is returned instead.
 */
static ColumnarCacheEntry *
ColumnarFindInCache(uint64 relId, uint64 stripeId, uint64 chunkId, uint32 columnId)
{
	if (head == NULL)
	{
		return NULL;
	}

	dlist_iter iter;
	dlist_foreach(iter, head)
	{
		ColumnarCacheEntry *entry = dlist_container(ColumnarCacheEntry, list_node, iter.cur);

		if (entry->relId == relId && entry->stripeId == stripeId &&
			entry->chunkId == chunkId && entry->columnId == columnId)
		{
			entry->readCount++;

			return entry;
		}
	}

	return NULL;
}

/*
 * ColumnarInvalidateCacheEntry searches for a cache entry by
 * (relId, stripeId, chunkId, columnId). On a hit it unlinks the entry,
 * pfrees the backing store + the entry itself, updates the running size
 * counter and the eviction stat, and returns true. Returns false when
 * no matching entry exists.
 */
static bool
ColumnarInvalidateCacheEntry(uint64 relId, uint64 stripeId, uint64 chunkId, uint32 columnId)
{
	if (head == nullptr)
	{
		return false;
	}

	dlist_mutable_iter miter;
	dlist_foreach_modify(miter, head)
	{
		ColumnarCacheEntry *entry = dlist_container(ColumnarCacheEntry, list_node, miter.cur);

		if (entry->relId == relId && entry->stripeId == stripeId &&
			entry->chunkId == chunkId && entry->columnId == columnId)
		{
			dlist_delete(miter.cur);

			totalAllocationLength -= entry->length;
			statistics.evictions++;

			StringInfo store = static_cast<StringInfo>(entry->store);
			if (store->data)
			{
				pfree(store->data);
			}
			pfree(store);
			pfree(entry);

			return true;
		}
	}

	return false;
}

static void
EvictCache(uint64 size)
{
	uint64 lastCount = 0;
	uint64 nextLowestCount = PG_UINT64_MAX;

	while (size > 0)
	{
		dlist_mutable_iter miter;

		dlist_foreach_modify(miter, head)
		{
			ColumnarCacheEntry *entry = dlist_container(ColumnarCacheEntry, list_node, miter.cur);

			if (entry->readCount != lastCount && entry->readCount < nextLowestCount)
			{
				nextLowestCount = entry->readCount;
			}

			if (entry->readCount == lastCount)
			{
				bool skipCacheEntry = false;
				for (auto* chunkGroupInUse : PList<ColumarCacheChunkGroupInUse>(ChunkGroupsInUse))
				{
					if (chunkGroupInUse->relId == entry->relId &&
						chunkGroupInUse->stripeId == entry->stripeId &&
						chunkGroupInUse->chunkId == entry->chunkId)
					{
						skipCacheEntry = true;
						break;
					}
				}

				if (skipCacheEntry)
					continue;

				dlist_delete(miter.cur);

				totalAllocationLength -= entry->length;
				statistics.evictions++;

				StringInfo str = static_cast<StringInfo>(entry->store);
				if (str->data) {
					pfree(str->data);
				}

				pfree(str);

				if (size < entry->length)
				{
					pfree(entry);
					return;
				}
				else {
					size -= entry->length;
					pfree(entry);
				}
			}
		}

		lastCount = nextLowestCount;
		nextLowestCount = PG_UINT64_MAX;
	}
}

export void
ColumnarMarkChunkGroupInUse(uint64 relId, uint64 stripeId, uint32 chunkId)
{
	bool found = false;
	MemoryContext ctx = MemoryContextSwitchTo(ColumnarCacheMemoryContext());

	for (auto* chunkGroupInUse : PList<ColumarCacheChunkGroupInUse>(ChunkGroupsInUse))
	{
		if (chunkGroupInUse->relId == relId)
		{
			chunkGroupInUse->stripeId = stripeId;
			chunkGroupInUse->chunkId = chunkId;
			found = true;
		}
	}

	if (!found)
	{
		ColumarCacheChunkGroupInUse *newChunkGroupInUse = make0<ColumarCacheChunkGroupInUse>();

		newChunkGroupInUse->relId = relId;
		newChunkGroupInUse->stripeId = stripeId;
		newChunkGroupInUse->chunkId = chunkId;

		ChunkGroupsInUse = lappend(ChunkGroupsInUse, newChunkGroupInUse);
	}

	MemoryContextSwitchTo(ctx);
}

/*
 * ColumnarAddCacheEntry
 *
 * Adds a cache entry, or updates an existing entry.
 */
export void
ColumnarAddCacheEntry(uint64 relId, uint64 stripeId, uint64 chunkId, 
					  uint32 columnId, void *data)
{
	if (bioseq_enable_page_cache == false)
	{
		return;
	}

	MemoryContext oldContext = MemoryContextSwitchTo(ColumnarCacheMemoryContext());

	if (head == NULL)
	{
		head = make0<dlist_head>();
	}

	ColumnarCacheEntry *entry = ColumnarFindInCache(relId, stripeId, chunkId, columnId);

	if (entry != NULL)
	{
		/* Free up any existing stored data, everything else will be overwritten. */
		StringInfo str = static_cast<StringInfo>(entry->store);
		if (str->data)
		{
			pfree(str->data);
		}

		pfree(str);

		totalAllocationLength -= entry->length;
	}
	else
	{
		entry = make0<ColumnarCacheEntry>();

		entry->relId = relId;
		entry->stripeId = stripeId;
		entry->chunkId = chunkId;
		entry->columnId = columnId;
		entry->creationTime = entry->lastAccessTime = time(NULL);
		entry->readCount = 0;

		/* Add the entry into the list. */
		dlist_push_tail(head, &(entry->list_node));
	}

	uint64 size = ((StringInfo) data)->len;

	entry->store = data;
	entry->length = size;

	totalAllocationLength += size;

	if (totalAllocationLength >= statistics.maximumCacheSize)
	{
		statistics.maximumCacheSize = totalAllocationLength;
	}

	/* If we are over our cache allocation, clear until we are at 90%. */
	if (totalAllocationLength >= (bioseq_page_cache_size * 1024 * 1024))
	{
		EvictCache((bioseq_page_cache_size * 1024 * 1024 * .1) + 
					(totalAllocationLength - (bioseq_page_cache_size * 1024 * 1024)));
	}

	statistics.writes++;

	MemoryContextSwitchTo(oldContext);
}

/*
 * ColumnarRetrieveCache
 *
 * Search for a cache entry, returning NULL if not found.	If found,
 * make a copy in the current memory context and return it.
 */
export void*
ColumnarRetrieveCache(uint64 relId, uint64 stripeId, uint64 chunkId, uint32 columnId)
{
	if (bioseq_enable_page_cache == false)
	{
		return NULL;
	}

	ColumnarCacheEntry *entry = ColumnarFindInCache(relId, stripeId, chunkId, columnId);

	if (entry == NULL)
	{
		statistics.misses++;

		return NULL;
	}

	statistics.hits++;

	void *chunkCopy = entry->store;

	return chunkCopy;
}

/*
 * ColumnarCacheLength
 *
 * Returns how large our cache is, used for accounting.
 */
static uint64
ColumnarCacheLength()
{
	uint64 count = 0;

	if (head == NULL)
	{
		return 0;
	}

	dlist_iter iter;
	dlist_foreach(iter, head)
	{
		count++;
	}

	return count;
}

export ColumnarCacheStatistics *
ColumnarGetCacheStatistics(void)
{
	statistics.endingCacheSize = totalAllocationLength;
	statistics.entries = ColumnarCacheLength();

	return &statistics;
}


/*
 * cache_evict is a debug SQL function that drops a single cache entry by
 * (relid, stripeId, chunkId, columnId). Only useful when called inside an
 * already-active transaction, or when automatic cache clearing has been
 * disabled.
 */
POSTGRES_FN(cache_evict)
{
	uint64 relId =PG_GETARG_INT64(0);
	uint64 stripeId = PG_GETARG_INT64(1);
	uint64 chunkId = PG_GETARG_INT16(2);
	uint32 columnId = PG_GETARG_UINT32(3);

	bool result = ColumnarInvalidateCacheEntry(relId, stripeId, chunkId, columnId);

	PG_RETURN_BOOL(result);
}

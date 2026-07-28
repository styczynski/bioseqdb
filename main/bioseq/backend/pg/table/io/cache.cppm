/*-------------------------------------------------------------------------
 *
 * table/io/columnar_cache.cppm
 *
 * In-memory cache of decompressed column chunks. Entries are keyed by
 * (relid, stripeId, chunkId, columnId) and live in a dedicated memory
 * context (see BioseqColumnCacheMemoryContext) so eviction and reset are
 * quick. The cache also tracks "in use" chunk groups so concurrent
 * scans don't evict each other's working set.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include <time.h>
#include <unistd.h>

export module bioseq.table.io:cache;

import bioseq.pg_utils;
import bioseq.config;                  // DOC0322 §9.3 — bioseq::settings::enable_column_cache, bioseq::settings::column_cache_size GUCs
import bioseq.storage.metadata;        // DOC0322 §9.2 — BioseqColumnReadState, BioseqColumnCacheStatistics (was types.h)


/*
 * Main caching MemoryContext.
 *
 * This MemoryContext is created at initialization, below 
 * TopMemoryContext, where all cache entries are stored.
 */
static MemoryContext bioseqColumnCacheContext = NULL;

/*
 * Cache entry. dlist_node must remain the first field because the
 * intrusive doubly-linked-list helpers (dlist_*) cast pointers between
 * `dlist_node*` and `BioseqColumnCacheEntry*` via dlist_container.
 */
struct BioseqColumnCacheEntry
{
	dlist_node list_node;
	uint64_t relId;
	uint64_t stripeId;
	uint64_t chunkId;
	uint64_t readCount;
	uint64_t length;
	time_t creationTime;
	time_t lastAccessTime;
	void *store;
	uint32_t columnId;
};

/*
 * Storage for the BioseqColumnCacheEntry linked list.
 */
static dlist_head *head = NULL;

/*
 * Storage for total length allocated.
 */
static uint64_t totalAllocationLength = 0;

/*
 * Cache statistics.
 *
 * Hits and misses, and general cache statistics.
 */
export BioseqColumnCacheStatistics statistics = { 0 };

/*
 * Housekeeping of current chunk in use - so they are not evicted.
 */
struct ColumarCacheChunkGroupInUse
{
	uint64_t relId;
	uint64_t stripeId;
	uint64_t chunkId;
};

static List * ChunkGroupsInUse = NIL;

/*
 * BioseqColumnCacheMemoryContext
 *
 * Returns the cache MemoryContext, initializing the cache MemoryContext
 * as a child of TopMemoryContext if it does not exist, also clears any
 * statistics gathered.
 */
export MemoryContext
BioseqColumnCacheMemoryContext(void)
{
	if (bioseqColumnCacheContext == NULL)
	{
		bioseqColumnCacheContext =
			AllocSetContextCreate(TopMemoryContext,
								  "Bioseq Decompression Cache",
								  0, static_cast<uint64_t>(bioseq::settings::column_cache_size * 1024 * 1024 * .1),
								  bioseq::settings::column_cache_size * 1024 * 1024);
		memset(&statistics, 0, sizeof(BioseqColumnCacheStatistics));
		head = NULL;
	}

	return bioseqColumnCacheContext;
}

/*
* BioseqColumnResetCache
*
* Deletes the caching memory context and sets it to NULL, thus removing the
* cache and all of its entries.
*/
export void
BioseqColumnResetCache(void)
{
	if (bioseqColumnCacheContext != NULL)
	{
		MemoryContextDelete(bioseqColumnCacheContext);
		bioseqColumnCacheContext = NULL;
		ChunkGroupsInUse = NIL;
	}

	totalAllocationLength = 0U;
	head = NULL;
}

/*
 * BioseqColumnFindInCache
 *
 * Searches the cache for an entry for a relation ID and a chunk ID.
 * If found, it increments the readCount, and returns the entry.	If
 * none are found, NULL is returned instead.
 */
static BioseqColumnCacheEntry *
BioseqColumnFindInCache(uint64_t relId, uint64_t stripeId, uint64_t chunkId, uint32_t columnId)
{
	if (head == NULL)
	{
		return NULL;
	}

	dlist_iter iter;
	dlist_foreach(iter, head)
	{
		BioseqColumnCacheEntry *entry = dlist_container(BioseqColumnCacheEntry, list_node, iter.cur);

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
 * BioseqColumnInvalidateCacheEntry searches for a cache entry by
 * (relId, stripeId, chunkId, columnId). On a hit it unlinks the entry,
 * pfrees the backing store + the entry itself, updates the running size
 * counter and the eviction stat, and returns true. Returns false when
 * no matching entry exists.
 */
static bool
BioseqColumnInvalidateCacheEntry(uint64_t relId, uint64_t stripeId, uint64_t chunkId, uint32_t columnId)
{
	if (head == nullptr)
	{
		return false;
	}

	dlist_mutable_iter miter;
	dlist_foreach_modify(miter, head)
	{
		BioseqColumnCacheEntry *entry = dlist_container(BioseqColumnCacheEntry, list_node, miter.cur);

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
EvictCache(uint64_t size)
{
	uint64_t lastCount = 0;
	uint64_t nextLowestCount = PG_UINT64_MAX;

	while (size > 0)
	{
		dlist_mutable_iter miter;

		dlist_foreach_modify(miter, head)
		{
			BioseqColumnCacheEntry *entry = dlist_container(BioseqColumnCacheEntry, list_node, miter.cur);

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
BioseqColumnMarkChunkGroupInUse(uint64_t relId, uint64_t stripeId, uint32_t chunkId)
{
	bool found = false;
	MemoryContextScope ctx_guard{BioseqColumnCacheMemoryContext()};

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
}

/*
 * BioseqColumnAddCacheEntry
 *
 * Adds a cache entry, or updates an existing entry.
 */
export void
BioseqColumnAddCacheEntry(uint64_t relId, uint64_t stripeId, uint64_t chunkId, 
					  uint32_t columnId, void *data)
{
	if (bioseq::settings::enable_column_cache == false)
	{
		return;
	}

	MemoryContextScope ctx_guard{BioseqColumnCacheMemoryContext()};

	if (head == NULL)
	{
		head = make0<dlist_head>();
	}

	BioseqColumnCacheEntry *entry = BioseqColumnFindInCache(relId, stripeId, chunkId, columnId);

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
		entry = make0<BioseqColumnCacheEntry>();

		entry->relId = relId;
		entry->stripeId = stripeId;
		entry->chunkId = chunkId;
		entry->columnId = columnId;
		entry->creationTime = entry->lastAccessTime = time(NULL);
		entry->readCount = 0;

		/* Add the entry into the list. */
		dlist_push_tail(head, &(entry->list_node));
	}

	uint64_t size = static_cast<StringInfo>(data)->len;

	entry->store = data;
	entry->length = size;

	totalAllocationLength += size;

	if (totalAllocationLength >= statistics.maximumCacheSize)
	{
		statistics.maximumCacheSize = totalAllocationLength;
	}

	/* If we are over our cache allocation, clear until we are at 90%. */
	if (totalAllocationLength >= (bioseq::settings::column_cache_size * 1024 * 1024))
	{
		EvictCache((bioseq::settings::column_cache_size * 1024 * 1024 * .1) +
					(totalAllocationLength - (bioseq::settings::column_cache_size * 1024 * 1024)));
	}

	statistics.writes++;
}

/*
 * BioseqColumnRetrieveCache
 *
 * Search for a cache entry, returning NULL if not found.	If found,
 * return the live cached store directly (no copy is made); the caller
 * must not free or take ownership of the returned buffer.
 */
export void*
BioseqColumnRetrieveCache(uint64_t relId, uint64_t stripeId, uint64_t chunkId, uint32_t columnId)
{
	if (bioseq::settings::enable_column_cache == false)
	{
		return NULL;
	}

	BioseqColumnCacheEntry *entry = BioseqColumnFindInCache(relId, stripeId, chunkId, columnId);

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
 * BioseqColumnCacheLength
 *
 * Returns how large our cache is, used for accounting.
 */
static uint64_t
BioseqColumnCacheLength()
{
	uint64_t count = 0;

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

export BioseqColumnCacheStatistics *
BioseqColumnGetCacheStatistics(void)
{
	statistics.endingCacheSize = totalAllocationLength;
	statistics.entries = BioseqColumnCacheLength();

	return &statistics;
}

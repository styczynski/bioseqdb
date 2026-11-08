/**
 * @file
 * @brief Cache decompressed column chunks within  a PostgreSQL backend.
 */
module;

#include <bioseq/pg_compat.h>

#include <cstdint>
#include <cstring>

export module bioseq.postgres.table.io:cache;

import bioseq.postgres.configuration.core;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.support;

namespace {

MemoryContext column_cache_context = nullptr;
dlist_head* cache_entries = nullptr;
std::uint64_t allocated_bytes = 0;
dlist_head chunk_groups_in_use = DLIST_STATIC_INIT(chunk_groups_in_use);
bool column_cache_reset_pending = false;
column_cache_statistics cache_statistics{};

/** One cached column buffer. Keep `list_node` first; key by persistent storage
 * identity because relation OIDs survive rewrite and TRUNCATE. */
struct column_cache_entry {
    dlist_node list_node{};
    std::uint64_t storage_id = 0;
    std::uint64_t stripe_id = 0;
    std::uint64_t chunk_id = 0;
    std::uint64_t read_count = 0;
    std::uint64_t length = 0;  // Retained allocation bytes, including buffer-context overhead.
    void* storage = nullptr;
    MemoryContext storage_context = nullptr;
    std::uint32_t column_id = 0;
};

/** One stripe reader's current chunk, released when its owning context resets. */
struct active_chunk_group {
    dlist_node list_node{};
    MemoryContext owner_context = nullptr;
    MemoryContextCallback cleanup_callback{};
    std::uint64_t storage_id = 0;
    std::uint64_t stripe_id = 0;
    std::uint64_t chunk_id = 0;
};

/** @brief Convert the configured cache capacity from MiB to bytes. */
[[nodiscard]] std::uint64_t cache_capacity_bytes() noexcept {
    return static_cast<std::uint64_t>(
        bioseq::settings::column_cache::capacity_mb) * 1024u * 1024u;
}

/** @brief Check whether a reader still needs this cached chunk group. */
[[nodiscard]] bool entry_is_in_use(const column_cache_entry& entry) {
    dlist_iter iterator;
    dlist_foreach(iterator, &chunk_groups_in_use) {
        const auto* active = dlist_container(
            active_chunk_group, list_node, iterator.cur);
        if (active->storage_id == entry.storage_id
            && active->stripe_id == entry.stripe_id
            && active->chunk_id == entry.chunk_id) {
            return true;
        }
    }
    return false;
}

/** @brief Release the cache-owned buffer referenced by an entry. */
void release_entry_storage(column_cache_entry& entry) noexcept {
    MemoryContextDelete(entry.storage_context);
}

/** Find a column buffer and count this lookup if it is present. */
[[nodiscard]] column_cache_entry* find_column_cache_entry(
        std::uint64_t storage_id,
        std::uint64_t stripe_id,
        std::uint64_t chunk_id,
        std::uint32_t column_id) noexcept {
    if (cache_entries == nullptr) {
        return nullptr;
    }

    dlist_iter iterator;
    dlist_foreach(iterator, cache_entries) {
        auto* entry = dlist_container(
            column_cache_entry, list_node, iterator.cur);
        if (entry->storage_id == storage_id
            && entry->stripe_id == stripe_id
            && entry->chunk_id == chunk_id
            && entry->column_id == column_id) {
            ++entry->read_count;
            return entry;
        }
    }
    return nullptr;
}

/**
 * Free the least-used buffers until enough bytes are released or only active
 * chunk groups remain. Readers borrow values from active groups, so those
 * buffers must stay alive even whev the cache exceeds its configured size.
 */
void evict_column_cache_bytes(std::uint64_t bytes_to_release) {
    if (cache_entries == nullptr) {
        return;
    }
    std::uint64_t current_read_count = 0;

    while (bytes_to_release > 0) {
        std::uint64_t next_read_count = 0;
        bool next_frequency_found = false;
        dlist_mutable_iter iterator;
        dlist_foreach_modify(iterator, cache_entries) {
            auto* entry = dlist_container(
                column_cache_entry, list_node, iterator.cur);
            if (entry_is_in_use(*entry)) {
                continue;
            }
            if (entry->read_count != current_read_count) {
                if (!next_frequency_found || entry->read_count < next_read_count) {
                    next_read_count = entry->read_count;
                    next_frequency_found = true;
                }
                continue;
            }

            dlist_delete(iterator.cur);
            allocated_bytes -= entry->length;
            ++cache_statistics.evictions;
            release_entry_storage(*entry);

            if (bytes_to_release < entry->length) {
                pfree(entry);
                return;
            }
            bytes_to_release -= entry->length;
            pfree(entry);
        }

        if (!next_frequency_found) {
            return;
        }
        current_read_count = next_read_count;
    }
}

/**
 * Reclaim unborrowed buffers after a reset or reader release. Borrowed buffers
 * remain alive until their final owner finishes; no callback deletes its current
 * memory context.
 */
void reclaim_column_cache() noexcept {
    if (column_cache_context == nullptr) {
        column_cache_reset_pending = false;
        return;
    }
    if (column_cache_reset_pending) {
        evict_column_cache_bytes(allocated_bytes);
    } else {
        const std::uint64_t capacity = cache_capacity_bytes();
        if (allocated_bytes > capacity) {
            evict_column_cache_bytes(allocated_bytes - capacity);
        }
    }
    if (column_cache_reset_pending && dlist_is_empty(&chunk_groups_in_use)
        && CurrentMemoryContext != column_cache_context) {
        MemoryContextDelete(column_cache_context);
        column_cache_context = nullptr;
        cache_entries = nullptr;
        allocated_bytes = 0;
        column_cache_reset_pending = false;
    }
}

/**
 * Release one reader's pin before its stripe allocations disappear. This also
 * runs for errors and subtransaction cleanup, independently of the current GUC.
 */
void release_chunk_group_owner(void* argument) noexcept {
    auto* active = static_cast<active_chunk_group*>(argument);
    dlist_delete(&active->list_node);
    reclaim_column_cache();
}

/** @return This stripe context's reusable lease, or null before its first fill. */
[[nodiscard]] active_chunk_group* find_chunk_group_owner(
        MemoryContext owner_context) noexcept {
    dlist_iter iterator;
    dlist_foreach(iterator, &chunk_groups_in_use) {
        auto* active = dlist_container(
            active_chunk_group, list_node, iterator.cur);
        if (active->owner_context == owner_context) {
            return active;
        }
    }
    return nullptr;
}

/** @brief Count entries currently linked into the cache list. */
[[nodiscard]] std::uint64_t count_column_cache_entries() noexcept {
    if (cache_entries == nullptr) {
        return 0;
    }

    std::uint64_t count = 0;
    dlist_iter iterator;
    dlist_foreach(iterator, cache_entries) {
        ++count;
    }
    return count;
}

}  // namespace

/**
 * @brief Return the cache memory context, creating it on first use.
 * @return Child of `TopMemoryContext` that owns all cache entries.
 */
export [[nodiscard]] MemoryContext get_column_cache_memory_context() {
    if (column_cache_context == nullptr) {
        // This parent owns only cache metadata; buffers have separately charged children.
        column_cache_context = AllocSetContextCreate(
            TopMemoryContext, "Bioseq Decompression Cache", ALLOCSET_DEFAULT_SIZES);
        std::memset(&cache_statistics, 0, sizeof(cache_statistics));
        cache_entries = nullptr;
    }
    return column_cache_context;
}

/**
 * Clear unborrowed buffers now and defer shared-context deletion until every
 * live reader releases its pin. An auxiliary index fetch may end while another
 * reader still holds Datums pointing into these immutable buffers.
 */
export void reset_column_cache() noexcept {
    column_cache_reset_pending = true;
    reclaim_column_cache();
}

/**
 * Protect the current physical chunk for one stripe reader. Its memory context
 * is unique to the reader; a nested scan of the same relation owns a separate
 * lease. Reuse one reset callback per stripe lifetime, not one per chunk visit.
 */
export void mark_chunk_group_in_use(
        MemoryContext owner_context,
        std::uint64_t storage_id,
        std::uint64_t stripe_id,
        std::uint32_t chunk_id) {
    active_chunk_group* active = find_chunk_group_owner(owner_context);
    if (active == nullptr) {
        active = static_cast<active_chunk_group*>(MemoryContextAllocZero(
            owner_context, sizeof(active_chunk_group)));
        active->owner_context = owner_context;
        active->cleanup_callback.func = release_chunk_group_owner;
        active->cleanup_callback.arg = active;
        MemoryContextRegisterResetCallback(owner_context, &active->cleanup_callback);
        dlist_push_tail(&chunk_groups_in_use, &active->list_node);
    }
    active->storage_id = storage_id;
    active->stripe_id = stripe_id;
    active->chunk_id = chunk_id;
    reclaim_column_cache();
}

/**
 * @brief Store a decompressed buffer or reuse the immutable existing entry.
 * @param storage Complete `StringInfo` owned by `storage_context`.
 * @param storage_context Temporary child of the reader context, no longer current.
 *
 * Allocate cache metadata before publishing the completed buffer context. The
 * no-error reparent transfers ownership without copying; failed decompression or
 * metadata allocation leaves the temporary context with its reader. A duplicate
 * key keeps the canonical buffer alive and deletes the redundant input context.
 * If caching is disabled, this function leaves ownership with the reader.
 * @return The buffer to decode; it may be the canonical existing cache entry.
 */
export [[nodiscard]] void* add_column_cache_entry(
        std::uint64_t storage_id,
        std::uint64_t stripe_id,
        std::uint64_t chunk_id,
        std::uint32_t column_id,
        void* storage,
        MemoryContext storage_context) {
    if (!bioseq::settings::column_cache::enabled) {
        return storage;
    }

    bioseq::postgres::support::memory_context_scope cache_scope{
        get_column_cache_memory_context()};
    if (cache_entries == nullptr) {
        cache_entries = bioseq::postgres::support::invoke_pg_as_cpp([] {
            return bioseq::postgres::support::allocate_zeroed_object<dlist_head>();
        });
    }

    column_cache_entry* entry = find_column_cache_entry(
        storage_id, stripe_id, chunk_id, column_id);
    if (entry != nullptr) {
        if (entry->storage_context != storage_context) {
            MemoryContextDelete(storage_context);
        }
        return entry->storage;
    }

    entry = bioseq::postgres::support::invoke_pg_as_cpp([] {
        return bioseq::postgres::support::allocate_zeroed_object<column_cache_entry>();
    });
    entry->storage_id = storage_id;
    entry->stripe_id = stripe_id;
    entry->chunk_id = chunk_id;
    entry->column_id = column_id;
    const std::uint64_t storage_size = static_cast<std::uint64_t>(
        MemoryContextMemAllocated(storage_context, true));
    entry->storage = storage;
    entry->storage_context = storage_context;
    entry->length = storage_size;
    // PostgreSQL guarantees reparenting cannot ERROR. Publish only after every
    // potentially failing allocation and while outside the buffer context.
    MemoryContextSetParent(storage_context, column_cache_context);
    dlist_push_tail(cache_entries, &entry->list_node);
    allocated_bytes += storage_size;
    if (allocated_bytes >= cache_statistics.maximum_cache_size) {
        cache_statistics.maximum_cache_size = allocated_bytes;
    }

    const std::uint64_t capacity = cache_capacity_bytes();
    if (allocated_bytes >= capacity) {
        evict_column_cache_bytes(
            static_cast<std::uint64_t>(capacity * 0.1)
            + (allocated_bytes - capacity));
    }
    ++cache_statistics.writes;
    return storage;
}

/**
 * @brief Borrow a decompressed column buffer from the cache.
 * @return Cache-owned `StringInfo`, or null if absent or caching is disabled.
 *
 * Keep the chunk group marked in use while reading its borrowed values.
 */
export [[nodiscard]] void* retrieve_column_cache_entry(
        std::uint64_t storage_id,
        std::uint64_t stripe_id,
        std::uint64_t chunk_id,
        std::uint32_t column_id) noexcept {
    if (!bioseq::settings::column_cache::enabled) {
        return nullptr;
    }

    column_cache_entry* entry = find_column_cache_entry(
        storage_id, stripe_id, chunk_id, column_id);
    if (entry == nullptr) {
        ++cache_statistics.misses;
        return nullptr;
    }
    ++cache_statistics.hits;
    return entry->storage;
}

/** @return Cache statistics  with the current byte and entry counts filled in. */
export [[nodiscard]] column_cache_statistics* get_column_cache_statistics() noexcept {
    cache_statistics.ending_cache_size = allocated_bytes;
    cache_statistics.entries = count_column_cache_entries();
    return &cache_statistics;
}

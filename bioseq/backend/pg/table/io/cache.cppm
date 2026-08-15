/**
 * @file
 * @brief Backend-local cache of decompressed column chunks.
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
List* chunk_groups_in_use = NIL;
column_cache_statistics cache_statistics{};

/** Intrusive cache entry; `list_node` must remain the first member. */
struct column_cache_entry {
    dlist_node list_node{};
    std::uint64_t relation_id = 0;
    std::uint64_t stripe_id = 0;
    std::uint64_t chunk_id = 0;
    std::uint64_t read_count = 0;
    std::uint64_t length = 0;
    void* storage = nullptr;
    std::uint32_t column_id = 0;
};

/** One relation's chunk group protected from eviction while a scan uses it. */
struct active_chunk_group {
    std::uint64_t relation_id = 0;
    std::uint64_t stripe_id = 0;
    std::uint64_t chunk_id = 0;
};

/** @brief Convert the configured cache capacity from MiB to bytes. */
[[nodiscard]] std::uint64_t cache_capacity_bytes() noexcept {
    return static_cast<std::uint64_t>(
        bioseq::settings::column_cache::capacity_mb) * 1024u * 1024u;
}

/** @brief Report whether a scan currently protects an entry's chunk group. */
[[nodiscard]] bool entry_is_in_use(const column_cache_entry& entry) {
    for (const auto* active : bioseq::postgres::support::postgres_list<active_chunk_group>(chunk_groups_in_use)) {
        if (active->relation_id == entry.relation_id
            && active->stripe_id == entry.stripe_id
            && active->chunk_id == entry.chunk_id) {
            return true;
        }
    }
    return false;
}

/** @brief Release the cache-owned buffer referenced by an entry. */
void release_entry_storage(column_cache_entry& entry) noexcept {
    auto* buffer = static_cast<StringInfo>(entry.storage);
    if (buffer->data != nullptr) {
        pfree(buffer->data);
    }
    pfree(buffer);
}

/** Find a cache entry and increment its frequency counter on a hit. */
[[nodiscard]] column_cache_entry* find_column_cache_entry(
        std::uint64_t relation_id,
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
        if (entry->relation_id == relation_id
            && entry->stripe_id == stripe_id
            && entry->chunk_id == chunk_id
            && entry->column_id == column_id) {
            ++entry->read_count;
            return entry;
        }
    }
    return nullptr;
}

/** Evict least-frequently-read entries until `bytes_to_release` is satisfied. */
void evict_column_cache_bytes(std::uint64_t bytes_to_release) {
    std::uint64_t current_read_count = 0;
    std::uint64_t next_read_count = PG_UINT64_MAX;

    while (bytes_to_release > 0) {
        dlist_mutable_iter iterator;
        dlist_foreach_modify(iterator, cache_entries) {
            auto* entry = dlist_container(
                column_cache_entry, list_node, iterator.cur);
            if (entry->read_count != current_read_count
                && entry->read_count < next_read_count) {
                next_read_count = entry->read_count;
            }
            if (entry->read_count != current_read_count
                || entry_is_in_use(*entry)) {
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

        current_read_count = next_read_count;
        next_read_count = PG_UINT64_MAX;
    }
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
        const std::uint64_t capacity = cache_capacity_bytes();
        column_cache_context = AllocSetContextCreate(
            TopMemoryContext,
            "Bioseq Decompression Cache",
            0,
            static_cast<std::uint64_t>(capacity * 0.1),
            capacity);
        std::memset(&cache_statistics, 0, sizeof(cache_statistics));
        cache_entries = nullptr;
    }
    return column_cache_context;
}

/** Delete all decompressed entries and reset backend-local cache accounting. */
export void reset_column_cache() noexcept {
    if (column_cache_context != nullptr) {
        MemoryContextDelete(column_cache_context);
        column_cache_context = nullptr;
        chunk_groups_in_use = NIL;
    }
    allocated_bytes = 0;
    cache_entries = nullptr;
}

/**
 * @brief Mark the relation's current chunk group as unavailable for eviction.
 */
export void mark_chunk_group_in_use(
        std::uint64_t relation_id,
        std::uint64_t stripe_id,
        std::uint32_t chunk_id) {
    bool found_relation = false;
    bioseq::postgres::support::memory_context_scope cache_scope{
        get_column_cache_memory_context()};

    for (auto* active : bioseq::postgres::support::postgres_list<active_chunk_group>(chunk_groups_in_use)) {
        if (active->relation_id == relation_id) {
            active->stripe_id = stripe_id;
            active->chunk_id = chunk_id;
            found_relation = true;
        }
    }
    if (!found_relation) {
        auto* active = bioseq::postgres::support::allocate_zeroed_object<active_chunk_group>();
        active->relation_id = relation_id;
        active->stripe_id = stripe_id;
        active->chunk_id = chunk_id;
        chunk_groups_in_use = lappend(chunk_groups_in_use, active);
    }
}

/**
 * @brief Add or replace a decompressed chunk buffer.
 * @param storage Owned `StringInfo`; ownership transfers to the cache.
 */
export void add_column_cache_entry(
        std::uint64_t relation_id,
        std::uint64_t stripe_id,
        std::uint64_t chunk_id,
        std::uint32_t column_id,
        void* storage) {
    if (!bioseq::settings::column_cache::enabled) {
        return;
    }

    bioseq::postgres::support::memory_context_scope cache_scope{
        get_column_cache_memory_context()};
    if (cache_entries == nullptr) {
        cache_entries = bioseq::postgres::support::allocate_zeroed_object<dlist_head>();
    }

    column_cache_entry* entry = find_column_cache_entry(
        relation_id, stripe_id, chunk_id, column_id);
    if (entry != nullptr) {
        release_entry_storage(*entry);
        allocated_bytes -= entry->length;
    } else {
        entry = bioseq::postgres::support::allocate_zeroed_object<column_cache_entry>();
        entry->relation_id = relation_id;
        entry->stripe_id = stripe_id;
        entry->chunk_id = chunk_id;
        entry->column_id = column_id;
        dlist_push_tail(cache_entries, &entry->list_node);
    }

    const std::uint64_t storage_size =
        static_cast<StringInfo>(storage)->len;
    entry->storage = storage;
    entry->length = storage_size;
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
}

/**
 * @brief Borrow a decompressed chunk buffer from the cache.
 * @return Live cache-owned `StringInfo`, or null on a miss.
 */
export [[nodiscard]] void* retrieve_column_cache_entry(
        std::uint64_t relation_id,
        std::uint64_t stripe_id,
        std::uint64_t chunk_id,
        std::uint32_t column_id) noexcept {
    if (!bioseq::settings::column_cache::enabled) {
        return nullptr;
    }

    column_cache_entry* entry = find_column_cache_entry(
        relation_id, stripe_id, chunk_id, column_id);
    if (entry == nullptr) {
        ++cache_statistics.misses;
        return nullptr;
    }
    ++cache_statistics.hits;
    return entry->storage;
}

/** @return Mutable backend-local metrics after refreshing size counters. */
export [[nodiscard]] column_cache_statistics* get_column_cache_statistics() noexcept {
    cache_statistics.ending_cache_size = allocated_bytes;
    cache_statistics.entries = count_column_cache_entries();
    return &cache_statistics;
}

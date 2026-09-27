/**
 * @file
 * @brief Cache the  sequence-column positions of an open table.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "access/relation.h"
#include "catalog/pg_attribute.h"
#include "utils/inval.h"
}

#include <cstdint>
#include <new>
#include <vector>

export module bioseq.postgres.table.am_cache;

import bioseq.postgres.support;
import bioseq.postgres.types.compact_sequence;

export namespace bioseq::storage {

/**
 * @brief Sequence columns cached in Relation::rd_amcache.
 *
 * Store one-based PostgreSQL attribute numbers so row operations can find
 * sequence values without scanning the tuple descriptor every time.
 */
struct table_access_cache {
    std::vector<std::int16_t> sequence_attribute_numbers{};
    bool is_initialized = false;
};

/**
 * @brief Get the table's sequence columns, reading its descriptor on first use
 * @param relation Open relation with a vlid tuple descriptor.
 * @return Cache associated with the current relation-cache entry.
 */
[[nodiscard]] const table_access_cache& get_table_access_cache(Relation relation);

/**
 * @brief Leave cache rebuilding to the next table access.
 *
 * PostgreSQL clears rd_amcache during invalidation. Opening the, relation here
 * could re-enter that process, so  this callback does nothing.
 */
void invalidate_table_access_cache(Datum callback_argument, Oid relation_id);

} // namespace bioseq::storage

namespace bioseq::storage {
namespace {

/** Populate sequence attribute numbers from the current tuple descriptor. */
void populate_sequence_attributes(table_access_cache& cache, Relation relation) {
    cache.sequence_attribute_numbers.clear();

    const Oid sequence_type_oid = bioseq_seq_type_oid_missing_ok();
    if (!OidIsValid(sequence_type_oid)) {
        cache.is_initialized = true;
        return;
    }

    TupleDesc tuple_descriptor = RelationGetDescr(relation);
    cache.sequence_attribute_numbers.reserve(static_cast<std::size_t>(tuple_descriptor->natts));
    for (int attribute_index = 0; attribute_index < tuple_descriptor->natts; ++attribute_index) {
        Form_pg_attribute attribute = bioseq::postgres::support::tuple_descriptor_attribute(tuple_descriptor, attribute_index);
        if (!attribute->attisdropped && attribute->atttypid == sequence_type_oid) {
            cache.sequence_attribute_numbers.push_back(
                static_cast<std::int16_t>(attribute_index + 1));
        }
    }
    cache.is_initialized = true;
}

} // namespace

/** Get cached sequence-column positions, filling rd_amcache on first use. */
const table_access_cache& get_table_access_cache(Relation relation) {
    if (relation->rd_amcache != nullptr) {
        auto* cache = static_cast<table_access_cache*>(relation->rd_amcache);
        if (!cache->is_initialized) {
            populate_sequence_attributes(*cache, relation);
        }
        return *cache;
    }

    bioseq::postgres::support::memory_context_scope cache_context{CacheMemoryContext};
    void* storage = palloc0(sizeof(table_access_cache));
    auto* cache = ::new (storage) table_access_cache{};
    populate_sequence_attributes(*cache, relation);
    relation->rd_amcache = cache;
    return *cache;
}

/** Leave the invalidated cache empty until the next table access. */
void invalidate_table_access_cache(Datum /*callback_argument*/, Oid /*relation_id*/) {
    // Reopening the relation during invalidation could re-enter relcache
    // cleanup. get_table_access_cache rebuilds the cache when it is next used.
}

} // namespace bioseq::storage

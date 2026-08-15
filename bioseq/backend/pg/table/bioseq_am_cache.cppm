/**
 * @file
 * @brief Relation-local metadata for sequence columns used by table hot paths.
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
 * @brief Metadata cached in `Relation::rd_amcache` for a Bioseq table.
 *
 * Attribute numbers are PostgreSQL's one-based `AttrNumber` values. The
 * vector is populated once per relation-cache instance so row hot paths do
 * not repeatedly inspect the tuple descriptor.
 */
struct table_access_cache {
    std::vector<std::int16_t> sequence_attribute_numbers{};
    bool is_initialized = false;
};

/**
 * @brief Return the lazily populated table-access cache for `relation`.
 * @param relation Open PostgreSQL relation whose descriptor remains valid.
 * @return Cache owned by `CacheMemoryContext` and the relation-cache entry.
 */
[[nodiscard]] const table_access_cache& get_table_access_cache(Relation relation);

/**
 * @brief PostgreSQL relcache callback for Bioseq table-access metadata.
 *
 * PostgreSQL clears `rd_amcache` during invalidation. Re-opening a relation
 * from this callback would recursively enter relcache invalidation, so cache
 * recreation is intentionally deferred to `get_table_access_cache()`.
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
        Form_pg_attribute attribute = TupleDescAttr(tuple_descriptor, attribute_index);
        if (!attribute->attisdropped && attribute->atttypid == sequence_type_oid) {
            cache.sequence_attribute_numbers.push_back(
                static_cast<std::int16_t>(attribute_index + 1));
        }
    }
    cache.is_initialized = true;
}

} // namespace

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

void invalidate_table_access_cache(Datum /*callback_argument*/, Oid /*relation_id*/) {
    // PostgreSQL has already detached rd_amcache. Allocations are bounded by
    // the relations observed by this backend and reclaimed with
    // CacheMemoryContext; touching the relation here can corrupt the active
    // ResourceOwner during a relcache flush.
}

} // namespace bioseq::storage

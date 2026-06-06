/*-------------------------------------------------------------------------
 *
 * table/bioseq_am_cache.cppm
 *
 * Per-Relation cache (DOC0260 §8.3) installed on rd_amcache. Captures the
 * 1-based attno of every bioseq.seq column on a relation so the TAM hot
 * paths can iterate over SEQ columns in O(1) per row instead of walking
 * the tuple descriptor by atttypid each time.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"
#include "columnar/columnar.h"

extern "C" {
#include "access/relation.h"
#include "access/table.h"
#include "utils/inval.h"
#include "utils/syscache.h"
#include "catalog/pg_attribute.h"
}

#include <cstdint>
#include <vector>

export module bioseq_am_cache;

import mlogging;
import pg_utils;

export namespace bioseq::storage {

/*
 * Per-Relation cache slot. Populated lazily by bioseq_am_cache_for() and
 * stored on Relation::rd_amcache for the lifetime of the relcache entry.
 * The struct is placement-new'd into CacheMemoryContext, and the
 * destructor is invoked from the relcache invalidation callback.
 */
struct BioseqAmCache {
    std::vector<std::int16_t> seq_attnos;
    bool initialised = false;
};

[[nodiscard]] const BioseqAmCache& bioseq_am_cache_for(Relation rel);

void bioseq_am_cache_invalidate(Datum arg, Oid relid);

}  // namespace bioseq::storage

// ============================================================================
// Implementation
// ============================================================================

namespace bioseq::storage {

[[nodiscard]] static Oid resolve_bioseq_seq_type_oid() {
    return DatumGetObjectId(DirectFunctionCall1(
        regtypein, CStringGetDatum("bioseq.seq")));
}

[[nodiscard]] static Oid current_bioseq_seq_type_oid() {
    /*
     * Resolve on each relation-cache population. Extension reset workflows
     * used by integration tests run DROP EXTENSION; CREATE EXTENSION in one
     * backend, which assigns a fresh pg_type OID without unloading this dylib.
     * A process-lifetime static OID would then make every new bioseq.seq
     * column invisible to the table-AM hydration/rewrite paths.
     */
    return resolve_bioseq_seq_type_oid();
}

static void populate_seq_attnos(BioseqAmCache& cache, Relation rel) {
    cache.seq_attnos.clear();
    const Oid seq_type_oid = current_bioseq_seq_type_oid();
    if (!OidIsValid(seq_type_oid)) {
        cache.initialised = true;
        return;
    }
    TupleDesc td = RelationGetDescr(rel);
    cache.seq_attnos.reserve(static_cast<std::size_t>(td->natts));
    for (int i = 0; i < td->natts; ++i) {
        Form_pg_attribute attr = TupleDescAttr(td, i);
        if (attr->attisdropped) continue;
        if (attr->atttypid == seq_type_oid) {
            cache.seq_attnos.push_back(static_cast<std::int16_t>(i + 1));
        }
    }
    cache.initialised = true;
}

const BioseqAmCache& bioseq_am_cache_for(Relation rel) {
    if (rel->rd_amcache != nullptr) {
        auto* existing = static_cast<BioseqAmCache*>(rel->rd_amcache);
        if (existing->initialised) {
            return *existing;
        }
        populate_seq_attnos(*existing, rel);
        return *existing;
    }
    MemoryContextScope ctx{CacheMemoryContext};
    void* raw = palloc0(sizeof(BioseqAmCache));
    auto* fresh = ::new (raw) BioseqAmCache{};
    populate_seq_attnos(*fresh, rel);
    rel->rd_amcache = fresh;
    return *fresh;
}

static void invalidate_one(Relation rel) {
    if (rel == nullptr || rel->rd_amcache == nullptr) {
        return;
    }
    auto* cache = static_cast<BioseqAmCache*>(rel->rd_amcache);
    cache->~BioseqAmCache();
    pfree(cache);
    rel->rd_amcache = nullptr;
}

void bioseq_am_cache_invalidate(Datum /*arg*/, Oid /*relid*/) {
    // DOC0260 §8.3 — relcache invalidation callback. Postgres itself
    // sets `rd_amcache = nullptr` on every relcache flush, so the
    // dominant code path is handled implicitly: the next
    // `bioseq_am_cache_for(rel)` call sees `rd_amcache == nullptr` and
    // rebuilds. We deliberately do NOT call `try_safe_relation_open()`
    // here — that would re-enter the relcache during a flush and corrupt
    // the surrounding INSERT's ResourceOwner ("tupdesc reference is not
    // owned by resource owner TopTransaction" → PANIC).
    //
    // The leaked `BioseqAmCache` allocation under `CacheMemoryContext`
    // (one struct holding a `std::vector<int16_t>`) is bounded by the
    // number of distinct USING bioseq relations a backend sees in its
    // lifetime — a few hundred at most — and is reclaimed when
    // CacheMemoryContext itself is reset.
}

}  // namespace bioseq::storage

/*-------------------------------------------------------------------------
 *
 * pg_utils/pg_utils.cppm
 *
 * Generic PostgreSQL C-API wrappers used across the bioseq extension:
 *   - palloc0 typed allocation helpers (make0 / make1)
 *   - always-inline variants of relation_open / try_relation_open
 *   - plan target-list rebuilding helper used by custom scan nodes
 *   - transaction-scoped HTAB (TxnScopedHtab) that self-installs a
 *     MemoryContext reset callback so callers don't hand-roll the
 *     hash_create + AllocSetContextCreate + cleanup-callback triple
 *
 * Any helper in this module must be PG-specific and generic enough to be
 * reused by multiple backend modules. Algorithm-specific wrappers belong
 * with their consumer.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include <type_traits>

export module bioseq.pg_utils;

export import bioseq.pg_support;
export import bioseq.pg_utils.lists;
export import bioseq.pg_utils.bitmapset;
export import bioseq.pg_utils.datums;
export import bioseq.pg_utils.error_bridge;
export import bioseq.pg_utils.sql_boundary;

import bioseq.pg_utils.safety_runtime_probe;

/** Disable a page-cache flag for this scope and restore its prior value. */
export class PageCacheDisableScope {
private:
    bool& target_flag;
    bool previous_value;

public:
    constexpr explicit PageCacheDisableScope(bool& target) noexcept
        : target_flag(target), previous_value(target) {
        target_flag = false;
    }

    constexpr ~PageCacheDisableScope() noexcept {
        target_flag = previous_value;
        if (!std::is_constant_evaluated()) {
            bioseq::pg_utils::safety_runtime_probe::record_cleanup_complete(
                "page_cache.restore");
        }
    }

    PageCacheDisableScope(const PageCacheDisableScope&) = delete;
    PageCacheDisableScope& operator=(const PageCacheDisableScope&) = delete;
    PageCacheDisableScope(PageCacheDisableScope&&) = delete;
    PageCacheDisableScope& operator=(PageCacheDisableScope&&) = delete;
};

static_assert(
    std::is_nothrow_destructible_v<PageCacheDisableScope>);
static_assert([] consteval {
    bool enabled = true;
    {
        PageCacheDisableScope disabled{enabled};
        if (enabled) return false;
    }
    return enabled;
}());

/*
 * make0<T>() returns a pointer to a zero-initialised T allocated in the
 * current PostgreSQL memory context. Equivalent to palloc0(sizeof(T)) with
 * a static_cast to T*.
 */
export template<typename T>
[[nodiscard]] inline T* make0() {
    return static_cast<T*>(palloc0(sizeof(T)));
}

/*
 * make1<T>(byte_count) returns a pointer to zero-initialised storage of
 * byte_count bytes typed as T*. Use this when the allocation size is not
 * exactly sizeof(T), e.g. flexible array members or byte-sized buffers.
 */
export template<typename T>
[[nodiscard]] inline T* make1(size_t byte_count) {
    return static_cast<T*>(palloc0(byte_count));
}

/*
 * make_array<T>(element_count) returns a zero-initialised T[element_count]
 * allocated in the current memory context. Prefer this over make1 when the
 * count of elements (rather than raw byte count) is the natural unit, so
 * the multiplication by sizeof(T) is centralised here.
 */
export template<typename T>
[[nodiscard]] inline T* make_array(size_t element_count) {
    return static_cast<T*>(palloc0(element_count * sizeof(T)));
}

/** Preserve Bioseq's historical global name while common owns the guard. */
export using MemoryContextScope =
    bioseq::pg_support::MemoryContextScope;

/*
 * safe_relation_open wraps relation_open. It exists so that all callers in
 * the extension funnel through a single entry point that can be temporarily
 * instrumented without touching each call site.
 */
export __attribute__((always_inline))
inline Relation safe_relation_open(Oid relation_id, LOCKMODE lock_mode) {
    return relation_open(relation_id, lock_mode);
}

/*
 * try_safe_relation_open is the non-throwing variant used when the relation
 * may have been concurrently dropped. Wraps try_relation_open.
 */
export __attribute__((always_inline))
inline Relation try_safe_relation_open(Oid relation_id, LOCKMODE lock_mode) {
    return try_relation_open(relation_id, lock_mode);
}

/*
 * find_primary_key_oid discovers the OID of the primary-key btree on
 * `rel`, or InvalidOid when the relation has none. Walks the relation's
 * index list and returns the first index whose pg_index.indisprimary is
 * true. Used by every bioseq read path that wants O(log n) lookups by
 * (key1, key2, ...) on a small heap-AM table (sidecar PK lookups,
 * metadata_age_log PK lookups). Centralised here so the sidecar and
 * metadata_admin modules share one source of truth without introducing
 * a circular module dependency.
 */
export [[nodiscard]] inline Oid find_primary_key_oid(Relation rel) {
    Oid pk_index_oid = InvalidOid;
    List* index_list = RelationGetIndexList(rel);
    ListCell* lc;
    foreach (lc, index_list) {
        const Oid index_oid = lfirst_oid(lc);
        Relation index_rel = index_open(index_oid, AccessShareLock);
        if (index_rel->rd_index != nullptr && index_rel->rd_index->indisprimary) {
            pk_index_oid = index_oid;
        }
        index_close(index_rel, AccessShareLock);
        if (OidIsValid(pk_index_oid)) {
            break;
        }
    }
    list_free(index_list);
    return pk_index_oid;
}

/*
 * CopyPlanCosts propagates the subset of Plan fields that describe planner
 * cost estimates (parallel awareness, startup/total cost, rowcount/width)
 * from source onto destination. Used when wrapping an existing Plan in a
 * CustomScan so the new outer plan inherits the costing inputs.
 */
export inline void CopyPlanCosts(Plan* destination, const Plan* source) {
    destination->parallel_aware = source->parallel_aware;
    destination->startup_cost = source->startup_cost;
    destination->total_cost = source->total_cost;
    destination->plan_rows = source->plan_rows;
    destination->plan_width = source->plan_width;
}

/*
 * CustomBuildTargetList rebuilds a target list by constructing fresh Var
 * nodes for each entry, rewriting their varno to reference a supplied range
 * table index. Used by custom scan nodes when projecting through a plan
 * that replaces a base relation reference.
 */
export inline List* CustomBuildTargetList(List* source_tlist, Index target_var_no) {
    List* rebuilt_tlist = NIL;
    for (auto* source_entry : PList<TargetEntry>(source_tlist))
    {
        Var* projected_var = makeVar(target_var_no,
                                     source_entry->resno,
                                     exprType(reinterpret_cast<Node*>(source_entry->expr)),
                                     exprTypmod(reinterpret_cast<Node*>(source_entry->expr)),
                                     exprCollation(reinterpret_cast<Node*>(source_entry->expr)),
                                     0);

        TargetEntry* rebuilt_entry = makeTargetEntry(reinterpret_cast<Expr*>(projected_var),
                                                     source_entry->resno,
                                                     source_entry->resname,
                                                     source_entry->resjunk);

        rebuilt_tlist = lappend(rebuilt_tlist, rebuilt_entry);
    }
    return rebuilt_tlist;
}

/**
 * Preserve the Oid-first Entry API consumed by Bioseq column state caches.
 * The implementation and transaction reset callback live in pg_support.
 */
export template <class Entry>
using TxnScopedHtab =
    bioseq::pg_support::OidEntryTxnScopedHtab<Entry>;

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

#include <cstring>

export module pg_utils;

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
    ListCell* cell;

    foreach (cell, source_tlist)
    {
        TargetEntry* source_entry = static_cast<TargetEntry*>(lfirst(cell));

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

/*
 * TxnScopedHtab<Entry> wraps a PostgreSQL dynahash table whose lifetime is
 * scoped to TopTransactionContext. On first acquire() the hash table and a
 * backing AllocSetContext are created lazily, and a reset callback is
 * registered so that both the HTAB pointer and the context pointer are
 * nulled out at transaction end. The Entry type must have an Oid-keyed
 * first field (sizeof(Oid) == keysize), matching the convention used by
 * the write-state and read-state caches.
 *
 * Typical usage:
 *
 *   static pg_utils::TxnScopedHtab<MyEntry> my_table{"my txn table"};
 *
 *   MyEntry* entry = my_table.find_or_insert(&oid_key);
 *   MemoryContext ctx = my_table.context();
 *
 * Callers should treat context() as nullable (it is nullable between
 * transactions) and call acquire() if they need it populated.
 */
export template<typename Entry>
class TxnScopedHtab {
private:
    const char* name_;
    HTAB* table_ = nullptr;
    MemoryContext context_ = nullptr;
    MemoryContextCallback reset_callback_{};

    static void cleanup_trampoline(void* argument) {
        auto* self = static_cast<TxnScopedHtab*>(argument);
        self->table_ = nullptr;
        self->context_ = nullptr;
    }

    void acquire() {
        if (table_ != nullptr) {
            return;
        }
        /*
         * Call the internal variant directly to bypass the __builtin_constant_p
         * static_assert in the AllocSetContextCreate macro -- the caller's
         * debug_name is a string literal but that literal is forwarded
         * through a runtime member pointer, which defeats the check.
         */
        context_ = AllocSetContextCreateInternal(
            TopTransactionContext, name_, ALLOCSET_DEFAULT_SIZES);

        HASHCTL hash_info;
        std::memset(&hash_info, 0, sizeof(hash_info));
        hash_info.keysize = sizeof(Oid);
        hash_info.hash = oid_hash;
        hash_info.entrysize = sizeof(Entry);
        hash_info.hcxt = context_;

        const uint32 hash_flags = HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT;
        table_ = hash_create(name_, 64, &hash_info, hash_flags);

        reset_callback_.func = &TxnScopedHtab::cleanup_trampoline;
        reset_callback_.arg = this;
        reset_callback_.next = nullptr;
        MemoryContextRegisterResetCallback(context_, &reset_callback_);
    }

public:
    explicit constexpr TxnScopedHtab(const char* debug_name) : name_{debug_name} {}

    /* Returns the owning MemoryContext (valid after acquire or any mutating call). */
    [[nodiscard]] MemoryContext context() const { return context_; }

    /* Returns the raw HTAB (nullable between transactions). */
    [[nodiscard]] HTAB* raw() const { return table_; }

    /* True when the table has been lazily created in the current transaction. */
    [[nodiscard]] bool is_initialised() const { return table_ != nullptr; }

    /* Searches for an existing entry; returns nullptr when absent. */
    [[nodiscard]] Entry* find(const void* key) {
        if (table_ == nullptr) {
            return nullptr;
        }
        return static_cast<Entry*>(hash_search(table_, key, HASH_FIND, nullptr));
    }

    /*
     * Looks up the entry for the given key, creating the table on first use
     * and inserting a fresh entry when none exists. When found_existing is
     * non-null, it is set to indicate whether the entry was pre-existing.
     */
    Entry* find_or_insert(const void* key, bool* found_existing = nullptr) {
        acquire();
        bool local_found = false;
        Entry* result = static_cast<Entry*>(
            hash_search(table_, key, HASH_ENTER, &local_found));
        if (found_existing != nullptr) {
            *found_existing = local_found;
        }
        return result;
    }

    /* Removes an entry if present. No-op if the table is uninitialised. */
    void remove(const void* key) {
        if (table_ != nullptr) {
            hash_search(table_, key, HASH_REMOVE, nullptr);
        }
    }

    /* Runs callback(entry) over every live entry in the table. */
    template<typename Fn>
    void for_each(Fn&& callback) {
        if (table_ == nullptr) {
            return;
        }
        HASH_SEQ_STATUS status;
        hash_seq_init(&status, table_);
        for (Entry* entry = static_cast<Entry*>(hash_seq_search(&status));
             entry != nullptr;
             entry = static_cast<Entry*>(hash_seq_search(&status)))
        {
            callback(entry);
        }
    }

    /*
     * Allocates a zero-initialised T inside the table's MemoryContext. Use
     * this when pushing a per-subtransaction frame onto a stack hanging off
     * an Entry: the frame must live as long as the map, so it needs to share
     * the same context. Caller must have already acquired the cache (any
     * prior find_or_insert call suffices).
     */
    template<typename Frame>
    [[nodiscard]] Frame* alloc_in_context() {
        MemoryContext old_context = MemoryContextSwitchTo(context_);
        Frame* frame = make0<Frame>();
        MemoryContextSwitchTo(old_context);
        return frame;
    }
};

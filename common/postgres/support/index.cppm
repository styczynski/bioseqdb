/**
 * @file common/postgres/support/index.cppm
 * @brief PostgreSQL memory-context and transaction-scoped hash-table owners.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/palloc.h"
#include "utils/rel.h"
#include "utils/relcache.h"

#include "common/hashfn.h"
}

#include <cstddef>
#include <type_traits>
#include <utility>

export module bioseq.postgres.support;

export import :safety_runtime_probe;
export import :error_boundary;
export import :lists;
export import :bitmapset;
export import :nodes;
export import :spi;

export namespace bioseq::postgres::support {

/** Return an immediate primary key, preserving the PG17 catalog contract. */
[[nodiscard]] inline Oid relation_immediate_primary_key_index(Relation relation) {
#if PG_VERSION_NUM >= 180000
    return RelationGetPrimaryKeyIndex(relation, false);
#else
    return RelationGetPrimaryKeyIndex(relation);
#endif
}

/** Restores the calling backend's PostgreSQL memory context at scope exit. */
class memory_context_scope final {
  public:
    /** Switches to @p target until this owner is destroyed. */
    explicit memory_context_scope(MemoryContext target)
        : previous_(MemoryContextSwitchTo(target)) {}

    /** Restores the context that was active at construction. */
    ~memory_context_scope() noexcept {
        MemoryContextSwitchTo(previous_);
    }

    memory_context_scope(const memory_context_scope&) = delete;
    memory_context_scope& operator=(const memory_context_scope&) = delete;
    memory_context_scope(memory_context_scope&&) = delete;
    memory_context_scope& operator=(memory_context_scope&&) = delete;

  private:
    MemoryContext previous_ = nullptr;
};

/** Result of inserting or finding an entry in a transaction hash table. */
template <class p_entry> struct transaction_hash_insert_result {
    /** Address of the stored PostgreSQL hash-table entry. */
    p_entry* entry = nullptr;

    /** Whether the entry existed before this operation. */
    bool was_present = false;
};

/**
 * Lazily owns an OID-keyed PostgreSQL hash table for the current transaction.
 *
 * @tparam p_entry Standard-layout entry whose first bytes contain its OID key.
 * The table and its memory context are invalidated by PostgreSQL's transaction
 * context reset callback. The wrapper itself owns no separately allocated C++
 * object and may therefore live for the backend process lifetime.
 */
template <class p_entry> class transaction_scoped_hash_table final {
    static_assert(std::is_standard_layout_v<p_entry>);
    static_assert(std::is_trivially_destructible_v<p_entry>);
    static_assert(sizeof(p_entry) >= sizeof(Oid));

  public:
    /** Binds the lazy table to a process-lifetime PostgreSQL diagnostic name. */
    explicit constexpr transaction_scoped_hash_table(const char* debug_name) noexcept
        : name_(debug_name) {}

    transaction_scoped_hash_table(const transaction_scoped_hash_table&) = delete;
    transaction_scoped_hash_table& operator=(const transaction_scoped_hash_table&) = delete;
    transaction_scoped_hash_table(transaction_scoped_hash_table&&) = delete;
    transaction_scoped_hash_table& operator=(transaction_scoped_hash_table&&) = delete;

    /** Returns whether the current transaction has materialized the table. */
    [[nodiscard]] bool is_initialized() const noexcept {
        return table_ != nullptr;
    }

    /** Returns the owning PostgreSQL memory context, or null before insertion. */
    [[nodiscard]] MemoryContext get_memory_context() const noexcept {
        return context_;
    }

    /** Returns the raw PostgreSQL table for narrow C-API adapters. */
    [[nodiscard]] HTAB* get_table() const noexcept {
        return table_;
    }

    /** Finds an existing entry without allocating, or returns null. */
    [[nodiscard]] p_entry* find(const void* key) const {
        if (table_ == nullptr) {
            return nullptr;
        }
        return static_cast<p_entry*>(hash_search(table_, key, HASH_FIND, nullptr));
    }

    /**
     * Finds or inserts one entry and reports whether it was already present.
     *
     * @param key Address of an OID key accepted by PostgreSQL dynahash.
     * @return The stored entry and its pre-operation presence state.
     */
    [[nodiscard]] transaction_hash_insert_result<p_entry> find_or_insert(const void* key) {
        return invoke_pg_as_cpp([this, key] {
            ensure_initialized();
            bool was_present = false;
            p_entry* entry =
                static_cast<p_entry*>(hash_search(table_, key, HASH_ENTER, &was_present));
            return transaction_hash_insert_result<p_entry>{
                .entry = entry,
                .was_present = was_present,
            };
        });
    }

    /** Removes an entry when the current transaction contains @p key. */
    void remove(const void* key) {
        if (table_ != nullptr) {
            static_cast<void>(hash_search(table_, key, HASH_REMOVE, nullptr));
        }
    }

    /** Visits each live entry without allocating or changing table ownership. */
    template <class p_callback> void for_each(p_callback&& callback) {
        if (table_ == nullptr) {
            return;
        }
        HASH_SEQ_STATUS cursor{};
        hash_seq_init(&cursor, table_);
        for (p_entry* entry = static_cast<p_entry*>(hash_seq_search(&cursor)); entry != nullptr;
             entry = static_cast<p_entry*>(hash_seq_search(&cursor))) {
            std::forward<p_callback>(callback)(entry);
        }
    }

    /** Allocates one zeroed trivial frame in the table's transaction context. */
    template <class p_frame> [[nodiscard]] p_frame* allocate_in_context() {
        static_assert(std::is_trivially_destructible_v<p_frame>);
        return invoke_pg_as_cpp([this] {
            Assert(context_ != nullptr);
            return static_cast<p_frame*>(MemoryContextAllocZero(context_, sizeof(p_frame)));
        });
    }

  private:
    /** Clears non-owning pointers after PostgreSQL resets the owner context. */
    static void reset_callback(void* argument) noexcept {
        auto* self = static_cast<transaction_scoped_hash_table*>(argument);
        self->table_ = nullptr;
        self->context_ = nullptr;
    }

    /** Materializes the context and hash table for the current transaction. */
    void ensure_initialized() {
        if (table_ != nullptr) {
            return;
        }
        context_ =
            AllocSetContextCreateInternal(TopTransactionContext, name_, ALLOCSET_DEFAULT_SIZES);

        HASHCTL controls{};
        controls.keysize = sizeof(Oid);
        controls.hash = oid_hash;
        controls.entrysize = sizeof(p_entry);
        controls.hcxt = context_;
        table_ = hash_create(name_, 64, &controls, HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);

        callback_.func = &transaction_scoped_hash_table::reset_callback;
        callback_.arg = this;
        callback_.next = nullptr;
        MemoryContextRegisterResetCallback(context_, &callback_);
    }

    const char* name_ = nullptr;
    HTAB* table_ = nullptr;
    MemoryContext context_ = nullptr;
    MemoryContextCallback callback_{};
};

static_assert(std::is_nothrow_destructible_v<memory_context_scope>);

/**
 * Temporarily disables a page-cache flag and restores its prior value.
 *
 * The referenced flag must outlive this scope. Destruction records the
 * completed restoration when runtime safety instrumentation is active.
 */
class page_cache_disable_scope final {
  public:
    /** Disables @p target_flag until this scope is destroyed. */
    constexpr explicit page_cache_disable_scope(bool& target_flag) noexcept
        : target_flag_(target_flag), previous_value_(target_flag) {
        target_flag_ = false;
    }

    /** Restores the flag value observed at construction. */
    constexpr ~page_cache_disable_scope() noexcept {
        target_flag_ = previous_value_;
        if (!std::is_constant_evaluated()) {
            safety_runtime_probe::record_cleanup_complete("page_cache.restore");
        }
    }

    page_cache_disable_scope(const page_cache_disable_scope&) = delete;
    page_cache_disable_scope& operator=(const page_cache_disable_scope&) = delete;
    page_cache_disable_scope(page_cache_disable_scope&&) = delete;
    page_cache_disable_scope& operator=(page_cache_disable_scope&&) = delete;

  private:
    bool& target_flag_;
    bool previous_value_;
};

static_assert(std::is_nothrow_destructible_v<page_cache_disable_scope>);
static_assert([] consteval {
    bool is_enabled = true;
    {
        page_cache_disable_scope disabled_scope{is_enabled};
        if (is_enabled) {
            return false;
        }
    }
    return is_enabled;
}());

/**
 * Allocates one zero-initialized object in the current PostgreSQL context.
 *
 * @tparam p_value_type Object type represented by the returned storage.
 */
template <class p_value_type>
[[nodiscard, gnu::always_inline]] inline p_value_type* allocate_zeroed_object() {
    return static_cast<p_value_type*>(palloc0(sizeof(p_value_type)));
}

/**
 * Allocates @p byte_count zero-initialized bytes in the current PostgreSQL
 * context and returns typed storage.
 *
 * @tparam p_value_type Object type represented by the returned storage.
 * @param byte_count Number of bytes to allocate.
 */
template <class p_value_type>
[[nodiscard, gnu::always_inline]] inline p_value_type*
allocate_zeroed_storage(std::size_t byte_count) {
    return static_cast<p_value_type*>(palloc0(byte_count));
}

/**
 * Allocates a zero-initialized array in the current PostgreSQL context.
 *
 * @tparam p_element_type Array element type.
 * @param element_count Number of elements to allocate.
 */
template <class p_element_type>
[[nodiscard, gnu::always_inline]] inline p_element_type*
allocate_zeroed_array(std::size_t element_count) {
    return static_cast<p_element_type*>(palloc0(element_count * sizeof(p_element_type)));
}

/** Opens a PostgreSQL relation and acquires @p lock_mode. */
[[nodiscard, gnu::always_inline]] inline Relation open_postgres_relation(Oid relation_oid,
                                                                         LOCKMODE lock_mode) {
    return relation_open(relation_oid, lock_mode);
}

/** Opens a PostgreSQL relation when it still exists, otherwise returns null. */
[[nodiscard, gnu::always_inline]] inline Relation try_open_postgres_relation(Oid relation_oid,
                                                                             LOCKMODE lock_mode) {
    return try_relation_open(relation_oid, lock_mode);
}

/**
 * Finds the primary-key index of @p relation.
 *
 * @return The first primary-key index OID, or InvalidOid when none exists.
 */
[[nodiscard]] inline Oid find_primary_key_oid(Relation relation) {
    Oid primary_key_index_oid = InvalidOid;
    List* index_oids = RelationGetIndexList(relation);
    ListCell* index_cell = nullptr;
    foreach (index_cell, index_oids) {
        const Oid index_oid = lfirst_oid(index_cell);
        Relation index_relation = index_open(index_oid, AccessShareLock);
        if (index_relation->rd_index != nullptr && index_relation->rd_index->indisprimary) {
            primary_key_index_oid = index_oid;
        }
        index_close(index_relation, AccessShareLock);
        if (OidIsValid(primary_key_index_oid)) {
            break;
        }
    }
    list_free(index_oids);
    return primary_key_index_oid;
}

/** Copies PostgreSQL planner cost fields from @p source to @p destination. */
inline void copy_plan_costs(Plan* destination, const Plan* source) noexcept {
    destination->parallel_aware = source->parallel_aware;
    destination->startup_cost = source->startup_cost;
    destination->total_cost = source->total_cost;
    destination->plan_rows = source->plan_rows;
    destination->plan_width = source->plan_width;
}

/**
 * Rebuilds a target list with fresh Var nodes referencing one range table.
 *
 * @param source_target_list Source TargetEntry list.
 * @param target_variable_number Range-table index assigned to every Var.
 * @return Newly allocated PostgreSQL target list.
 */
[[nodiscard]] inline List* build_custom_target_list(List* source_target_list,
                                                    Index target_variable_number) {
    List* rebuilt_target_list = NIL;
    for (TargetEntry* source_entry : postgres_list<TargetEntry>{source_target_list}) {
        Var* projected_variable =
            makeVar(target_variable_number,
                    source_entry->resno,
                    exprType(reinterpret_cast<Node*>(source_entry->expr)),
                    exprTypmod(reinterpret_cast<Node*>(source_entry->expr)),
                    exprCollation(reinterpret_cast<Node*>(source_entry->expr)),
                    0);

        TargetEntry* rebuilt_entry = makeTargetEntry(reinterpret_cast<Expr*>(projected_variable),
                                                     source_entry->resno,
                                                     source_entry->resname,
                                                     source_entry->resjunk);
        rebuilt_target_list = lappend(rebuilt_target_list, rebuilt_entry);
    }
    return rebuilt_target_list;
}

} // namespace bioseq::postgres::support

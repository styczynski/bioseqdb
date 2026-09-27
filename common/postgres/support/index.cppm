/**
 * @file common/postgres/support/index.cppm
 * @brief Helpers for PostgreSQL plans, memory contexts and transaction hash tables.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/tupdesc.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "optimizer/cost.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/palloc.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "varatt.h"

#include "common/hashfn.h"
}

#include <cstddef>
#include <cstdint>
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

/**
 * Read a tuple attribute without copying it.
 * Keep the TupleDescAttr call in one module because Clang can emit conflicting
 * copies of PostgreSQL 18's inline accessor when several modules use it.
 * PostgreSQL's bounds assertions still apply to the zero-based attribute index.
 */
[[nodiscard]] [[gnu::always_inline]] inline auto
tuple_descriptor_attribute(TupleDesc descriptor, const int index) noexcept {
    return TupleDescAttr(descriptor, index);
}

/**
 * Read the size of a varlena with a four-byte header, without detoasting it.
 * Keep the VARSIZE call in one module to avoid conflicting copies of the
 * PostgreSQL 19 inline function. Short or external values need VARSIZE_ANY.
 */
[[nodiscard]] [[gnu::always_inline]] inline auto
varlena_4byte_size(const void* value) noexcept {
    return VARSIZE(value);
}

/**
 * Get PostgreSQL's preliminary cost estimate for a serial nested loop.
 * In PostgreSQL 19, unwrap inner projections to check whether the loop uses
 * Material or Memoize and pass that choice to the cost function. Keep the
 * caller's extra.pgs_mask for PostgreSQL to check. The final path constructor
 * uses the returned workspace.
 */
[[nodiscard]] inline JoinCostWorkspace initial_cost_serial_nested_loop(PlannerInfo* root,
                                                                      JoinType jointype,
                                                                      Path* outer_path,
                                                                      Path* inner_path,
                                                                      JoinPathExtraData* extra) {
    JoinCostWorkspace workspace{};
#if PG_VERSION_NUM >= 190000
    const Path* strategy_inner = inner_path;
    while (strategy_inner != nullptr && IsA(strategy_inner, ProjectionPath))
        strategy_inner = reinterpret_cast<const ProjectionPath*>(strategy_inner)->subpath;
    const std::uint64_t strategy =
        strategy_inner != nullptr && IsA(strategy_inner, MemoizePath) ? PGS_NESTLOOP_MEMOIZE
        : strategy_inner != nullptr && IsA(strategy_inner, MaterialPath) ? PGS_NESTLOOP_MATERIALIZE
                                                                       : PGS_NESTLOOP_PLAIN;
#endif
    initial_cost_nestloop(root,
                         &workspace,
                         jointype,
#if PG_VERSION_NUM >= 190000
                         strategy | PGS_CONSIDER_NONPARTIAL,
#endif
                         outer_path,
                         inner_path,
                         extra);
    return workspace;
}

/** Return the index for an immediate, non-deferrable primary key. */
[[nodiscard]] inline Oid relation_immediate_primary_key_index(Relation relation) {
#if PG_VERSION_NUM >= 180000
    return RelationGetPrimaryKeyIndex(relation, false);
#else
    return RelationGetPrimaryKeyIndex(relation);
#endif
}

/** Switch PostgreSQL memory contexts for a C++ scope and restore on destruction. */
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
 * Create an OID-keyed hash table on first insertion in a transaction.
 * PostgreSQL frees the table with its transaction memory context. A reset
 * callback clears this wrapper's pointers so it can be used in a later
 * transaction. Keep the wrapper alive until that callback has run.
 *
 * @tparam p_entry Standard-layout, trivially destructible entry with its OID
 * key in the first bytes.
 */
template <class p_entry> class transaction_scoped_hash_table final {
    static_assert(std::is_standard_layout_v<p_entry>);
    static_assert(std::is_trivially_destructible_v<p_entry>);
    static_assert(sizeof(p_entry) >= sizeof(Oid));

  public:
    /** Save the table name, which must stay valid for this wrapper's lifetime. */
    explicit constexpr transaction_scoped_hash_table(const char* debug_name) noexcept
        : name_(debug_name) {}

    transaction_scoped_hash_table(const transaction_scoped_hash_table&) = delete;
    transaction_scoped_hash_table& operator=(const transaction_scoped_hash_table&) = delete;
    transaction_scoped_hash_table(transaction_scoped_hash_table&&) = delete;
    transaction_scoped_hash_table& operator=(transaction_scoped_hash_table&&) = delete;

    /** Return whether this transaction has created the table. */
    [[nodiscard]] bool is_initialized() const noexcept {
        return table_ != nullptr;
    }

    /** Returns the owning PostgreSQL memory context, or null before insertion. */
    [[nodiscard]] MemoryContext get_memory_context() const noexcept {
        return context_;
    }

    /** Return the PostgreSQL hash-table pointer, or null before initialization. */
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
     * Find or insert an entry, creating the table if needed.
     *
     * @param key Address of the OID key.
     * @return Entry pointer and a flag saying whether the entry already existed.
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

    /** Call the visitor for every entry in the current table. */
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

    /** Allocate zeroed storage for a trivial frame in the table's memory context. */
    template <class p_frame> [[nodiscard]] p_frame* allocate_in_context() {
        static_assert(std::is_trivially_destructible_v<p_frame>);
        return invoke_pg_as_cpp([this] {
            Assert(context_ != nullptr);
            return static_cast<p_frame*>(MemoryContextAllocZero(context_, sizeof(p_frame)));
        });
    }

  private:
    /** Clear the saved pointers when PostgreSQL resets the table's memory context. */
    static void reset_callback(void* argument) noexcept {
        auto* self = static_cast<transaction_scoped_hash_table*>(argument);
        self->table_ = nullptr;
        self->context_ = nullptr;
    }

    /** Create this transaction's memory context and OID-keyed hash table. */
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
 * Disable a page-cache flag for a C++ scope, then restore its saved value.
 * The flag must outlive this object. When runtime safety tracing is enabled,
 * destruction records the restoration.
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
 * Allocate zeroed storage for one value in the current PostgreSQL memory
 * context. This does not call a C++ constructor.
 *
 * @tparam p_value_type Type whose size determines the allocation.
 */
template <class p_value_type>
[[nodiscard, gnu::always_inline]] inline p_value_type* allocate_zeroed_object() {
    return static_cast<p_value_type*>(palloc0(sizeof(p_value_type)));
}

/**
 * Allocate zeroed bytes in the current PostgreSQL memory context and return
 * a typed pointer.
 *
 * @tparam p_value_type Pointer target type.
 * @param byte_count Number of bytes to allocate.
 */
template <class p_value_type>
[[nodiscard, gnu::always_inline]] inline p_value_type*
allocate_zeroed_storage(std::size_t byte_count) {
    return static_cast<p_value_type*>(palloc0(byte_count));
}

/**
 * Allocate zeroed storage for an array in the current PostgreSQL memory
 * context. This does not call element constructors.
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

/** Copy cost, row-count and width estimates and the parallel-aware flag. */
inline void copy_plan_costs(Plan* destination, const Plan* source) noexcept {
    destination->parallel_aware = source->parallel_aware;
    destination->startup_cost = source->startup_cost;
    destination->total_cost = source->total_cost;
    destination->plan_rows = source->plan_rows;
    destination->plan_width = source->plan_width;
}

/**
 * Build a target list whose columns refer to one input relation.
 * Each new Var keeps its source column number and SQL type. The new entries
 * keep the column names and junk flags.
 *
 * @param source_target_list Columns to reference.
 * @param target_variable_number Range-table index used by the new Vars.
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

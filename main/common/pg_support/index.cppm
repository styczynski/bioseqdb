/**
 * @file common/pg_support/index.cppm
 * @brief Small PostgreSQL memory-context and transaction-hash utilities.
 */
module;

extern "C" {
#include "postgres.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
}

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

export module bioseq.pg_support;

export namespace bioseq::pg_support {

/** Restore the calling backend's MemoryContext at normal C++ scope exit. */
class MemoryContextScope {
public:
    explicit MemoryContextScope(MemoryContext target)
        : previous_(MemoryContextSwitchTo(target)) {}

    ~MemoryContextScope() noexcept {
        MemoryContextSwitchTo(previous_);
    }

    MemoryContextScope(const MemoryContextScope&) = delete;
    MemoryContextScope& operator=(const MemoryContextScope&) = delete;
    MemoryContextScope(MemoryContextScope&&) = delete;
    MemoryContextScope& operator=(MemoryContextScope&&) = delete;

private:
    MemoryContext previous_ = nullptr;
};

/** Allocate a zeroed fixed object directly in a selected PG context. */
template <class Value>
[[nodiscard]] Value* allocate_zeroed(MemoryContext context) {
    static_assert(std::is_trivially_destructible_v<Value>);
    return static_cast<Value*>(MemoryContextAllocZero(context, sizeof(Value)));
}

/** Default bytewise policy for fully initialized fixed-width keys. */
template <class Key>
struct BytewiseKeyPolicy {
    /** Hash all canonical key bytes with a stable 32-bit FNV-1a fold. */
    [[nodiscard]] static std::uint32_t hash(const Key& key) noexcept {
        constexpr std::uint32_t offset_basis = 2166136261u;
        constexpr std::uint32_t prime = 16777619u;
        const auto* bytes = reinterpret_cast<const unsigned char*>(&key);
        std::uint32_t value = offset_basis;
        for (std::size_t index = 0; index < sizeof(Key); ++index) {
            value ^= static_cast<std::uint32_t>(bytes[index]);
            value *= prime;
        }
        return value;
    }

    /** Compare every canonical key byte. */
    [[nodiscard]] static bool equal(
        const Key& left,
        const Key& right) noexcept {
        return std::memcmp(&left, &right, sizeof(Key)) == 0;
    }
};

/** Empty mapped value for transaction-scoped key sets. */
struct EmptyTxnValue {
    std::uint8_t reserved = 0;
};

/**
 * Lazily allocated PostgreSQL dynahash whose owner context is reset with the
 * top-level transaction. Key and Value are fixed, trivially copyable records;
 * KeyPolicy supplies allocation-free hash and equality operations.
 */
template <
    class Key,
    class Value = EmptyTxnValue,
    class KeyPolicy = BytewiseKeyPolicy<Key>>
class TxnScopedHtab {
    static_assert(std::is_trivially_copyable_v<Key>);
    static_assert(std::is_trivially_copyable_v<Value>);

    struct Entry {
        Key key{};
        Value value{};
    };

    static_assert(std::is_standard_layout_v<Entry>);
    static_assert(offsetof(Entry, key) == 0);

public:
    /** Bind this lazy table to a process-lifetime diagnostic name. */
    explicit constexpr TxnScopedHtab(const char* debug_name) noexcept
        : name_(debug_name) {}

    TxnScopedHtab(const TxnScopedHtab&) = delete;
    TxnScopedHtab& operator=(const TxnScopedHtab&) = delete;
    TxnScopedHtab(TxnScopedHtab&&) = delete;
    TxnScopedHtab& operator=(TxnScopedHtab&&) = delete;

    /** Return whether this transaction has materialized the table. */
    [[nodiscard]] bool initialized() const noexcept {
        return table_ != nullptr;
    }

    /** Return the table's owning context, or null before first insertion. */
    [[nodiscard]] MemoryContext context() const noexcept {
        return context_;
    }

    /** Return the raw PG table for narrow adapter operations. */
    [[nodiscard]] HTAB* raw() const noexcept {
        return table_;
    }

    /** Find an existing value without allocating. */
    [[nodiscard]] Value* find(const Key& key) noexcept {
        if (table_ == nullptr) {
            return nullptr;
        }
        Entry* entry = static_cast<Entry*>(
            hash_search(table_, &key, HASH_FIND, nullptr));
        return entry == nullptr ? nullptr : &entry->value;
    }

    /** Find or insert one key, reporting whether it already existed. */
    [[nodiscard]] Value* find_or_insert(
        const Key& key,
        bool* found_existing = nullptr) {
        acquire();
        bool found = false;
        Entry* entry = static_cast<Entry*>(
            hash_search(table_, &key, HASH_ENTER, &found));
        if (!found) {
            std::memset(&entry->value, 0, sizeof(Value));
        }
        if (found_existing != nullptr) {
            *found_existing = found;
        }
        return &entry->value;
    }

    /** Remove a key if present. */
    void remove(const Key& key) noexcept {
        if (table_ != nullptr) {
            static_cast<void>(hash_search(table_, &key, HASH_REMOVE, nullptr));
        }
    }

    /** Visit each live key/value pair without allocating. */
    template <class Callback>
    void for_each(Callback&& callback) {
        if (table_ == nullptr) {
            return;
        }
        HASH_SEQ_STATUS cursor{};
        hash_seq_init(&cursor, table_);
        for (Entry* entry = static_cast<Entry*>(hash_seq_search(&cursor));
             entry != nullptr;
             entry = static_cast<Entry*>(hash_seq_search(&cursor))) {
            std::forward<Callback>(callback)(entry->key, entry->value);
        }
    }

    /** Allocate one zeroed POD frame in this transaction's table context. */
    template <class Frame>
    [[nodiscard]] Frame* allocate_frame() {
        acquire();
        return allocate_zeroed<Frame>(context_);
    }

private:
    /** Adapt the typed key policy to PostgreSQL's dynahash callback. */
    [[nodiscard]] static std::uint32_t hash_key(
        const void* bytes,
        Size) noexcept {
        return KeyPolicy::hash(*static_cast<const Key*>(bytes));
    }

    /** Adapt the typed equality policy to PostgreSQL's dynahash callback. */
    [[nodiscard]] static int compare_keys(
        const void* left,
        const void* right,
        Size) noexcept {
        return KeyPolicy::equal(
            *static_cast<const Key*>(left),
            *static_cast<const Key*>(right))
            ? 0
            : 1;
    }

    /** Clear non-owning pointers when PostgreSQL resets the owner context. */
    static void reset_callback(void* argument) noexcept {
        auto* self = static_cast<TxnScopedHtab*>(argument);
        self->table_ = nullptr;
        self->context_ = nullptr;
    }

    /** Materialize the context and dynahash for the current top transaction. */
    void acquire() {
        if (table_ != nullptr) {
            return;
        }
        context_ = AllocSetContextCreateInternal(
            TopTransactionContext, name_, ALLOCSET_DEFAULT_SIZES);

        HASHCTL controls{};
        controls.keysize = sizeof(Key);
        controls.entrysize = sizeof(Entry);
        controls.hash = &TxnScopedHtab::hash_key;
        controls.match = &TxnScopedHtab::compare_keys;
        controls.hcxt = context_;
        table_ = hash_create(
            name_, 64, &controls,
            HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);

        callback_.func = &TxnScopedHtab::reset_callback;
        callback_.arg = this;
        callback_.next = nullptr;
        MemoryContextRegisterResetCallback(context_, &callback_);
    }

    const char* name_ = nullptr;
    HTAB* table_ = nullptr;
    MemoryContext context_ = nullptr;
    MemoryContextCallback callback_{};
};

} // namespace bioseq::pg_support

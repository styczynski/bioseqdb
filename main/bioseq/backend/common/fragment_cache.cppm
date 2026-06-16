/*-------------------------------------------------------------------------
 *
 * common/fragment_cache.cppm
 *
 * A fixed-capacity cache for sequence fragment handles (DOC0263 §5).
 *
 * This is NOT `circular_append_buffer` (which is a producer-consumer ring
 * with read-once semantics, no random lookup and no explicit pop). Phase B's
 * lazy_compact_sequence wants:
 *   - random lookup-by-key (find a resident fragment by fragment number),
 *   - explicit eviction (evict_oldest / evict_at) with a callback so the
 *     owning scanner can release per-fragment resources (Phase C: pfree),
 *   - a monotonic per-slot generation counter so an in-flight iterator can
 *     detect "the slot I cached has been replaced underneath me".
 *
 * The cache is header-only, allocation-free (the slots live inline in a
 * std::array), and single-threaded — one fragment_cache belongs to one
 * lazy_compact_sequence, which is itself single-threaded (DOC0263 §1.2).
 * Operations are O(N) over N (=16 by default) slots; the linear scan is
 * branchless on modern targets and N is tiny.
 *
 * This module has NO dependency on Postgres — it lives in the NO_POSTGRES
 * shared module library so the unit tests link against it directly
 * (DOC0263 §2.2).
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

export module bioseq.common.fragment_cache;

static_assert(__cplusplus >= 202302L, "DOC0263 requires C++23-or-later");

export namespace bioseq::common {

/// Fixed-capacity cache for `sequence_fragment_handle<R>` (or any movable T).
///
/// Semantics:
///   - insert via push(value, on_evict) -> returns (slot_idx, generation).
///   - look up a resident slot by key via find(key, key_of) ->
///     optional<(slot_idx, gen)>.
///   - explicit eviction via evict_oldest(on_evict) and
///     evict_at(slot_idx, on_evict). The callback is invoked with the
///     evicted value BEFORE the slot is cleared, so the scanner can release
///     the fragment's backing bytes.
///   - per-slot monotonic generation counter (`is_valid_at(idx, gen)`) so a
///     caller that cached a (idx, gen) pair can detect eviction/replacement
///     after a potentially-evicting operation.
template <class T, std::size_t N>
class fragment_cache {
public:
    using slot_idx_t = std::size_t;
    using gen_t      = std::uint64_t;
    static constexpr std::size_t capacity = N;
    static_assert(N >= 1, "fragment_cache needs at least one slot");

    /// Every slot starts empty.
    fragment_cache() noexcept = default;

    fragment_cache(const fragment_cache&)            = delete;
    fragment_cache& operator=(const fragment_cache&) = delete;
    fragment_cache(fragment_cache&&)                 = delete;
    fragment_cache& operator=(fragment_cache&&)      = delete;
    ~fragment_cache()                                = default;

    [[nodiscard]] [[gnu::always_inline]]
    std::size_t valid_count() const noexcept { return _valid_count; }

    /// O(N) scan for a resident slot whose `key_of(slot) == key`. Returns the
    /// slot's (idx, gen) on hit; nullopt on miss. `_hint_idx` records the last
    /// hit so a repeated read of the same fragment is O(1). The hint mutation
    /// under `const` is sound because lazy_compact_sequence (the sole owner)
    /// is single-threaded (DOC0263 §1.2 / consolidated-review N25).
    template <class Key, class KeyOf>
    [[nodiscard]] [[gnu::always_inline]]
    std::optional<std::pair<slot_idx_t, gen_t>>
    find(Key key, KeyOf&& key_of) const noexcept {
        std::size_t i = _hint_idx;
        for (std::size_t step = 0; step < capacity; ++step) {
            if (_filled[i] && key_of(_slots[i]) == key) {
                const_cast<fragment_cache*>(this)->_hint_idx = i;
                return std::pair<slot_idx_t, gen_t>{i, _gens[i]};
            }
            i = (i + 1) % capacity;
        }
        return std::nullopt;
    }

    /// Push a new value, evicting the oldest slot if the cache is full.
    /// Returns (slot_idx, gen). The eviction callback runs BEFORE the slot is
    /// reassigned, so it observes the old value; the new value is moved in
    /// after the callback returns (consolidated-review N46 — the reference
    /// handed to on_evict is valid only for the duration of the call).
    template <class EvictFn>
    [[gnu::always_inline]]
    std::pair<slot_idx_t, gen_t> push(T value, EvictFn&& on_evict) noexcept {
        slot_idx_t idx = 0;
        if (_valid_count < capacity) {
            for (idx = 0; idx < capacity; ++idx) {
                if (!_filled[idx]) break;
            }
        } else {
            idx = _oldest_idx;
            std::forward<EvictFn>(on_evict)(_slots[idx]);
            _oldest_idx = (_oldest_idx + 1) % capacity;
            --_valid_count;
            _filled[idx] = false;
        }
        _slots[idx]  = std::move(value);
        _filled[idx] = true;
        _gens[idx]   = ++_next_gen;
        ++_valid_count;
        _hint_idx = idx;
        return {idx, _gens[idx]};
    }

    /// Evict the genuinely-oldest filled slot. No-op if the cache is empty.
    template <class EvictFn>
    [[gnu::always_inline]]
    void evict_oldest(EvictFn&& on_evict) noexcept {
        if (_valid_count == 0) return;
        slot_idx_t idx = _oldest_idx;
        for (std::size_t step = 0; step < capacity; ++step) {
            if (_filled[idx]) break;
            idx = (idx + 1) % capacity;
        }
        std::forward<EvictFn>(on_evict)(_slots[idx]);
        _filled[idx] = false;
        --_valid_count;
        _oldest_idx = (idx + 1) % capacity;
    }

    /// Evict a specific slot (consolidated-review §17.3). Safe to call with an
    /// out-of-range or already-empty slot (no-op).
    template <class EvictFn>
    [[gnu::always_inline]]
    void evict_at(slot_idx_t idx, EvictFn&& on_evict) noexcept {
        if (idx >= capacity || !_filled[idx]) return;
        std::forward<EvictFn>(on_evict)(_slots[idx]);
        _filled[idx] = false;
        --_valid_count;
    }

    /// True iff (idx, gen) still names the slot the caller saw. False once the
    /// slot has been evicted or replaced.
    [[nodiscard]] [[gnu::always_inline]]
    bool is_valid_at(slot_idx_t idx, gen_t gen) const noexcept {
        return idx < capacity && _filled[idx] && _gens[idx] == gen;
    }

    /// Raw slot accessors. The caller must have checked is_valid_at / find.
    [[nodiscard]] [[gnu::always_inline]]
    const T& slot_at(slot_idx_t idx) const noexcept { return _slots[idx]; }
    [[nodiscard]] [[gnu::always_inline]]
    T& slot_at(slot_idx_t idx) noexcept { return _slots[idx]; }

private:
    std::array<T, capacity>          _slots{};
    std::array<bool, capacity>       _filled{};
    std::array<gen_t, capacity>      _gens{};
    std::size_t                      _valid_count = 0;
    std::size_t                      _oldest_idx  = 0;
    std::size_t                      _hint_idx    = 0;
    gen_t                            _next_gen    = 0;
};

}  // namespace bioseq::common

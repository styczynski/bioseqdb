/**
 * @file concurrency.cppm
 * @brief Lamport-style SPSC ring + simple mutex-guarded MPSC queue.
 *
 * Two primitives live here:
 *
 *   1. **`spsc_ring<T, Capacity>`** — single-producer / single-consumer
 *      bounded queue with relaxed-acquire-release atomics. Capacity is
 *      a power of two so `(counter & MASK)` resolves the slot in one
 *      cycle. Cache-line aligned head and tail counters prevent false
 *      sharing between the producer and consumer cores. Spec §5.2 /
 *      Appendix I. Used on the parser → worker handoff edge.
 *
 *   2. **`mpsc_queue<T>`** — `std::mutex` + `std::condition_variable`
 *      multi-producer / single-consumer queue. Backs the parser-side
 *      `_free_batches` pool that any worker can return a batch to.
 *      Not on the hot path; the simpler lock-based shape is fine.
 *
 * The async pipeline (`pipeline.cppm`) wires both shapes together.
 */
module;

#include "common.h"

#include <array>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>

export module bioseq.io.concurrency;

import bioseq.common.mlogging;

export namespace bioseq::io {

/// Lamport SPSC ring. `_head` and `_tail` are unbounded monotonic
/// counters; the slot index is `(counter & MASK)`. Fullness is
/// `(_head - _tail) >= Capacity`; emptiness is `_head == _tail`.
/// Spec §5.2 / Appendix I.
template <typename T, std::size_t Capacity>
class spsc_ring {
public:
    static_assert(std::has_single_bit(Capacity),
                  "spsc_ring capacity must be a power of two");

    spsc_ring() = default;
    spsc_ring(const spsc_ring&) = delete;
    spsc_ring& operator=(const spsc_ring&) = delete;
    spsc_ring(spsc_ring&&) = delete;
    spsc_ring& operator=(spsc_ring&&) = delete;

    /// Returns false when the ring is full. The producer call site is
    /// expected to retry / spin / wait — this primitive does not
    /// block.
    [[nodiscard]] bool try_push(T value) noexcept {
        const std::uint64_t head = _head.load(std::memory_order_relaxed);
        const std::uint64_t tail = _tail.load(std::memory_order_acquire);
        if (head - tail >= Capacity) return false;
        _slots[head & kMask] = std::move(value);
        _head.store(head + 1, std::memory_order_release);
        return true;
    }

    /// Returns false when the ring is empty.
    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::uint64_t tail = _tail.load(std::memory_order_relaxed);
        const std::uint64_t head = _head.load(std::memory_order_acquire);
        if (head == tail) return false;
        out = std::move(_slots[tail & kMask]);
        _tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::uint64_t head = _head.load(std::memory_order_relaxed);
        const std::uint64_t tail = _tail.load(std::memory_order_relaxed);
        return static_cast<std::size_t>(head - tail);
    }

    [[nodiscard]] bool empty_approx() const noexcept {
        return size_approx() == 0;
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;
    alignas(64) std::atomic<std::uint64_t> _head{0};
    alignas(64) std::atomic<std::uint64_t> _tail{0};
    alignas(64) std::array<T, Capacity> _slots{};
};

/// Mutex-guarded MPSC queue. Used for the cross-worker batch-recycle
/// path where lock contention is rare (batches recycle once per
/// hundreds of records, so the lock is on the cold path).
template <typename T>
class mpsc_queue {
public:
    void push(T value) {
        {
            std::lock_guard<std::mutex> guard{_mutex};
            _items.push(std::move(value));
        }
        _cv.notify_one();
    }

    [[nodiscard]] std::optional<T> try_pop() {
        std::lock_guard<std::mutex> guard{_mutex};
        if (_items.empty()) return std::nullopt;
        T item = std::move(_items.front());
        _items.pop();
        return item;
    }

    /// Block until at least one item is available OR `predicate()`
    /// returns true (typically a "shutdown" flag). Returns nullopt
    /// when predicate fired without an item.
    template <typename PredicateFn>
    [[nodiscard]] std::optional<T> wait_pop_or(PredicateFn&& predicate) {
        std::unique_lock<std::mutex> guard{_mutex};
        _cv.wait(guard, [&] {
            return !_items.empty() || predicate();
        });
        if (_items.empty()) return std::nullopt;
        T item = std::move(_items.front());
        _items.pop();
        return item;
    }

    void notify_all() {
        _cv.notify_all();
    }

    [[nodiscard]] std::size_t size() {
        std::lock_guard<std::mutex> guard{_mutex};
        return _items.size();
    }

private:
    std::mutex _mutex;
    std::condition_variable _cv;
    std::queue<T> _items;
};

}  // namespace bioseq::io

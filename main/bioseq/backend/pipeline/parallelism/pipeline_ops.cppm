/**
 * @file pipeline_ops.cppm
 * @brief Runtime-gated helpers for coarse pipeline parallel regions.
 */
module;
#include "common.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>

export module bioseq.parallelism.pipeline_ops;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;

export namespace bioseq::parallelism {

enum class TaskParallelRegion : std::uint8_t {
    query_batch,
    align_regions,
    segment_recompute,
    fragment_build,
};

struct TaskParallelMinTriggers {
    static constexpr std::size_t MIN_QUERY_COUNT = 4;
    static constexpr std::size_t MIN_REGIONS_COUNT = 4;
    static constexpr std::size_t MIN_SEEDS_COUNT = 64;
    static constexpr std::size_t MIN_FRAGMENTS_COUNT = 4;
};

class TaskParallelScope {
public:
    explicit TaskParallelScope(TaskParallelRegion) noexcept {
        unsigned expected = 0;
        acquired_ = active_regions_.compare_exchange_strong(
            expected, 1, std::memory_order_acq_rel, std::memory_order_relaxed);
    }
    TaskParallelScope(const TaskParallelScope&) = delete;
    TaskParallelScope& operator=(const TaskParallelScope&) = delete;
    ~TaskParallelScope() {
        if (acquired_) {
            active_regions_.store(0, std::memory_order_release);
        }
    }

    [[nodiscard]] bool acquired() const noexcept {
        return acquired_;
    }

    [[nodiscard]] static bool active() noexcept {
        return active_regions_.load(std::memory_order_acquire) != 0;
    }

private:
    inline static std::atomic<unsigned> active_regions_{0};
    bool acquired_ = false;
};

template <class Body>
inline void parallel_for_pipeline(
        std::size_t begin,
        std::size_t end,
        std::size_t grain,
        std::size_t min_items,
        TaskParallelRegion region,
        Body&& body) {
    auto& pool = GlobalPool::instance();
    if (!task_parallel(pool.mode())
        || pool.max_concurrency() <= 1
        || end <= begin
        || (end - begin) < min_items) {
        if (end > begin) {
            body(begin, end);
        }
        return;
    }

    TaskParallelScope scope(region);
    if (!scope.acquired()) {
        body(begin, end);
        return;
    }

    pool.parallel_for_with_context(
        begin, end, grain,
        range_body{[&body](std::size_t lo, std::size_t hi) {
            body(lo, hi);
        }});
}

template <class Body, class SerialFallback>
inline void parallel_for_pipeline_budgeted(
        std::size_t begin,
        std::size_t end,
        std::size_t min_items,
        std::uint64_t peak_bytes_per_item,
        TaskParallelRegion region,
        Body&& body,
        SerialFallback&& serial_fallback) {
    auto& pool = GlobalPool::instance();
    if (!task_parallel(pool.mode())
        || pool.max_concurrency() <= 1
        || end <= begin
        || (end - begin) < min_items) {
        if (end > begin) {
            serial_fallback(begin, end);
        }
        return;
    }

    TaskParallelScope scope(region);
    if (!scope.acquired()) {
        serial_fallback(begin, end);
        return;
    }

    for (std::size_t lo = begin; lo < end;) {
        auto wave = try_reserve_budgeted_wave(pool.max_concurrency(), peak_bytes_per_item);
        const std::size_t hi = std::min(
            end, lo + static_cast<std::size_t>(wave.fanout));
        if (!wave.admitted) {
            serial_fallback(lo, hi);
            lo = hi;
            continue;
        }
        if (wave.fanout <= 1) {
            body(lo, hi);
            lo = hi;
            continue;
        }
        pool.parallel_for_with_context(
            lo, hi, 1,
            range_body{[&body](std::size_t body_lo, std::size_t body_hi) {
                body(body_lo, body_hi);
            }});
        lo = hi;
    }
}

} // namespace bioseq::parallelism

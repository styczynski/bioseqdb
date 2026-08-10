/**
 * @file pipeline_ops.cppm
 * @brief Runtime-gated helpers for coarse pipeline parallel regions.
 */
module;
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

enum class task_parallel_region : std::uint8_t {
    query_batch,
    align_regions,
    segment_recompute,
    fragment_build,
};

/// Minimum item counts that justify entering each coarse TBB region.
struct task_parallel_minimums {
    static constexpr std::size_t k_query_count = 4;
    static constexpr std::size_t k_region_count = 4;
    static constexpr std::size_t k_seed_count = 64;
    static constexpr std::size_t k_fragment_count = 4;
};

/// Non-nesting guard for coarse pipeline parallel regions.
class task_parallel_scope {
public:
    explicit task_parallel_scope(task_parallel_region) noexcept {
        unsigned expected = 0;
        is_acquired_ = active_regions_.compare_exchange_strong(
            expected, 1, std::memory_order_acq_rel, std::memory_order_relaxed);
    }
    task_parallel_scope(const task_parallel_scope&) = delete;
    task_parallel_scope& operator=(const task_parallel_scope&) = delete;
    ~task_parallel_scope() {
        if (is_acquired_) {
            active_regions_.store(0, std::memory_order_release);
        }
    }

    [[nodiscard]] bool is_acquired() const noexcept {
        return is_acquired_;
    }

    [[nodiscard]] static bool is_active() noexcept {
        return active_regions_.load(std::memory_order_acquire) != 0;
    }

private:
    inline static std::atomic<unsigned> active_regions_{0};
    bool is_acquired_ = false;
};

template <class p_body>
inline void parallel_for_pipeline(
        std::size_t begin,
        std::size_t end,
        std::size_t grain,
        std::size_t min_items,
        task_parallel_region region,
        p_body&& body) {
    auto& pool = global_pool::instance();
    if (!has_parallel_tasks(pool.get_mode())
        || pool.maximum_concurrency() <= 1
        || end <= begin
        || (end - begin) < min_items) {
        if (end > begin) {
            body(begin, end);
        }
        return;
    }

    task_parallel_scope scope(region);
    if (!scope.is_acquired()) {
        body(begin, end);
        return;
    }

    pool.parallel_for_with_context(
        begin, end, grain,
        range_body{[&body](std::size_t lo, std::size_t hi) {
            body(lo, hi);
        }});
}

template <class p_body, class p_serial_fallback>
inline void parallel_for_pipeline_budgeted(
        std::size_t begin,
        std::size_t end,
        std::size_t min_items,
        std::uint64_t peak_bytes_per_item,
        task_parallel_region region,
        p_body&& body,
        p_serial_fallback&& serial_fallback) {
    auto& pool = global_pool::instance();
    if (!has_parallel_tasks(pool.get_mode())
        || pool.maximum_concurrency() <= 1
        || end <= begin
        || (end - begin) < min_items) {
        if (end > begin) {
            serial_fallback(begin, end);
        }
        return;
    }

    task_parallel_scope scope(region);
    if (!scope.is_acquired()) {
        serial_fallback(begin, end);
        return;
    }

    for (std::size_t lo = begin; lo < end;) {
        auto wave = try_reserve_budgeted_wave(pool.maximum_concurrency(), peak_bytes_per_item);
        const std::size_t hi = std::min(
            end, lo + static_cast<std::size_t>(wave.fanout));
        if (!wave.is_admitted) {
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

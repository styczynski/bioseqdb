/**
 * @file pipeline_ops.cppm
 * @brief Run independent pipeline work in the shared pool when permitted.
 */
module;
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

export module bioseq.parallelism.pipeline_ops;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;

export namespace bioseq::parallelism {

/// Types of pipeline work that share the parallel-region active flag.
enum class task_parallel_region : std::uint8_t {
    /// Independent queries in one caller-owned batch.
    query_batch,
    /// Independent alignment regions within one query.
    align_regions,
    /// Target segments whose indexes are rebuilt within the memory budget.
    segment_recompute,
    /// Independent fragments built for a persistent index.
    fragment_build,
};

/// Work-size thresholds for scheduling parallel pipeline tasks.
struct task_parallel_minimums {
    static constexpr std::size_t k_query_count = 4;
    static constexpr std::size_t k_region_count = 4;
    static constexpr std::size_t k_seed_count = 64;
    static constexpr std::size_t k_fragment_count = 4;
};

/// Allow one coarse parallel region at a time in this process.
class task_parallel_scope {
public:
  /// Set the shared active flag if it is clear, without waiting.
  explicit task_parallel_scope(task_parallel_region) noexcept {
      unsigned expected = 0;
      is_acquired_ = active_regions_.compare_exchange_strong(
          expected, 1, std::memory_order_acq_rel, std::memory_order_relaxed);
  }
    task_parallel_scope(const task_parallel_scope&) = delete;
    task_parallel_scope& operator=(const task_parallel_scope&) = delete;
    /// Clear the shared active flag if this scope set it.
    ~task_parallel_scope() {
        if (is_acquired_) {
            active_regions_.store(0, std::memory_order_release);
        }
    }

    /// Return whether this scope set the shared active flag.
    [[nodiscard]] bool is_acquired() const noexcept {
        return is_acquired_;
    }

    /// Return whether a parallel region is already active in this process.
    [[nodiscard]] static bool is_active() noexcept {
        return active_regions_.load(std::memory_order_acquire) != 0;
    }

private:
    inline static std::atomic<unsigned> active_regions_{0};
    bool is_acquired_ = false;
};

/**
 * Run [begin, end) in the pool when the mode and size allow it and no other
 * parallel region is active. Otherwise run the range on the caller.
 * Wait for all subranges before returning.
 *
 * @tparam p_body Callable accepting the beginning and end of a subrange.
 */
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

/**
 * Run a range with worker contexts supplied by the admitted factory.
 * Keep the factory and callback alive until all work completes. If the range
 * runs on the caller, it uses the caller's existing context.
 */
template <class p_body>
inline void parallel_for_pipeline(
        std::size_t begin,
        std::size_t end,
        std::size_t grain,
        std::size_t min_items,
        task_parallel_region region,
        p_body&& body,
        worker_context_factory factory) {
    if (factory.state == nullptr || factory.acquire == nullptr ||
        factory.maximum_concurrency == 0u) {
        throw std::invalid_argument{"admitted pipeline range requires a worker factory"};
    }
    auto& pool = global_pool::instance();
    if (!has_parallel_tasks(pool.get_mode()) || pool.maximum_concurrency() <= 1 ||
        factory.maximum_concurrency <= 1u || end <= begin || end - begin < min_items) {
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
    pool.parallel_for_with_context(begin, end, grain,
        range_body{[&body](std::size_t lo, std::size_t hi) { body(lo, hi); }}, factory);
}

/**
 * Run work in groups whose estimated peak memory fits the budget.
 * Use serial_fallback if parallel work is unavailable or memory cannot be reserved.
 * Each group finishes before its permit is released and the next group begins.
 *
 * @tparam p_body Callable for admitted half-open subranges.
 * @tparam p_serial_fallback Callable for subranges that must use the serial path.
 */
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

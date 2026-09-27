/**
 * @file concepts.cppm
 * @brief Task callbacks, pool settings and scheduler counters.
 */
module;

#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

export module bioseq.parallelism.concepts;
import bioseq.parallelism.mode;

export namespace bioseq::parallelism {

/// Callback that processes the half-open range [begin, end).
using range_body = std::function<void(std::size_t, std::size_t)>;

/// Own a task callable, including one with move-only captures.
class task_body {
    /// Common interface for invoking and destroying the stored callable.
    struct base {
        /// Invoke the stored callable.
        virtual void call() = 0;
        /// Destroy, the stored callable through a base pointer.
        virtual ~base() = default;
    };

    /// Store a callable of its original type behind the common interface.
    template <class p_callable>
    struct model final : base {
        p_callable callable;

        /// Take ownership of one scheduled callable.
        explicit model(p_callable&& body) : callable(std::move(body)) {}

        /// Invoke the owned callable.
        void call() override {
            callable();
        }
    };

public:
  /// Create an empty task.
  task_body() = default;

  /** Take ownership of a callable that can be invoked without arguments. */
  template <class p_callable>
      requires(!std::same_as<std::remove_cvref_t<p_callable>, task_body>)
  task_body(p_callable&& callable)
      : body_(std::make_unique<model<std::remove_cvref_t<p_callable>>>(
            std::forward<p_callable>(callable))) {}

  task_body(task_body&&) noexcept = default;
  task_body& operator=(task_body&&) noexcept = default;
  task_body(const task_body&) = delete;
  task_body& operator=(const task_body&) = delete;

  /// Invoke the task when it owns a callable.
  void operator()() {
      if (body_) {
          body_->call();
      }
  }

private:
    std::unique_ptr<base> body_;
};

/// Settings for the oneTBB arena in this process.
struct pool_config {
    /// Maximum simultanoous participants in this arena, including the caller.
    /// Zero uses the oneTBB default. Other arenas keep their  own limits.
    unsigned maximum_concurrency = 1;
    /// Which pipeline work may run in parallel.
    pipeline_concurrency_mode mode = pipeline_concurrency_mode::single_thread;
};

/// Atomic scheduler counters read by PostgreSQL instrumentation.
struct scheduler_stats {
    /// Batches registered with begin_batch().
    std::atomic<std::uint64_t> batches_scheduled{0};
    /// Batches completed through end_batch().
    std::atomic<std::uint64_t> batches_processed{0};
    /// Batches that have neither completed nor been cancelled.
    std::atomic<std::int64_t> batches_in_flight{0};
    /// Queries in batches that have neither completed nor been cancelled.
    std::atomic<std::int64_t> queries_waiting{0};
    /// Queries reported as completed.
    std::atomic<std::uint64_t> queries_processed{0};
    /// Start time for the current throughput window.
    std::atomic<std::uint64_t> rate_window_start_ns{0};
    /// Completed-query count at the start of the throughput window.
    std::atomic<std::uint64_t> rate_window_start_queries{0};
    /// Most recent pool restart time.
    std::atomic<std::uint64_t> restart_ns{0};
    /// Number of slots used to track the ages of unfinished batches.
    static constexpr std::size_t k_inflight_slots = 4096;
    /// Submission times in nanoseconds, with zero marking an unused slot.
    std::array<std::atomic<std::uint64_t>, k_inflight_slots> inflight_submit_ns{};
    /// Increasing batch number used to choose a timestamp slot.
    std::atomic<std::uint64_t> inflight_seq{0};
};

/// Operations a pool must provide to run the  pipeline.
template <class p_pool>
concept thread_pool_like = requires(p_pool& pool, range_body body) {
    { pool.maximum_concurrency() } -> std::convertible_to<unsigned>;
    { pool.live_worker_count() } -> std::convertible_to<unsigned>;
    { pool.get_mode() } -> std::same_as<pipeline_concurrency_mode>;
    { pool.parallel_for(std::size_t{}, std::size_t{}, std::size_t{}, body) } -> std::same_as<void>;
    { pool.parallel_for_with_context(std::size_t{}, std::size_t{}, std::size_t{}, body) } -> std::same_as<void>;
    { pool.get_scheduler_stats() } -> std::same_as<scheduler_stats&>;
};

} // namespace bioseq::parallelism

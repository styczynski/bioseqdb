/**
 * @file concepts.cppm
 * @brief Small no-Postgres callable and stats types used by the global pool.
 */
module;

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

/// Type-erased half-open range callback accepted by pool facades.
using range_body = std::function<void(std::size_t, std::size_t)>;

/// Move-only type-erased task callable used at the pool module boundary.
class task_body {
    /// Type-erased callable storage used only inside one scheduled task.
    struct base {
        virtual void call() = 0;
        virtual ~base() = default;
    };

    template <class p_callable>
    struct model final : base {
        p_callable callable;
        explicit model(p_callable&& body) : callable(std::move(body)) {}
        void call() override {
            callable();
        }
    };

public:
    task_body() = default;

    template <class p_callable>
        requires (!std::same_as<std::remove_cvref_t<p_callable>, task_body>)
    task_body(p_callable&& callable)
        : body_(std::make_unique<model<std::remove_cvref_t<p_callable>>>(
              std::forward<p_callable>(callable))) {}

    task_body(task_body&&) noexcept = default;
    task_body& operator=(task_body&&) noexcept = default;
    task_body(const task_body&) = delete;
    task_body& operator=(const task_body&) = delete;

    void operator()() {
        if (body_) {
            body_->call();
        }
    }

private:
    std::unique_ptr<base> body_;
};

/// Configuration applied atomically to the process-wide TBB arena facade.
struct pool_config {
    /// Local global-pool task-arena ceiling. Zero selects oneTBB's runtime
    /// default. This must not become a process-wide scheduler limit: other
    /// oneTBB arenas may have an independent concurrency contract.
    unsigned maximum_concurrency = 1;
    pipeline_concurrency_mode mode = pipeline_concurrency_mode::single_thread;
};

/// Lock-free scheduler counters sampled by PostgreSQL instrumentation.
struct scheduler_stats {
    std::atomic<std::uint64_t> batches_scheduled{0};
    std::atomic<std::uint64_t> batches_processed{0};
    std::atomic<std::int64_t> batches_in_flight{0};
    std::atomic<std::int64_t> queries_waiting{0};
    std::atomic<std::uint64_t> queries_processed{0};
    std::atomic<std::uint64_t> rate_window_start_ns{0};
    std::atomic<std::uint64_t> rate_window_start_queries{0};
    std::atomic<std::uint64_t> restart_ns{0};
    static constexpr std::size_t k_inflight_slots = 4096;
    std::atomic<std::uint64_t> inflight_submit_ns[k_inflight_slots]{};
    std::atomic<std::uint64_t> inflight_seq{0};
};

/// Pool surface required by scheduler-independent pipeline algorithms.
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

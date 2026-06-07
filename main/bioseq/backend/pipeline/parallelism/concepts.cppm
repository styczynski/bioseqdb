/**
 * @file concepts.cppm
 * @brief Small no-Postgres callable and stats types used by the global pool.
 */
module;
#include "common.h"

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

export module bioseq_parallelism.concepts;
import bioseq_parallelism.mode;

export namespace bioseq::parallelism {

using range_body = std::function<void(std::size_t, std::size_t)>;

class task_body {
    struct base {
        virtual void call() = 0;
        virtual ~base() = default;
    };

    template <class Fn>
    struct model final : base {
        Fn fn;
        explicit model(Fn&& body) : fn(std::move(body)) {}
        void call() override {
            fn();
        }
    };

public:
    task_body() = default;

    template <class Fn>
        requires (!std::same_as<std::remove_cvref_t<Fn>, task_body>)
    task_body(Fn&& fn)
        : body_(std::make_unique<model<std::remove_cvref_t<Fn>>>(std::forward<Fn>(fn))) {}

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

struct PoolConfig {
    unsigned max_concurrency = 1;
    PipelineConcurrencyMode mode = PipelineConcurrencyMode::SINGLE;
};

struct SchedulerStats {
    std::atomic<std::uint64_t> batches_scheduled{0};
    std::atomic<std::uint64_t> batches_processed{0};
    std::atomic<std::int64_t> batches_in_flight{0};
    std::atomic<std::int64_t> queries_waiting{0};
    std::atomic<std::uint64_t> queries_processed{0};
    std::atomic<std::uint64_t> rate_window_start_ns{0};
    std::atomic<std::uint64_t> rate_window_start_queries{0};
    std::atomic<std::uint64_t> restart_ns{0};
    static constexpr std::size_t kInflightSlots = 4096;
    std::atomic<std::uint64_t> inflight_submit_ns[kInflightSlots]{};
    std::atomic<std::uint64_t> inflight_seq{0};
};

template <class Pool>
concept thread_pool_like = requires(Pool& pool, range_body body) {
    { pool.max_concurrency() } -> std::convertible_to<unsigned>;
    { pool.live_workers() } -> std::convertible_to<unsigned>;
    { pool.mode() } -> std::same_as<PipelineConcurrencyMode>;
    { pool.parallel_for(std::size_t{}, std::size_t{}, std::size_t{}, body) } -> std::same_as<void>;
    { pool.parallel_for_with_context(std::size_t{}, std::size_t{}, std::size_t{}, body) } -> std::same_as<void>;
    { pool.stats() } -> std::same_as<SchedulerStats&>;
};

} // namespace bioseq::parallelism

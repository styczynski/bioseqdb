/**
 * @file pool.cppm
 * @brief TBB-backed global workload pool facade.
 *
 * This is intentionally the only module that includes oneTBB headers.
 */
module;

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/enumerable_thread_specific.h>
#include <oneapi/tbb/info.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/parallel_sort.h>
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_group.h>

export module bioseq.parallelism.pool;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.common.context;
import bioseq.instr.clock;

namespace otbb = oneapi::tbb;

namespace bioseq::parallelism::detail {

struct PoolImpl {
    std::optional<otbb::task_arena> arena;
};

struct TaskGroupImpl {
    std::shared_ptr<PoolImpl> pool;
    otbb::task_group group;
};

} // namespace bioseq::parallelism::detail

export namespace bioseq::parallelism {

class ScopedTaskGroup {
public:
    ScopedTaskGroup() = default;
    ScopedTaskGroup(const ScopedTaskGroup&) = delete;
    ScopedTaskGroup& operator=(const ScopedTaskGroup&) = delete;
    ScopedTaskGroup(ScopedTaskGroup&&) noexcept = default;
    ScopedTaskGroup& operator=(ScopedTaskGroup&&) noexcept = default;
    ~ScopedTaskGroup() {
        wait();
    }

    void run(task_body body);
    void wait();

private:
    friend class GlobalPool;
    explicit ScopedTaskGroup(std::shared_ptr<detail::TaskGroupImpl> impl)
        : impl_(std::move(impl)) {}

    std::shared_ptr<detail::TaskGroupImpl> impl_;
};

class GlobalPool {
public:
    static GlobalPool& instance() noexcept {
        static GlobalPool pool;
        return pool;
    }

    void configure(const PoolConfig& config);
    void ensure_configured() const;
    void shutdown() noexcept;
    void shutdown_at_exit() noexcept;
    void restart(const PoolConfig& config);

    [[nodiscard]] unsigned max_concurrency() const noexcept {
        return max_concurrency_;
    }
    [[nodiscard]] unsigned live_workers() const noexcept {
        return live_workers_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] PipelineConcurrencyMode mode() const noexcept {
        return static_cast<PipelineConcurrencyMode>(mode_.load(std::memory_order_relaxed));
    }
    void set_mode(PipelineConcurrencyMode mode) noexcept {
        mode_.store(static_cast<std::uint8_t>(mode), std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint32_t adopted_generation() const noexcept {
        return adopted_generation_.load(std::memory_order_relaxed);
    }
    void set_adopted_generation(std::uint32_t generation) noexcept {
        adopted_generation_.store(generation, std::memory_order_relaxed);
    }
    [[nodiscard]] bool active() const noexcept {
        return configured_.load(std::memory_order_acquire);
    }
    [[nodiscard]] SchedulerStats& stats() noexcept {
        return stats_;
    }

    void parallel_for(
        std::size_t begin,
        std::size_t end,
        std::size_t grain,
        const range_body& body) const;
    void parallel_for_with_context(std::size_t begin, std::size_t end, std::size_t grain, const range_body& body);

    // DOC0346 — TBB parallel sort wrapped in the pool's arena (mirrors
    // parallel_for). Defined inline because it is a template; the wrapper keeps
    // oneTBB the ONLY module that includes its headers (guideline Top-rule).
    template <class RandomIt, class Compare>
    void parallel_sort(RandomIt first, RandomIt last, Compare comp) {
        ensure_configured();
        if (last <= first) {
            return;
        }
        if (max_concurrency_ <= 1) {
            std::sort(first, last, comp);
            return;
        }
        const auto impl = impl_;
        impl->arena->execute([first, last, &comp] {
            otbb::parallel_sort(first, last, comp);
        });
    }

    [[nodiscard]] ScopedTaskGroup make_task_group();

    struct BatchTicket {
        std::size_t slot = 0;
        std::int64_t queries = 0;
    };
    [[nodiscard]] BatchTicket begin_batch(std::int64_t query_count) noexcept;
    void end_batch(BatchTicket ticket) noexcept;
    void cancel_batch(BatchTicket ticket) noexcept;
    void reset_stats(std::uint64_t now_ns) noexcept;

private:
    GlobalPool() = default;
    void teardown_locked(bool join) noexcept;

    std::mutex lifecycle_mu_;
    SchedulerStats stats_{};
    std::atomic<std::uint8_t> mode_{0};
    std::atomic<std::uint32_t> adopted_generation_{0};
    std::atomic<unsigned> live_workers_{0};
    std::atomic<bool> configured_{false};
    unsigned max_concurrency_ = 1;
    std::shared_ptr<detail::PoolImpl> impl_;
};

[[nodiscard]] unsigned default_concurrency() noexcept {
    const int value = otbb::info::default_concurrency();
    return value <= 0 ? 1u : static_cast<unsigned>(value);
}

void GlobalPool::configure(const PoolConfig& config) {
    std::lock_guard<std::mutex> guard(lifecycle_mu_);
    teardown_locked(true);

    const unsigned requested = config.max_concurrency == 0
        ? default_concurrency()
        : config.max_concurrency;
    max_concurrency_ = std::max(1u, requested);
    mode_.store(static_cast<std::uint8_t>(config.mode), std::memory_order_relaxed);

    auto impl = std::make_shared<detail::PoolImpl>();
    impl->arena.emplace(static_cast<int>(max_concurrency_));
    impl->arena->initialize();
    impl_ = std::move(impl);
    live_workers_.store(max_concurrency_, std::memory_order_relaxed);
    configured_.store(true, std::memory_order_release);
}

void GlobalPool::ensure_configured() const {
    if (active()) {
        return;
    }
    // GlobalPool has a private constructor, so every instance is the mutable
    // process singleton even when execution is requested through a const view.
    instance().configure(PoolConfig{});
}

void GlobalPool::teardown_locked(bool join) noexcept {
    configured_.store(false, std::memory_order_release);
    live_workers_.store(0, std::memory_order_relaxed);
    if (impl_) {
        if (join && impl_->arena) {
            impl_->arena->execute([] {});
        }
        impl_.reset();
    }
}

void GlobalPool::shutdown() noexcept {
    std::lock_guard<std::mutex> guard(lifecycle_mu_);
    teardown_locked(true);
}

void GlobalPool::shutdown_at_exit() noexcept {
    std::lock_guard<std::mutex> guard(lifecycle_mu_);
    teardown_locked(false);
}

void GlobalPool::restart(const PoolConfig& config) {
    configure(config);
}

void GlobalPool::parallel_for(
        std::size_t begin,
        std::size_t end,
        std::size_t grain,
        const range_body& body) const {
    ensure_configured();
    if (end <= begin) {
        return;
    }
    const std::size_t effective_grain = std::max<std::size_t>(1, grain);
    if (max_concurrency_ <= 1 || (end - begin) <= effective_grain) {
        body(begin, end);
        return;
    }
    const auto impl = impl_;
    impl->arena->execute([begin, end, effective_grain, &body] {
        otbb::parallel_for(
            otbb::blocked_range<std::size_t>(begin, end, effective_grain),
            [&body](const otbb::blocked_range<std::size_t>& range) {
                body(range.begin(), range.end());
            });
    });
}

void GlobalPool::parallel_for_with_context(
        std::size_t begin,
        std::size_t end,
        std::size_t grain,
        const range_body& body) {
    ensure_configured();
    if (end <= begin) {
        return;
    }
    const std::size_t effective_grain = std::max<std::size_t>(1, grain);
    if (max_concurrency_ <= 1 || (end - begin) <= effective_grain) {
        if (context::current_or_null() != nullptr) {
            body(begin, end);
        } else {
            context::context local_context;
            context::installed_context_scope scope(local_context);
            body(begin, end);
        }
        return;
    }

    otbb::enumerable_thread_specific<context::context> region_contexts;
    const auto impl = impl_;
    impl->arena->execute([begin, end, effective_grain, &body, &region_contexts] {
        otbb::parallel_for(
            otbb::blocked_range<std::size_t>(begin, end, effective_grain),
            [&body, &region_contexts](const otbb::blocked_range<std::size_t>& range) {
                if (context::current_or_null() != nullptr) {
                    body(range.begin(), range.end());
                } else {
                    context::installed_context_scope scope(region_contexts.local());
                    body(range.begin(), range.end());
                }
            });
    });
}

ScopedTaskGroup GlobalPool::make_task_group() {
    ensure_configured();
    auto group = std::make_shared<detail::TaskGroupImpl>();
    group->pool = impl_;
    return ScopedTaskGroup{std::move(group)};
}

GlobalPool::BatchTicket GlobalPool::begin_batch(std::int64_t query_count) noexcept {
    const std::uint64_t seq = stats_.inflight_seq.fetch_add(1, std::memory_order_acq_rel);
    const std::size_t slot = static_cast<std::size_t>(seq % SchedulerStats::kInflightSlots);
    const std::uint64_t now = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    stats_.inflight_submit_ns[slot].store(now, std::memory_order_release);
    stats_.batches_scheduled.fetch_add(1, std::memory_order_acq_rel);
    stats_.batches_in_flight.fetch_add(1, std::memory_order_acq_rel);
    stats_.queries_waiting.fetch_add(query_count, std::memory_order_acq_rel);
    return BatchTicket{slot, query_count};
}

void GlobalPool::end_batch(BatchTicket ticket) noexcept {
    stats_.batches_processed.fetch_add(1, std::memory_order_acq_rel);
    stats_.queries_processed.fetch_add(
        static_cast<std::uint64_t>(std::max<std::int64_t>(0, ticket.queries)),
        std::memory_order_acq_rel);
    cancel_batch(ticket);
}

void GlobalPool::cancel_batch(BatchTicket ticket) noexcept {
    stats_.batches_in_flight.fetch_sub(1, std::memory_order_acq_rel);
    stats_.queries_waiting.fetch_sub(ticket.queries, std::memory_order_acq_rel);
    if (ticket.slot < SchedulerStats::kInflightSlots) {
        stats_.inflight_submit_ns[ticket.slot].store(0, std::memory_order_release);
    }
}

void GlobalPool::reset_stats(std::uint64_t now_ns) noexcept {
    stats_.batches_scheduled.store(0, std::memory_order_release);
    stats_.batches_processed.store(0, std::memory_order_release);
    stats_.batches_in_flight.store(0, std::memory_order_release);
    stats_.queries_waiting.store(0, std::memory_order_release);
    stats_.queries_processed.store(0, std::memory_order_release);
    stats_.rate_window_start_ns.store(now_ns, std::memory_order_release);
    stats_.rate_window_start_queries.store(0, std::memory_order_release);
    stats_.restart_ns.store(now_ns, std::memory_order_release);
    for (std::size_t slot = 0; slot < SchedulerStats::kInflightSlots; ++slot) {
        stats_.inflight_submit_ns[slot].store(0, std::memory_order_release);
    }
    stats_.inflight_seq.store(0, std::memory_order_release);
}

void ScopedTaskGroup::run(task_body body) {
    if (!impl_ || !impl_->pool || !impl_->pool->arena) {
        body();
        return;
    }
    auto body_ptr = std::make_shared<task_body>(std::move(body));
    impl_->pool->arena->execute([group_impl = impl_, body_ptr = std::move(body_ptr)] {
        group_impl->group.run([body_ptr = std::move(body_ptr)] {
            (*body_ptr)();
        });
    });
}

void ScopedTaskGroup::wait() {
    if (!impl_) {
        return;
    }
    if (impl_->pool && impl_->pool->arena) {
        impl_->pool->arena->execute([group_impl = impl_] {
            group_impl->group.wait();
        });
    }
    impl_.reset();
}

} // namespace bioseq::parallelism

/**
 * @file pool.cppm
 * @brief TBB-backed global workload pool facade.
 *
 * This is intentionally the only module that includes oneTBB headers.
 */
module;

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
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

struct tracked_noexcept_callback {
  void *argument{};
  void (*invoke)(void *) noexcept {};
};

struct tracked_job_resources {
  tracked_noexcept_callback cancel{};
  tracked_noexcept_callback join{};
  tracked_noexcept_callback backings{};
};

struct tracked_job_handle {
  std::uint32_t slot_index{};
  std::uint64_t generation{};
};

struct tracked_job_outcome {
  bool drained{};
  bool incomplete{};
  bool failed{};
  std::uint8_t reserved{};
  std::uint32_t sqlstate{};
  std::uint16_t message_length{};
  std::array<char, 238> message{};
};

static_assert(std::is_trivially_copyable_v<tracked_job_outcome>);
static_assert(std::is_standard_layout_v<tracked_job_outcome>);

struct tracked_worker_scope_storage {
  alignas(std::max_align_t) std::array<std::byte, 64> bytes{};
};

struct tracked_job_observer {
  void *context{};
  bool (*enroll)(void *, tracked_job_resources,
                 tracked_job_handle *) noexcept {};
  bool (*mark_scheduled)(void *, tracked_job_handle) noexcept {};
  bool (*mark_finished)(void *, tracked_job_handle) noexcept {};
  bool (*drain)(void *, tracked_job_handle, tracked_job_outcome *) noexcept {};
  void (*handoff_error)(void *, tracked_job_handle, std::uint32_t, char const *,
                        bool) noexcept {};
  void (*worker_enter)(void *, tracked_job_handle,
                       tracked_worker_scope_storage *) noexcept {};
  void (*worker_leave)(void *, tracked_job_handle,
                       tracked_worker_scope_storage *) noexcept {};

  [[nodiscard]] explicit operator bool() const noexcept {
    return context != nullptr && enroll != nullptr &&
           mark_scheduled != nullptr && mark_finished != nullptr &&
           drain != nullptr && worker_enter != nullptr &&
           worker_leave != nullptr;
  }
};

namespace detail {
inline thread_local tracked_job_observer active_tracked_job_observer{};
}

[[nodiscard]] inline tracked_job_observer
current_tracked_job_observer() noexcept {
  return detail::active_tracked_job_observer;
}

[[nodiscard]] inline tracked_job_observer
exchange_tracked_job_observer(tracked_job_observer next) noexcept {
  return std::exchange(detail::active_tracked_job_observer, next);
}

inline void
restore_tracked_job_observer(tracked_job_observer previous) noexcept {
  detail::active_tracked_job_observer = previous;
}

struct tracked_task_ref {
  void *argument{};
  void (*invoke)(void *){};

  [[nodiscard]] explicit operator bool() const noexcept {
    return invoke != nullptr;
  }
};

/* Caller-owned and address-stable until join/drain.  Worker-visible fields are
 * immutable after enqueue; only the two byte handoffs are accessed atomically.
 * This record intentionally owns no heap allocation or std::function. */
struct tracked_task_submission {
  tracked_job_observer observer{};
  tracked_job_handle handle{};
  tracked_task_ref task{};
  tracked_job_outcome outcome{};
  std::uint8_t cancel_requested{};
  std::uint8_t completion_published{};
  std::uint8_t enrolled{};
  std::uint8_t scheduled{};
  std::uint8_t worker_failed{};
  std::uint8_t backing_released{};
  std::array<std::byte, 2> reserved{};
};

static_assert(std::is_trivially_copyable_v<tracked_task_submission>);
static_assert(std::is_standard_layout_v<tracked_task_submission>);

enum class tracked_submit_status : std::uint8_t {
  submitted = 0u,
  invalid,
  enrollment_rejected,
  scheduler_rejected,
  transition_rejected,
};

class ScopedTaskGroup {
public:
  ScopedTaskGroup() = default;
  ScopedTaskGroup(const ScopedTaskGroup &) = delete;
  ScopedTaskGroup &operator=(const ScopedTaskGroup &) = delete;
  ScopedTaskGroup(ScopedTaskGroup &&) noexcept = default;
  ScopedTaskGroup &operator=(ScopedTaskGroup &&) noexcept = default;
  ~ScopedTaskGroup() { wait(); }

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
  static GlobalPool &instance() noexcept {
    static GlobalPool pool;
    return pool;
  }

  void configure(const PoolConfig &config);
  void ensure_configured() const;
  void shutdown() noexcept;
  void shutdown_at_exit() noexcept;
  void restart(const PoolConfig &config);

  [[nodiscard]] unsigned max_concurrency() const noexcept {
    return max_concurrency_;
  }
  [[nodiscard]] unsigned live_workers() const noexcept {
    return live_workers_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] PipelineConcurrencyMode mode() const noexcept {
    return static_cast<PipelineConcurrencyMode>(
        mode_.load(std::memory_order_relaxed));
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
  [[nodiscard]] SchedulerStats &stats() noexcept { return stats_; }

  void parallel_for(std::size_t begin, std::size_t end, std::size_t grain,
                    const range_body &body) const;
  void parallel_for_with_context(std::size_t begin, std::size_t end,
                                 std::size_t grain, const range_body &body);
  void parallel_for_with_context_tracked(std::size_t begin, std::size_t end,
                                         std::size_t grain,
                                         const range_body &body,
                                         tracked_job_observer observer);
  [[nodiscard]] tracked_submit_status
  submit_tracked_task_noexcept(tracked_task_submission *submission,
                               tracked_task_ref task,
                               tracked_job_observer observer) noexcept;
  [[nodiscard]] tracked_job_outcome join_and_drain_tracked_task_noexcept(
      tracked_task_submission *submission) noexcept;

  /**
   * Schedule one callback with task_arena::enqueue and wait for its result.
   * The callback therefore runs on a TBB arena worker, never on the backend
   * thread that invokes this function.  This is deliberately callback-only:
   * PostgreSQL-backed state must already have been materialized before submit.
   */
  [[nodiscard]] bool invoke_worker_probe_noexcept(
      void *argument, bool (*callback)(void *) noexcept) const noexcept;

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
    impl->arena->execute(
        [first, last, &comp] { otbb::parallel_sort(first, last, comp); });
  }

  template <class RandomIt, class Compare>
  void parallel_sort_tracked(RandomIt first, RandomIt last, Compare comp,
                             tracked_job_observer observer) {
    ensure_configured();
    if (!observer || last <= first || max_concurrency_ <= 1) {
      parallel_sort(first, last, std::move(comp));
      return;
    }
    struct sort_task_context {
      GlobalPool *pool;
      RandomIt first;
      RandomIt last;
      Compare *compare;
    } task_context{this, first, last, std::addressof(comp)};
    tracked_task_submission submission{};
    const tracked_task_ref task{
        .argument = std::addressof(task_context),
        .invoke =
            +[](void *argument) {
              auto *context = static_cast<sort_task_context *>(argument);
              /* The comparator consumes only already-admitted resident
               * facades.  It performs no materialization, so nested TBB
               * comparator tasks deliberately inherit no trace sink. */
              context->pool->parallel_sort(context->first, context->last,
                                           *context->compare);
            },
    };
    const auto status = submit_tracked_task_noexcept(std::addressof(submission),
                                                     task, observer);
    const auto outcome =
        join_and_drain_tracked_task_noexcept(std::addressof(submission));
    if (status != tracked_submit_status::submitted || !outcome.drained ||
        outcome.incomplete || outcome.failed) {
      throw std::runtime_error(outcome.message_length == 0u
                                   ? "tracked parallel sort did not complete"
                                   : outcome.message.data());
    }
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

void GlobalPool::configure(const PoolConfig &config) {
  std::lock_guard<std::mutex> guard(lifecycle_mu_);
  teardown_locked(true);

  const unsigned requested = config.max_concurrency == 0
                                 ? default_concurrency()
                                 : config.max_concurrency;
  max_concurrency_ = std::max(1u, requested);
  mode_.store(static_cast<std::uint8_t>(config.mode),
              std::memory_order_relaxed);

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
    impl->arena->execute([begin, end, effective_grain, &body,
                          &region_contexts] {
      otbb::parallel_for(
          otbb::blocked_range<std::size_t>(begin, end, effective_grain),
          [&body,
           &region_contexts](const otbb::blocked_range<std::size_t> &range) {
            if (context::current_or_null() != nullptr) {
              body(range.begin(), range.end());
            } else {
              context::installed_context_scope scope(region_contexts.local());
              body(range.begin(), range.end());
            }
          });
    });
}

bool GlobalPool::invoke_worker_probe_noexcept(
    void *argument, bool (*callback)(void *) noexcept) const noexcept {
  if (callback == nullptr) {
    return false;
  }
  try {
    ensure_configured();
    const auto impl = impl_;
    if (impl == nullptr || !impl->arena.has_value()) {
      return false;
    }
    struct probe_state {
      std::mutex mutex;
      std::condition_variable completed;
      bool done{false};
      bool result{false};
    };
    auto state = std::make_shared<probe_state>();
    impl->arena->enqueue([state, argument, callback] {
      const bool result = callback(argument);
      {
        std::lock_guard<std::mutex> guard(state->mutex);
        state->result = result;
        state->done = true;
      }
      state->completed.notify_one();
    });
    std::unique_lock<std::mutex> guard(state->mutex);
    state->completed.wait(guard, [&state] { return state->done; });
    return state->result;
  } catch (...) {
    return false;
  }
}

} // namespace bioseq::parallelism

namespace bioseq::parallelism {
namespace {

void cancel_tracked_pool_task(void *argument) noexcept {
  auto *submission = static_cast<tracked_task_submission *>(argument);
  if (submission == nullptr) {
    return;
  }
  std::atomic_ref<std::uint8_t>{submission->cancel_requested}.store(
      1u, std::memory_order_release);
}

void join_tracked_pool_task(void *argument) noexcept {
  auto *submission = static_cast<tracked_task_submission *>(argument);
  if (submission == nullptr) {
    return;
  }
  auto completion =
      std::atomic_ref<std::uint8_t>{submission->completion_published};
  std::uint8_t observed = completion.load(std::memory_order_acquire);
  while (observed == 0u) {
    completion.wait(0u, std::memory_order_acquire);
    observed = completion.load(std::memory_order_acquire);
  }
}

void release_tracked_pool_task(void *argument) noexcept {
  auto *submission = static_cast<tracked_task_submission *>(argument);
  if (submission != nullptr) {
    submission->backing_released = 1u;
  }
}

void publish_tracked_pool_completion(
    tracked_task_submission *submission) noexcept {
  auto completion =
      std::atomic_ref<std::uint8_t>{submission->completion_published};
  completion.store(1u, std::memory_order_release);
  completion.notify_all();
}

void handoff_tracked_pool_failure(tracked_task_submission *submission,
                                  char const *message) noexcept {
  submission->worker_failed = 1u;
  if (submission->observer.handoff_error != nullptr) {
    submission->observer.handoff_error(submission->observer.context,
                                       submission->handle, 0u, message, true);
  }
}

void execute_tracked_pool_task(tracked_task_submission *submission) noexcept {
  tracked_worker_scope_storage scope{};
  const auto observer = submission->observer;
  const auto handle = submission->handle;
  const auto task = submission->task;
  const bool cancelled =
      std::atomic_ref<std::uint8_t>{submission->cancel_requested}.load(
          std::memory_order_acquire) != 0u;
  if (!cancelled) {
    observer.worker_enter(observer.context, handle, std::addressof(scope));
    try {
      task.invoke(task.argument);
    } catch (std::exception const &error) {
      handoff_tracked_pool_failure(submission, error.what());
    } catch (...) {
      handoff_tracked_pool_failure(
          submission, "tracked pool task raised an unknown exception");
    }
    observer.worker_leave(observer.context, handle, std::addressof(scope));
  }
  publish_tracked_pool_completion(submission);
}

} // namespace
} // namespace bioseq::parallelism

export namespace bioseq::parallelism {

tracked_submit_status GlobalPool::submit_tracked_task_noexcept(
    tracked_task_submission *submission, tracked_task_ref task,
    tracked_job_observer observer) noexcept {
  if (submission == nullptr || !task || !observer) {
    return tracked_submit_status::invalid;
  }
  *submission = {};
  submission->observer = observer;
  submission->task = task;
  const tracked_job_resources resources{
      .cancel = {submission, &cancel_tracked_pool_task},
      .join = {submission, &join_tracked_pool_task},
      .backings = {submission, &release_tracked_pool_task},
  };
  if (!observer.enroll(observer.context, resources,
                       std::addressof(submission->handle))) {
    return tracked_submit_status::enrollment_rejected;
  }
  submission->enrolled = 1u;
  try {
    ensure_configured();
    const auto impl = impl_;
    impl->arena->enqueue(
        [submission] { execute_tracked_pool_task(submission); });
  } catch (...) {
    handoff_tracked_pool_failure(submission,
                                 "tracked pool task scheduling failed");
    publish_tracked_pool_completion(submission);
    return tracked_submit_status::scheduler_rejected;
  }
  if (!observer.mark_scheduled(observer.context, submission->handle)) {
    cancel_tracked_pool_task(submission);
    return tracked_submit_status::transition_rejected;
  }
  submission->scheduled = 1u;
  return tracked_submit_status::submitted;
}

tracked_job_outcome GlobalPool::join_and_drain_tracked_task_noexcept(
    tracked_task_submission *submission) noexcept {
  if (submission == nullptr) {
    return {};
  }
  if (submission->enrolled == 0u) {
    return submission->outcome;
  }
  join_tracked_pool_task(submission);
  if (submission->scheduled != 0u &&
      !submission->observer.mark_finished(submission->observer.context,
                                          submission->handle)) {
    handoff_tracked_pool_failure(
        submission, "tracked pool task rejected its finished transition");
  }
  tracked_job_outcome outcome{};
  const bool drained =
      submission->observer.drain(submission->observer.context,
                                 submission->handle, std::addressof(outcome));
  outcome.drained = drained;
  outcome.failed = outcome.failed || submission->worker_failed != 0u;
  submission->outcome = outcome;
  if (drained) {
    submission->enrolled = 0u;
    submission->scheduled = 0u;
  }
  return outcome;
}

void GlobalPool::parallel_for_with_context_tracked(
    std::size_t begin, std::size_t end, std::size_t grain,
    const range_body &body, tracked_job_observer observer) {
  ensure_configured();
  const std::size_t effective_grain = std::max<std::size_t>(1u, grain);
  if (!observer || end <= begin || max_concurrency_ <= 1u ||
      end - begin <= effective_grain) {
    parallel_for_with_context(begin, end, grain, body);
    return;
  }
  struct tracked_range_context {
    range_body const *body;
    std::size_t begin;
    std::size_t end;
  };
  const std::size_t midpoint = begin + (end - begin) / 2u;
  std::array<tracked_range_context, 2> contexts{{
      {std::addressof(body), begin, midpoint},
      {std::addressof(body), midpoint, end},
  }};
  std::array<tracked_task_submission, 2> submissions{};
  std::array<tracked_submit_status, 2> statuses{};
  const auto invoke_range = +[](void *argument) {
    auto *range = static_cast<tracked_range_context *>(argument);
    if (context::current_or_null() != nullptr) {
      (*range->body)(range->begin, range->end);
      return;
    }
    context::context local_context;
    context::installed_context_scope scope(local_context);
    (*range->body)(range->begin, range->end);
  };
  for (std::size_t index = 0u; index < submissions.size(); ++index) {
    statuses[index] = submit_tracked_task_noexcept(
        std::addressof(submissions[index]),
        {std::addressof(contexts[index]), invoke_range}, observer);
  }
  std::array<tracked_job_outcome, 2> outcomes{};
  for (std::size_t index = 0u; index < submissions.size(); ++index) {
    outcomes[index] = join_and_drain_tracked_task_noexcept(
        std::addressof(submissions[index]));
  }
  for (std::size_t index = 0u; index < submissions.size(); ++index) {
    if (statuses[index] != tracked_submit_status::submitted ||
        !outcomes[index].drained || outcomes[index].incomplete ||
        outcomes[index].failed) {
      throw std::runtime_error(outcomes[index].message_length == 0u
                                   ? "tracked parallel range did not complete"
                                   : outcomes[index].message.data());
    }
  }
}

ScopedTaskGroup GlobalPool::make_task_group() {
  ensure_configured();
  auto group = std::make_shared<detail::TaskGroupImpl>();
  group->pool = impl_;
  return ScopedTaskGroup{std::move(group)};
}

GlobalPool::BatchTicket
GlobalPool::begin_batch(std::int64_t query_count) noexcept {
  const std::uint64_t seq =
      stats_.inflight_seq.fetch_add(1, std::memory_order_acq_rel);
  const std::size_t slot =
      static_cast<std::size_t>(seq % SchedulerStats::kInflightSlots);
  const std::uint64_t now =
      static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
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

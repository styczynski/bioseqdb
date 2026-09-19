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
#include <exception>
#include <functional>
#include <limits>
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
import bioseq.instrumentation.clock;

namespace otbb = oneapi::tbb;

namespace bioseq::parallelism::detail {

/// Private oneTBB arena ownership kept out of the exported class layout.
struct pool_implementation {
    std::optional<otbb::task_arena> arena;
};

/// Private task-group ownership tied to the pool arena lifetime.
struct task_group_implementation {
    std::shared_ptr<pool_implementation> pool;
    otbb::task_group group;
};

} // namespace bioseq::parallelism::detail

export namespace bioseq::parallelism {

/// Non-owning acquisition boundary for caller-admitted per-worker contexts.
struct worker_context_factory {
    /// Caller-owned, address-stable slot owner, retained until the joined call returns.
    void* state = nullptr;
    /// Bounded number of independently admitted worker slots.
    std::size_t maximum_concurrency = 0u;
    /// Claims and constructs one slot per participating worker. May throw on denial.
    context::context& (*acquire)(void*) = nullptr;
};

/// Non-owning callback used while draining scheduler-controlled resources.
struct tracked_noexcept_callback {
    /// Opaque caller-owned callback state.
    void* argument{};
    /// Callback that must not propagate an exception.
    void (*invoke)(void*) noexcept {};
};

/// Cleanup callbacks registered for one tracked scheduler job.
struct tracked_job_resources {
    /// Request cancellation of queued work.
    tracked_noexcept_callback cancel{};
    /// Join the submitted work.
    tracked_noexcept_callback join{};
    /// Release caller-owned backing state after drain.
    tracked_noexcept_callback backings{};
};

/// Generation-checked slot handle for a tracked scheduler job.
struct tracked_job_handle {
    /// Scheduler slot selected during enrollment.
    std::uint32_t slot_index{};
    /// Generation protecting the slot from stale completions.
    std::uint64_t generation{};
};

/// Fixed-layout terminal outcome copied across the scheduler boundary.
struct tracked_job_outcome {
    /// Scheduler resources reached their drained state.
    bool is_drained{};
    /// A terminal transition was missing.
    bool is_incomplete{};
    /// Worker or scheduler execution reported a failure.
    bool has_failure{};
    /// Reserved layout byte.
    std::uint8_t reserved{};
    /// SQLSTATE supplied by the scheduler boundary, or zero.
    std::uint32_t sqlstate{};
    /// Used bytes in the fixed message buffer.
    std::uint16_t message_length{};
    /// Fixed-capacity error text copied across the scheduler boundary.
    std::array<char, 238> message{};
};

static_assert(std::is_trivially_copyable_v<tracked_job_outcome>);
static_assert(std::is_standard_layout_v<tracked_job_outcome>);

/// Caller-owned inline storage for one worker's scheduler scope.
struct tracked_worker_scope_storage {
    /// Aligned bytes interpreted only by the scheduler observer.
    alignas(std::max_align_t) std::array<std::byte, 64> bytes{};
};

/// Callback table connecting pool tasks to scheduler job accounting.
struct tracked_job_observer {
    /// Opaque scheduler-owned observer state.
    void* context{};
    /// Enroll one tracked job and return its generation-checked handle.
    bool (*enroll)(void*, tracked_job_resources, tracked_job_handle*) noexcept {};
    /// Publish the scheduled transition.
    bool (*mark_scheduled)(void*, tracked_job_handle) noexcept {};
    /// Publish the worker-finished transition.
    bool (*mark_finished)(void*, tracked_job_handle) noexcept {};
    /// Drain scheduler resources and copy the terminal outcome.
    bool (*drain)(void*, tracked_job_handle, tracked_job_outcome*) noexcept {};
    /// Transfer worker failure information to the scheduler boundary.
    void (*handoff_error)(void*, tracked_job_handle, std::uint32_t, char const*, bool) noexcept {};
    /// Install worker-local scheduler state before invoking user work.
    void (*worker_enter)(void*, tracked_job_handle, tracked_worker_scope_storage*) noexcept {};
    /// Remove worker-local scheduler state after invoking user work.
    void (*worker_leave)(void*, tracked_job_handle, tracked_worker_scope_storage*) noexcept {};

    /// Return whether every mandatory observer callback is present.
    [[nodiscard]] explicit operator bool() const noexcept {
        return context != nullptr && enroll != nullptr && mark_scheduled != nullptr &&
               mark_finished != nullptr && drain != nullptr && worker_enter != nullptr &&
               worker_leave != nullptr;
    }
};

namespace detail {
inline thread_local tracked_job_observer active_tracked_job_observer{};
}

/// Return the observer currently installed on this thread.
[[nodiscard]] inline tracked_job_observer
current_tracked_job_observer() noexcept {
  return detail::active_tracked_job_observer;
}

/// Replace the thread-local observer and return its previous value.
[[nodiscard]] inline tracked_job_observer
exchange_tracked_job_observer(tracked_job_observer next) noexcept {
  return std::exchange(detail::active_tracked_job_observer, next);
}

/// Restore a previously exchanged thread-local observer.
inline void
restore_tracked_job_observer(tracked_job_observer previous) noexcept {
  detail::active_tracked_job_observer = previous;
}

/// Non-owning executable task reference used by tracked submissions.
struct tracked_task_ref {
    /// Opaque caller-owned task state.
    void* argument{};
    /// Invoke the caller-owned task state.
    void (*invoke)(void*){};

    /// Return whether this task reference has an invocation target.
    [[nodiscard]] explicit operator bool() const noexcept {
        return invoke != nullptr;
    }
};

/** Caller-owned and address-stable until join/drain. Worker-visible fields are
 * immutable after enqueue. Only the two byte handoffs are accessed atomically.
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

/// Result of attempting to enqueue a tracked task.
enum class tracked_submit_status : std::uint8_t {
    /// Task enrollment and enqueue completed.
    submitted = 0u,
    /// Submission, task, or observer was incomplete.
    invalid,
    /// The scheduler refused enrollment.
    enrollment_rejected,
    /// The TBB arena refused or failed to enqueue the task.
    scheduler_rejected,
    /// The scheduler rejected the scheduled transition.
    transition_rejected,
};

/// Move-only oneTBB task group that joins before releasing its arena owner.
class scoped_task_group {
public:
  /// Create an empty group that executes tasks synchronously.
  scoped_task_group() = default;
  scoped_task_group(const scoped_task_group &) = delete;
  scoped_task_group &operator=(const scoped_task_group &) = delete;
  scoped_task_group(scoped_task_group &&) noexcept = default;
  scoped_task_group &operator=(scoped_task_group &&) noexcept = default;
  /// Join submitted tasks before releasing the arena owner.
  ~scoped_task_group() { wait(); }

  /// Submit one move-only task to the group or run it synchronously.
  void run(task_body body);
  /// Join all submitted tasks and release the group implementation.
  void wait();

private:
  friend class global_pool;
  /// Adopt one arena-bound task-group implementation.
  explicit scoped_task_group(std::shared_ptr<detail::task_group_implementation> implementation)
      : implementation_(std::move(implementation)) {}

  std::shared_ptr<detail::task_group_implementation> implementation_;
};

/// Process-wide facade over the pipeline's oneTBB task arena.
class global_pool {
public:
  /// Return the process-wide pipeline pool facade.
  static global_pool &instance() noexcept {
    static global_pool pool;
    return pool;
  }

  /// Replace the owned TBB arena using one atomic configuration boundary.
  void configure(const pool_config &config);
  /// Lazily create the default arena when no configuration is active.
  void ensure_configured() const;
  /// Join work and release the owned arena.
  void shutdown() noexcept;
  /// Release the arena during process teardown without re-entering workers.
  void shutdown_at_exit() noexcept;
  /// Recreate the arena with a new configuration.
  void restart(const pool_config &config);

  /// Return the concurrency ceiling of the owned arena.
  [[nodiscard]] unsigned maximum_concurrency() const noexcept {
    return maximum_concurrency_;
  }
  /// Return the configured worker count while the arena is active.
  [[nodiscard]] unsigned live_worker_count() const noexcept {
    return live_worker_count_.load(std::memory_order_relaxed);
  }
  /// Return the current pipeline concurrency mode.
  [[nodiscard]] pipeline_concurrency_mode get_mode() const noexcept {
    return static_cast<pipeline_concurrency_mode>(
        mode_.load(std::memory_order_relaxed));
  }
  /// Set the runtime pipeline concurrency mode.
  void set_mode(pipeline_concurrency_mode mode) noexcept {
    mode_.store(static_cast<std::uint8_t>(mode), std::memory_order_relaxed);
  }
  /// Return the scheduler generation adopted by this pool.
  [[nodiscard]] std::uint32_t adopted_generation() const noexcept {
    return adopted_generation_.load(std::memory_order_relaxed);
  }
  /// Record the scheduler generation adopted by this pool.
  void set_adopted_generation(std::uint32_t generation) noexcept {
    adopted_generation_.store(generation, std::memory_order_relaxed);
  }
  /// Return whether an owned arena is configured.
  [[nodiscard]] bool is_active() const noexcept {
    return configured_.load(std::memory_order_acquire);
  }
  /// Return the lock-free scheduler counters owned by this pool.
  [[nodiscard]] scheduler_stats &get_scheduler_stats() noexcept { return scheduler_stats_; }

  /// Execute a half-open range inside the owned arena.
  void parallel_for(std::size_t begin, std::size_t end, std::size_t grain,
                    const range_body &body) const;
  /// Execute a half-open range with one context per participating worker.
  void parallel_for_with_context(std::size_t begin, std::size_t end,
                                 std::size_t grain, const range_body &body);
  /// Execute with caller-admitted contexts; never borrow an unrelated installed context.
  void parallel_for_with_context(std::size_t begin, std::size_t end,
                                 std::size_t grain, const range_body &body,
                                 worker_context_factory factory);
  /// Execute range shards through the tracked scheduler lifecycle.
  void parallel_for_with_context_tracked(std::size_t begin, std::size_t end,
                                         std::size_t grain,
                                         const range_body &body,
                                         tracked_job_observer observer);
  /// Track joined shards using caller-owned, physically admitted fresh contexts.
  void parallel_for_with_context_tracked(std::size_t begin, std::size_t end,
                                         std::size_t grain,
                                         const range_body &body,
                                         tracked_job_observer observer,
                                         worker_context_factory factory);
  /// Enroll and enqueue one address-stable tracked task without throwing.
  [[nodiscard]] tracked_submit_status
  submit_tracked_task_noexcept(tracked_task_submission *submission,
                               tracked_task_ref task,
                               tracked_job_observer observer) noexcept;
  /// Join one tracked task and drain its scheduler resources without throwing.
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

  /// Sort inside the owned arena so the configured concurrency ceiling applies.
  template <class p_random_iterator, class p_compare>
  void parallel_sort(
      p_random_iterator first,
      p_random_iterator last,
      p_compare compare) {
    ensure_configured();
    if (last <= first) {
      return;
    }
    if (maximum_concurrency_ <= 1) {
      std::sort(first, last, compare);
      return;
    }
    const auto implementation = implementation_;
    implementation->arena->execute(
        [first, last, &compare] {
          otbb::parallel_sort(first, last, compare);
        });
  }

  /// Sort through the tracked-job observer lifecycle without nested dispatch.
  template <class p_random_iterator, class p_compare>
  void parallel_sort_tracked(
      p_random_iterator first,
      p_random_iterator last,
      p_compare compare,
      tracked_job_observer observer) {
    ensure_configured();
    if (!observer || last <= first || maximum_concurrency_ <= 1) {
      parallel_sort(first, last, std::move(compare));
      return;
    }
    struct sort_task_context {
      global_pool *pool;
      p_random_iterator first;
      p_random_iterator last;
      p_compare *compare;
    } task_context{this, first, last, std::addressof(compare)};
    tracked_task_submission submission{};
    const tracked_task_ref task{
        .argument = std::addressof(task_context),
        .invoke =
            +[](void *argument) {
              auto *context = static_cast<sort_task_context *>(argument);
              /* The comparator consumes only already-is_admitted resident
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
    if (status != tracked_submit_status::submitted || !outcome.is_drained ||
        outcome.is_incomplete || outcome.has_failure) {
      throw std::runtime_error(outcome.message_length == 0u
                                   ? "tracked parallel sort did not complete"
                                   : outcome.message.data());
    }
  }

  /// Create a task group tied to the current arena lifetime.
  [[nodiscard]] scoped_task_group make_task_group();

  /// Rolling slot and query count owned by one submitted batch.
  struct batch_ticket {
      /// Timestamp slot selected for this batch.
      std::size_t slot = 0;
      /// Queries represented by this batch.
      std::int64_t queries = 0;
  };
  /// Record a newly scheduled batch and return its completion ticket.
  [[nodiscard]] batch_ticket begin_batch(std::int64_t query_count) noexcept;
  /// Record successful completion of one batch.
  void end_batch(batch_ticket ticket) noexcept;
  /// Remove an unfinished or completed batch from in-flight counters.
  void cancel_batch(batch_ticket ticket) noexcept;
  /// Reset scheduler counters at a new rate-window boundary.
  void reset_stats(std::uint64_t now_ns) noexcept;

private:
  global_pool() = default;
  /// Release the arena while the lifecycle mutex is held.
  void teardown_locked(bool join) noexcept;

  std::mutex lifecycle_mutex_;
  scheduler_stats scheduler_stats_{};
  std::atomic<std::uint8_t> mode_{0};
  std::atomic<std::uint32_t> adopted_generation_{0};
  std::atomic<unsigned> live_worker_count_{0};
  std::atomic<bool> configured_{false};
  unsigned maximum_concurrency_ = 1;
  std::shared_ptr<detail::pool_implementation> implementation_;
};

/// Return oneTBB's normalized default arena concurrency.
[[nodiscard]] unsigned default_concurrency_limit() noexcept {
  const int value = otbb::info::default_concurrency();
  return value <= 0 ? 1u : static_cast<unsigned>(value);
}

void global_pool::configure(const pool_config &config) {
  std::lock_guard<std::mutex> guard(lifecycle_mutex_);
  teardown_locked(true);

  const unsigned requested = config.maximum_concurrency == 0
                                 ? default_concurrency_limit()
                                 : config.maximum_concurrency;
  maximum_concurrency_ = std::max(1u, requested);
  mode_.store(static_cast<std::uint8_t>(config.mode),
              std::memory_order_relaxed);

  auto implementation = std::make_shared<detail::pool_implementation>();
  implementation->arena.emplace(static_cast<int>(maximum_concurrency_));
  implementation->arena->initialize();
  implementation_ = std::move(implementation);
  live_worker_count_.store(maximum_concurrency_, std::memory_order_relaxed);
  configured_.store(true, std::memory_order_release);
}

void global_pool::ensure_configured() const {
  if (is_active()) {
    return;
  }
  // global_pool has a private constructor, so every instance is the mutable
  // process singleton even when execution is requested through a const view.
  instance().configure(pool_config{});
}

void global_pool::teardown_locked(bool join) noexcept {
  configured_.store(false, std::memory_order_release);
  live_worker_count_.store(0, std::memory_order_relaxed);
  if (implementation_) {
    if (join && implementation_->arena) {
      implementation_->arena->execute([] {});
    }
    implementation_.reset();
  }
}

void global_pool::shutdown() noexcept {
  std::lock_guard<std::mutex> guard(lifecycle_mutex_);
  teardown_locked(true);
}

void global_pool::shutdown_at_exit() noexcept {
  std::lock_guard<std::mutex> guard(lifecycle_mutex_);
  teardown_locked(false);
}

void global_pool::restart(const pool_config& config) {
    configure(config);
}

void global_pool::parallel_for(
        std::size_t begin,
        std::size_t end,
        std::size_t grain,
        const range_body& body) const {
    ensure_configured();
    if (end <= begin) {
        return;
    }
    const std::size_t effective_grain = std::max<std::size_t>(1, grain);
    if (maximum_concurrency_ <= 1 || (end - begin) <= effective_grain) {
        body(begin, end);
        return;
    }
    const auto implementation = implementation_;
    implementation->arena->execute([begin, end, effective_grain, &body] {
        otbb::parallel_for(
            otbb::blocked_range<std::size_t>(begin, end, effective_grain),
            [&body](const otbb::blocked_range<std::size_t>& range) {
                body(range.begin(), range.end());
            });
    });
}

void global_pool::parallel_for_with_context(
        std::size_t begin,
        std::size_t end,
        std::size_t grain,
        const range_body& body) {
    ensure_configured();
    if (end <= begin) {
        return;
    }
    const std::size_t effective_grain = std::max<std::size_t>(1, grain);
    if (maximum_concurrency_ <= 1 || (end - begin) <= effective_grain) {
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
    const auto implementation = implementation_;
    implementation->arena->execute([begin, end, effective_grain, &body,
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

void global_pool::parallel_for_with_context(
        std::size_t begin,
        std::size_t end,
        std::size_t grain,
        const range_body& body,
        worker_context_factory factory) {
    ensure_configured();
    if (end <= begin)
        return;
    if (factory.state == nullptr || factory.acquire == nullptr ||
        factory.maximum_concurrency == 0u ||
        factory.maximum_concurrency > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        throw std::invalid_argument{"parallel context factory is not a bounded live owner"};
    }
    const std::size_t effective_grain = std::max<std::size_t>(1u, grain);
    const std::size_t concurrency = std::min<std::size_t>(
        maximum_concurrency_, factory.maximum_concurrency);
    if (concurrency <= 1u || end - begin <= effective_grain) {
        context::installed_context_scope scope(factory.acquire(factory.state));
        body(begin, end);
        return;
    }

    // Only a participating worker claims a slot, exactly once in this ETS map.
    // Exceptions from acquisition or work cancel and join the TBB operation
    // before its caller may destroy any slot or associated physical permit.
    otbb::enumerable_thread_specific<context::context*> worker_contexts(
        [factory] { return std::addressof(factory.acquire(factory.state)); });
    const auto implementation = implementation_;
    implementation->arena->execute([begin, end, effective_grain, concurrency,
                                    &body, &worker_contexts] {
        otbb::task_arena bounded_workers{static_cast<int>(concurrency)};
        bounded_workers.execute([begin, end, effective_grain, &body, &worker_contexts] {
            otbb::parallel_for(
                otbb::blocked_range<std::size_t>(begin, end, effective_grain),
                [&body, &worker_contexts](const otbb::blocked_range<std::size_t>& range) {
                    context::installed_context_scope scope(*worker_contexts.local());
                    body(range.begin(), range.end());
                });
        });
    });
}

bool global_pool::invoke_worker_probe_noexcept(
    void *argument, bool (*callback)(void *) noexcept) const noexcept {
  if (callback == nullptr) {
    return false;
  }
  try {
    ensure_configured();
    const auto implementation = implementation_;
    if (implementation == nullptr || !implementation->arena.has_value()) {
      return false;
    }
    struct probe_state {
      std::mutex mutex;
      std::condition_variable completed;
      bool done{false};
      bool result{false};
    };
    auto state = std::make_shared<probe_state>();
    implementation->arena->enqueue([state, argument, callback] {
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

/// Publish cancellation to a tracked task's atomic handoff byte.
void cancel_tracked_pool_task(void *argument) noexcept {
  auto *submission = static_cast<tracked_task_submission *>(argument);
  if (submission == nullptr) {
    return;
  }
  std::atomic_ref<std::uint8_t>{submission->cancel_requested}.store(
      1u, std::memory_order_release);
}

/// Wait for a tracked task to publish completion.
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

/// Mark caller-owned backing state as released after scheduler drain.
void release_tracked_pool_task(void *argument) noexcept {
  auto *submission = static_cast<tracked_task_submission *>(argument);
  if (submission != nullptr) {
    submission->backing_released = 1u;
  }
}

/// Publish completion and wake every tracked-task joiner.
void publish_tracked_pool_completion(
    tracked_task_submission *submission) noexcept {
  auto completion =
      std::atomic_ref<std::uint8_t>{submission->completion_published};
  completion.store(1u, std::memory_order_release);
  completion.notify_all();
}

/// Transfer a worker failure to the scheduler observer.
void handoff_tracked_pool_failure(tracked_task_submission *submission,
                                  char const *message) noexcept {
  submission->worker_failed = 1u;
  if (submission->observer.handoff_error != nullptr) {
    submission->observer.handoff_error(submission->observer.context,
                                       submission->handle, 0u, message, true);
  }
}

/// Execute one tracked task inside its scheduler worker scope.
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

tracked_submit_status global_pool::submit_tracked_task_noexcept(
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
    const auto implementation = implementation_;
    implementation->arena->enqueue(
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

tracked_job_outcome global_pool::join_and_drain_tracked_task_noexcept(
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
  const bool is_drained =
      submission->observer.drain(submission->observer.context,
                                 submission->handle, std::addressof(outcome));
  outcome.is_drained = is_drained;
  outcome.has_failure = outcome.has_failure || submission->worker_failed != 0u;
  submission->outcome = outcome;
  if (is_drained) {
    submission->enrolled = 0u;
    submission->scheduled = 0u;
  }
  return outcome;
}

void global_pool::parallel_for_with_context_tracked(
    std::size_t begin, std::size_t end, std::size_t grain,
    const range_body &body, tracked_job_observer observer) {
  ensure_configured();
  const std::size_t effective_grain = std::max<std::size_t>(1u, grain);
  if (!observer || end <= begin || maximum_concurrency_ <= 1u ||
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
        !outcomes[index].is_drained || outcomes[index].is_incomplete ||
        outcomes[index].has_failure) {
      throw std::runtime_error(outcomes[index].message_length == 0u
                                   ? "tracked parallel range did not complete"
                                   : outcomes[index].message.data());
    }
  }
}

void global_pool::parallel_for_with_context_tracked(
    std::size_t begin, std::size_t end, std::size_t grain,
    const range_body &body, tracked_job_observer observer,
    worker_context_factory factory) {
  ensure_configured();
  if (end <= begin) {
    return;
  }
  if (factory.state == nullptr || factory.acquire == nullptr ||
      factory.maximum_concurrency == 0u ||
      factory.maximum_concurrency > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    throw std::invalid_argument{"tracked range requires a bounded worker context factory"};
  }
  const std::size_t effective_grain = std::max<std::size_t>(1u, grain);
  if (!observer || maximum_concurrency_ <= 1u || factory.maximum_concurrency <= 1u ||
      end - begin <= effective_grain) {
    parallel_for_with_context(begin, end, grain, body, factory);
    return;
  }

  // Preserve the scheduler's existing two-shard lifecycle. The stack records
  // and their borrowed factory remain live until BOTH tasks join and drain,
  // including acquisition failure, cancellation and a throwing range body.
  struct admitted_tracked_range_context {
    range_body const *body;
    std::size_t begin;
    std::size_t end;
    worker_context_factory factory;
  };
  const std::size_t midpoint = begin + (end - begin) / 2u;
  std::array<admitted_tracked_range_context, 2> ranges{{
      {std::addressof(body), begin, midpoint, factory},
      {std::addressof(body), midpoint, end, factory},
  }};
  std::array<tracked_task_submission, 2> submissions{};
  std::array<tracked_submit_status, 2> statuses{};
  const auto invoke_range = +[](void *argument) {
    auto& range = *static_cast<admitted_tracked_range_context*>(argument);
    // A scheduler observer may have installed its own context. It is not the
    // biological attempt's allocation authority and must never be borrowed.
    context::installed_context_scope scope{
        range.factory.acquire(range.factory.state)};
    (*range.body)(range.begin, range.end);
  };
  for (std::size_t index = 0u; index < submissions.size(); ++index) {
    statuses[index] = submit_tracked_task_noexcept(
        std::addressof(submissions[index]),
        {std::addressof(ranges[index]), invoke_range}, observer);
  }
  std::array<tracked_job_outcome, 2> outcomes{};
  for (std::size_t index = 0u; index < submissions.size(); ++index) {
    outcomes[index] = join_and_drain_tracked_task_noexcept(
        std::addressof(submissions[index]));
  }
  for (std::size_t index = 0u; index < submissions.size(); ++index) {
    if (statuses[index] != tracked_submit_status::submitted ||
        !outcomes[index].is_drained || outcomes[index].is_incomplete ||
        outcomes[index].has_failure) {
      throw std::runtime_error(outcomes[index].message_length == 0u
                                   ? "admitted tracked parallel range did not complete"
                                   : outcomes[index].message.data());
    }
  }
}

scoped_task_group global_pool::make_task_group() {
  ensure_configured();
  auto group = std::make_shared<detail::task_group_implementation>();
  group->pool = implementation_;
  return scoped_task_group{std::move(group)};
}

global_pool::batch_ticket
global_pool::begin_batch(std::int64_t query_count) noexcept {
  const std::uint64_t seq =
      scheduler_stats_.inflight_seq.fetch_add(1, std::memory_order_acq_rel);
  const std::size_t slot =
      static_cast<std::size_t>(seq % scheduler_stats::k_inflight_slots);
  const std::uint64_t now =
      static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
  scheduler_stats_.inflight_submit_ns[slot].store(now, std::memory_order_release);
  scheduler_stats_.batches_scheduled.fetch_add(1, std::memory_order_acq_rel);
  scheduler_stats_.batches_in_flight.fetch_add(1, std::memory_order_acq_rel);
  scheduler_stats_.queries_waiting.fetch_add(query_count, std::memory_order_acq_rel);
  return batch_ticket{slot, query_count};
}

void global_pool::end_batch(batch_ticket ticket) noexcept {
  scheduler_stats_.batches_processed.fetch_add(1, std::memory_order_acq_rel);
  scheduler_stats_.queries_processed.fetch_add(
      static_cast<std::uint64_t>(std::max<std::int64_t>(0, ticket.queries)),
      std::memory_order_acq_rel);
  cancel_batch(ticket);
}

void global_pool::cancel_batch(batch_ticket ticket) noexcept {
    scheduler_stats_.batches_in_flight.fetch_sub(1, std::memory_order_acq_rel);
    scheduler_stats_.queries_waiting.fetch_sub(ticket.queries, std::memory_order_acq_rel);
    if (ticket.slot < scheduler_stats::k_inflight_slots) {
        scheduler_stats_.inflight_submit_ns[ticket.slot].store(0, std::memory_order_release);
    }
}

void global_pool::reset_stats(std::uint64_t now_ns) noexcept {
    scheduler_stats_.batches_scheduled.store(0, std::memory_order_release);
    scheduler_stats_.batches_processed.store(0, std::memory_order_release);
    scheduler_stats_.batches_in_flight.store(0, std::memory_order_release);
    scheduler_stats_.queries_waiting.store(0, std::memory_order_release);
    scheduler_stats_.queries_processed.store(0, std::memory_order_release);
    scheduler_stats_.rate_window_start_ns.store(now_ns, std::memory_order_release);
    scheduler_stats_.rate_window_start_queries.store(0, std::memory_order_release);
    scheduler_stats_.restart_ns.store(now_ns, std::memory_order_release);
    for (std::size_t slot = 0; slot < scheduler_stats::k_inflight_slots; ++slot) {
        scheduler_stats_.inflight_submit_ns[slot].store(0, std::memory_order_release);
    }
    scheduler_stats_.inflight_seq.store(0, std::memory_order_release);
}

void scoped_task_group::run(task_body body) {
    if (!implementation_ || !implementation_->pool || !implementation_->pool->arena) {
        body();
        return;
    }
    auto body_ptr = std::make_shared<task_body>(std::move(body));
    implementation_->pool->arena->execute([group_implementation = implementation_, body_ptr = std::move(body_ptr)] {
        group_implementation->group.run([body_ptr = std::move(body_ptr)] {
            (*body_ptr)();
        });
    });
}

void scoped_task_group::wait() {
    if (!implementation_) {
        return;
    }
    if (implementation_->pool && implementation_->pool->arena) {
        implementation_->pool->arena->execute([group_implementation = implementation_] {
            group_implementation->group.wait();
        });
    }
    implementation_.reset();
}

} // namespace bioseq::parallelism

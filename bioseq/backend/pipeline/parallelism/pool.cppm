/**
 * @file pool.cppm
 * @brief Runs pipeline work in a process-local oneTBB arena.
 *
 * Tasks share the configured concurrency limit. Tracked tasks also report
 * completion and cleanup to the PostgreSQL scheduler.
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

/// Holds the oneTBB arena behind the pool's private implementation.
struct pool_implementation {
    std::optional<otbb::task_arena> arena;
};

/// Keeps the arena alive while its task group has work to finish.
struct task_group_implementation {
    std::shared_ptr<pool_implementation> pool;
    otbb::task_group group;
};

} // namespace bioseq::parallelism::detail

export namespace bioseq::parallelism {

/// Borrows a caller-owned factory for worker contexts with reserved memory.
struct worker_context_factory {
    /// Context owner, kept at the same address until all work has joined.
    void* state = nullptr;
    /// Maximum number of worker contexts the caller has reserved memory for.
    std::size_t maximum_concurrency = 0u;
    /// Claim a worker context. May throw if its memory reservation fails.
    context::context& (*acquire)(void*) = nullptr;
};

/// Borrows a cleanup callback and its state from a tracked job.
struct tracked_noexcept_callback {
    /// Caller-owned state passed to the callback.
    void* argument{};
    /// Callback that must not propagate an exception.
    void (*invoke)(void*) noexcept {};
};

/// Cleanup callbacks registered for one tracked scheduler job.
struct tracked_job_resources {
    /// Request cancellation of queued work.
    tracked_noexcept_callback cancel{};
    /// Wait until the submitted work has stopped.
    tracked_noexcept_callback join{};
    /// Release task data after the work has joined.
    tracked_noexcept_callback backings{};
};

/// Identifies a scheduler slot and the job currently using it.
struct tracked_job_handle {
    /// Scheduler slot selected during enrollment.
    std::uint32_t slot_index{};
    /// Distinguishes this job from earlier jobs that used the same slot.
    std::uint64_t generation{};
};

/// Reports cleanup and errors without allocating an error object.
struct tracked_job_outcome {
    /// The scheduler finished cleaning up this job.
    bool is_drained{};
    /// The job was cancelled, failed, or missed a completion step.
    bool is_incomplete{};
    /// Worker or scheduler execution reported a failure.
    bool has_failure{};
    /// Reserved layout byte.
    std::uint8_t reserved{};
    /// PostgreSQL error code, or zero when none was supplied.
    std::uint32_t sqlstate{};
    /// Used bytes in the fixed message buffer.
    std::uint16_t message_length{};
    /// Error message copied from the worker or scheduler.
    std::array<char, 238> message{};
};

static_assert(std::is_trivially_copyable_v<tracked_job_outcome>);
static_assert(std::is_standard_layout_v<tracked_job_outcome>);

/// Scratch space for saving and restoring a worker's scheduler state.
struct tracked_worker_scope_storage {
    /// Storage owned by the caller and interpreted by the observer callbacks.
    alignas(std::max_align_t) std::array<std::byte, 64> bytes{};
};

/// Lets pool tasks report progress and cleanup to the scheduler.
struct tracked_job_observer {
    /// Scheduler state passed to every observer callback.
    void* context{};
    /// Register a job and return the slot and generation assigned to it.
    bool (*enroll)(void*, tracked_job_resources, tracked_job_handle*) noexcept {};
    /// Record that the job was scheduled.
    bool (*mark_scheduled)(void*, tracked_job_handle) noexcept {};
    /// Record that the worker finished.
    bool (*mark_finished)(void*, tracked_job_handle) noexcept {};
    /// Clean up the job and copy its completion status and any error.
    bool (*drain)(void*, tracked_job_handle, tracked_job_outcome*) noexcept {};
    /// Copy a worker error to the scheduler. This callback is optional.
    void (*handoff_error)(void*, tracked_job_handle, std::uint32_t, char const*, bool) noexcept {};
    /// Install worker-local scheduler state before invoking user work.
    void (*worker_enter)(void*, tracked_job_handle, tracked_worker_scope_storage*) noexcept {};
    /// Restore the worker's previous scheduler state after the task.
    void (*worker_leave)(void*, tracked_job_handle, tracked_worker_scope_storage*) noexcept {};

    /// Check that scheduler state and all required callbacks are present.
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

/// Borrows a task callback and the data it will use.
struct tracked_task_ref {
    /// Caller-owned data passed to the task.
    void* argument{};
    /// Run the task using its borrowed data.
    void (*invoke)(void*){};

    /// Check whether a task callback is present.
    [[nodiscard]] explicit operator bool() const noexcept {
        return invoke != nullptr;
    }
};

/**
 * State shared by the submitting thread and one worker. The caller must keep
 * this record and its borrowed task data alive at the same addresses until
 * joining and cleanup finish.
 *
 * Cancellation and completion use atomic byte flags. The task and observer
 * stay unchanged after enqueue, and the remaining status is read after join.
 * This record owns no heap memory.
 */
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
    /// The scheduler accepted the job and the arena queued it.
    submitted = 0u,
    /// The submission pointer, task callback, or observer was missing.
    invalid,
    /// The scheduler refused enrollment.
    enrollment_rejected,
    /// Preparing the arena or enqueueing the task failed.
    scheduler_rejected,
    /// The scheduler rejected the queued job's scheduled state.
    transition_rejected,
};

/// Owns a group of tasks and waits for them before releasing the arena.
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

  /// Queue a task, or run it here if this group has no arena.
  void run(task_body body);
  /// Join all submitted tasks and release the group implementation.
  void wait();

private:
  friend class global_pool;
  /// Take ownership of a task group that keeps its arena alive.
  explicit scoped_task_group(std::shared_ptr<detail::task_group_implementation> implementation)
      : implementation_(std::move(implementation)) {}

  std::shared_ptr<detail::task_group_implementation> implementation_;
};

/// Shares one configured oneTBB arena across this process's pipeline work.
class global_pool {
public:
  /// Return this process's pipeline pool.
  static global_pool &instance() noexcept {
    static global_pool pool;
    return pool;
  }

  /// Replace the arena and its concurrency settings under the lifecycle lock.
  void configure(const pool_config &config);
  /// Lazily create the default arena when no configuration is active.
  void ensure_configured() const;
  /// Release the pool's arena during normal shutdown.
  void shutdown() noexcept;
  /// Release the arena during process teardown without re-entering workers.
  void shutdown_at_exit() noexcept;
  /// Recreate the arena with a new configuration.
  void restart(const pool_config &config);

  /// Return the arena's concurrency limit, including a participating caller.
  [[nodiscard]] unsigned maximum_concurrency() const noexcept {
    return maximum_concurrency_;
  }
  /// Return the configured concurrency while active, or zero after shutdown.
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
  /// Return the pool's atomic scheduler counters.
  [[nodiscard]] scheduler_stats &get_scheduler_stats() noexcept { return scheduler_stats_; }

  /// Process [begin, end), waiting for any parallel ranges to finish.
  void parallel_for(std::size_t begin, std::size_t end, std::size_t grain,
                    const range_body &body) const;
  /// Process the range with an installed context on each participating thread.
  void parallel_for_with_context(std::size_t begin, std::size_t end,
                                 std::size_t grain, const range_body &body);
  /// Process the range using contexts from the caller's memory-backed factory.
  void parallel_for_with_context(std::size_t begin, std::size_t end,
                                 std::size_t grain, const range_body &body,
                                 worker_context_factory factory);
  /// Run two tracked range tasks when parallel, and join both before returning.
  void parallel_for_with_context_tracked(std::size_t begin, std::size_t end,
                                         std::size_t grain,
                                         const range_body &body,
                                         tracked_job_observer observer);
  /// Run tracked range tasks with contexts reserved by the caller.
  void parallel_for_with_context_tracked(std::size_t begin, std::size_t end,
                                         std::size_t grain,
                                         const range_body &body,
                                         tracked_job_observer observer,
                                         worker_context_factory factory);
  /// Register and queue a task whose caller-owned state remains at a fixed address.
  [[nodiscard]] tracked_submit_status
  submit_tracked_task_noexcept(tracked_task_submission *submission,
                               tracked_task_ref task,
                               tracked_job_observer observer) noexcept;
  /// Wait for a tracked task and finish its scheduler cleanup without throwing.
  [[nodiscard]] tracked_job_outcome join_and_drain_tracked_task_noexcept(
      tracked_task_submission *submission) noexcept;

  /**
   * Run a callback on an arena worker and wait for its result. The calling
   * thread only waits, so this can check behavior on a worker thread.
   *
   * Prepare any PostgreSQL-backed data before submitting the callback.
   * Return the callback result, or false if it is missing or scheduling fails.
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

  /// With an observer, track a parallel sort as one job and wait for it to finish.
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
              /* Comparisons must use data already available to workers.
               * Nested TBB tasks do not inherit the materialization trace
               * sink, so the comparator must not load PostgreSQL-backed data. */
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

  /// Create a task group that keeps the current arena alive.
  [[nodiscard]] scoped_task_group make_task_group();

  /// Identifies a batch's timestamp slot and its contribution to query counts.
  struct batch_ticket {
      /// Timestamp slot selected for this batch.
      std::size_t slot = 0;
      /// Queries represented by this batch.
      std::int64_t queries = 0;
  };
  /// Record a newly scheduled batch and return its completion ticket.
  [[nodiscard]] batch_ticket begin_batch(std::int64_t query_count) noexcept;
  /// Count a successful batch and remove it from the in-flight counters.
  void end_batch(batch_ticket ticket) noexcept;
  /// Remove a batch from the in-flight counters without counting success.
  void cancel_batch(batch_ticket ticket) noexcept;
  /// Clear scheduler counters and start a rate window at the supplied time.
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

/// Return oneTBB's default concurrency, with a minimum of one.
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
  // The private constructor makes this the mutable process singleton,
  // even when the caller holds a const reference.
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

    // Each participating thread acquires one context through this ETS map.
    // TBB joins the work before an exception escapes, so the caller can then
    // release the contexts and their memory reservations.
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

/// Ask the worker to skip this task if it has not started.
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

/// Record that cleanup reached the step for releasing task data.
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

/// Record a worker error and pass its text to the observer when available.
void handoff_tracked_pool_failure(tracked_task_submission *submission,
                                  char const *message) noexcept {
  submission->worker_failed = 1u;
  if (submission->observer.handoff_error != nullptr) {
    submission->observer.handoff_error(submission->observer.context,
                                       submission->handle, 0u, message, true);
  }
}

/// Run an uncancelled task with scheduler state installed and capture errors.
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

  // These two tasks borrow records on this stack. Join and clean up both
  // before reporting an error, so the records and context factory stay alive
  // through cancellation or a failure in context acquisition or range work.
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
    // Use the caller's reserved worker context so allocations count against
    // this attempt's budget, even if the observer installed another context.
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

/**
 * @file async_ownership.cppm
 * Track asynchronous sequence jobs and release their resources during shutdown.
 */
module;

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

#include <bioseq/pg_compat.h>
extern "C" {
#include "storage/shmem.h"
#include "utils/datum.h"
#include "utils/expandeddatum.h"
}

export module bioseq.postgres.types.compact_sequence:packed_output_async_ownership;

import :packed_output_materialization_trace;
import bioseq.parallelism.pool;

/* Allocate job storage and prepare cleanup callbacks before scheduling work.
 * State changes use that storage and callbacks that cannot throw. */
/// Whether the executor accepts work, is stopping it, or has finished cleanup.
export enum class slice_async_owner_phase : std::uint8_t {
  accepting = 0u,
  quiescing,
  quiesced,
  terminal,
};

/// Maximum number of active slice jobs.
export inline constexpr std::uint32_t slice_async_max_live_jobs = 2u;

/// Progress of one job from enrollment through cleanup, before its slot is reused.
export enum class slice_async_job_state : std::uint8_t {
  unused = 0u,
  enrolled,
  scheduled,
  finished,
  joined,
  merged,
  released,
};

/// Whether shutdown followed a complete or interrupted execution.
export enum class slice_shutdown_disposition : std::uint8_t {
  complete = 0u,
  incomplete,
};

/// Slot index and generation used to reject completions from an earlier use.
export struct slice_async_completion_handle {
  std::uint32_t slot_index;
  std::uint64_t generation;
};

/// Callback and argument prepared before cleanup. The callback must not throw.
export struct slice_noexcept_callback {
  void *argument;
  void (*invoke)(void *) noexcept;
};

/// Cancellation callback stored with an asynchronous job.
export using preconstructed_cancel_handle = slice_noexcept_callback;
/// Callback that waits for the job to stop using its resources.
export using preconstructed_join_handle = slice_noexcept_callback;
/// Callback that releases the input buffers and other resources held by a job.
export using retained_job_backings = slice_noexcept_callback;

/// Fixed-size worker error record that can be copied without allocating.
export struct fixed_job_error_handoff {
  std::uint8_t published;
  std::uint8_t incomplete;
  std::array<std::byte, 2> reserved;
  std::uint32_t sqlstate;
  std::uint16_t message_length;
  std::array<char, 238> message;
};

static_assert(std::is_trivially_copyable_v<fixed_job_error_handoff>);
static_assert(std::is_standard_layout_v<fixed_job_error_handoff>);

/// Prepared callbacks transferred to the owner when a job is enrolled.
export struct slice_async_job_resources {
  preconstructed_cancel_handle cancel;
  preconstructed_join_handle join;
  retained_job_backings backings;
};

/// Executor-owned state, counters, and cleanup callbacks for one job slot.
export struct slice_async_job_slot {
  std::uint64_t generation;
  /* Only the executor changes the job state. Workers write their counters
   * and error record, then return the unchanged completion handle through
   * the scheduler. */
  slice_async_job_state state;
  slice_dsm_counter *counters;
  std::uint32_t counter_count;
  slice_counter_sink local_sink;
  fixed_job_error_handoff error;
  preconstructed_cancel_handle cancel;
  preconstructed_join_handle join;
  retained_job_backings backings;
  slice_trace_attachment *attachment;
};

static_assert(std::is_trivially_copyable_v<slice_async_job_slot>);
static_assert(std::is_standard_layout_v<slice_async_job_slot>);

/// Keep job resources and tracing alive until all work has stopped.
export struct slice_async_owner_guard {
  MemoryContextCallback callback;
  slice_materialization_runtime *runtime;
  slice_trace_attachment *attachment;
  slice_async_job_slot *jobs;
  std::uint32_t job_capacity;
  std::uint32_t enrolled_jobs;
  std::uint64_t generation;
  void const *owner_thread_identity;
  slice_async_owner_phase phase;
  bool local_state_published;
  bool terminal_requested;
  bool slot_generation_overflow;
  slice_noexcept_callback publish_complete;
  slice_noexcept_callback publish_incomplete;
};

static_assert(std::is_standard_layout_v<slice_async_owner_guard>);

namespace {

inline std::uint64_t next_slice_async_owner_generation = 1u;

inline void invoke_slice_callback(slice_noexcept_callback callback) noexcept {
  if (callback.invoke != nullptr) {
    callback.invoke(callback.argument);
  }
}

inline void reset_error_handoff(fixed_job_error_handoff &error) noexcept {
  error.published = 0u;
  error.incomplete = 0u;
  error.reserved.fill(std::byte{0});
  error.sqlstate = 0u;
  error.message_length = 0u;
  error.message.fill('\0');
}

[[nodiscard]] inline bool
handle_matches(slice_async_owner_guard const *guard,
               slice_async_completion_handle handle) noexcept {
  return guard != nullptr && guard->jobs != nullptr &&
         handle.slot_index < guard->job_capacity &&
         guard->jobs[handle.slot_index].generation == handle.generation;
}

/// Check every worker counter before adding a completed job to the totals.
inline void merge_job_counters_noexcept(slice_async_owner_guard *guard,
                                        slice_async_job_slot &job) noexcept {
  if (guard->runtime == nullptr ||
      guard->runtime->cumulative_sink.entries == nullptr ||
      job.counters == nullptr ||
      job.counter_count != guard->runtime->cumulative_sink.entry_count) {
    if (guard->runtime != nullptr) {
      guard->runtime->invalid = true;
    }
    return;
  }
  /* Check every source and destination record before adding any counters.
   * If one is malformed, mark the trace invalid and keep its totals unchanged. */
  for (std::uint32_t index = 0u; index < job.counter_count; ++index) {
    if (job.counters[index].overflowed > 1u ||
        !slice_dsm_counter_reserved_valid(job.counters[index]) ||
        guard->runtime->cumulative_sink.entries[index].overflowed > 1u ||
        !slice_dsm_counter_reserved_valid(
            guard->runtime->cumulative_sink.entries[index])) {
      guard->runtime->invalid = true;
      guard->runtime->overflowed = true;
      return;
    }
  }
  for (std::uint32_t index = 0u; index < job.counter_count; ++index) {
    merge_slice_dsm_counter_noexcept(
        guard->runtime->cumulative_sink.entries[index], job.counters[index]);
    guard->runtime->overflowed =
        guard->runtime->overflowed ||
        guard->runtime->cumulative_sink.entries[index].overflowed != 0u;
  }
}

/// Clear a released slot and advance its generation before reuse.
inline void rearm_job_noexcept(slice_async_owner_guard *guard,
                               slice_async_job_slot &job) noexcept {
  if (job.state != slice_async_job_state::released) {
    return;
  }
  for (std::uint32_t index = 0u; index < job.counter_count; ++index) {
    reset_slice_dsm_counter_noexcept(job.counters[index]);
  }
  reset_error_handoff(job.error);
  job.cancel = {};
  job.join = {};
  job.backings = {};
  job.attachment = nullptr;
  if (guard->enrolled_jobs != 0u) {
    --guard->enrolled_jobs;
  }
  if (job.generation == (std::numeric_limits<std::uint64_t>::max)()) {
    guard->slot_generation_overflow = true;
    if (guard->runtime != nullptr) {
      guard->runtime->overflowed = true;
    }
    return;
  }
  ++job.generation;
  job.state = slice_async_job_state::unused;
}

} // namespace

/* Install this scope when the scheduler enters a worker job. Each job
 * writes to its own counters, so workers do not race the executor's totals. */
/// Return this job's tracing scope, or an empty scope for a stale handle.
export [[nodiscard]] inline slice_materialization_scope
slice_async_job_scope_noexcept(slice_async_owner_guard *guard,
                               slice_async_completion_handle handle) noexcept {
  if (!handle_matches(guard, handle)) {
    return {};
  }
  auto &job = guard->jobs[handle.slot_index];
  return {
      .attachment = job.attachment,
      .boundary_id = 0u,
      .producer_value_id = 0u,
      .mechanism = materialization_mechanism::backing_copy,
      .reason = materialization_reason::executor_copy,
      .local_sink = std::addressof(job.local_sink),
  };
}

/* An upper executor such as BatchSort can receive a traced expanded value
 * without a direct pointer to its job owner. Recover that owner only on its
 * original backend thread and while its generation is still live. */
/// Borrow an owner that is still accepting work on the current backend thread.
export [[nodiscard]] inline slice_async_owner_guard *
borrow_slice_async_owner_noexcept(
    slice_trace_attachment const *attachment) noexcept {
  if (!slice_trace_attachment_live(attachment) ||
      attachment->async_owner == nullptr) {
    return nullptr;
  }
  auto *owner = attachment->async_owner;
  if (owner->generation == 0u ||
      owner->generation != attachment->async_owner_generation ||
      owner->attachment != attachment ||
      owner->owner_thread_identity !=
          current_slice_async_owner_thread_identity_noexcept() ||
      owner->phase != slice_async_owner_phase::accepting) {
    return nullptr;
  }
  return owner;
}

/// Give an unused job slot its cleanup callbacks and return a completion handle.
export [[nodiscard]] inline bool enroll_slice_async_job_noexcept(
    slice_async_owner_guard *guard, slice_async_job_resources resources,
    slice_async_completion_handle *handle) noexcept {
  if (guard == nullptr || handle == nullptr ||
      guard->phase != slice_async_owner_phase::accepting ||
      guard->slot_generation_overflow) {
    return false;
  }
  for (std::uint32_t index = 0u; index < guard->job_capacity; ++index) {
    auto &job = guard->jobs[index];
    if (job.state != slice_async_job_state::unused) {
      continue;
    }
    for (std::uint32_t counter = 0u; counter < job.counter_count; ++counter) {
      reset_slice_dsm_counter_noexcept(job.counters[counter]);
    }
    reset_error_handoff(job.error);
    job.cancel = resources.cancel;
    job.join = resources.join;
    job.backings = resources.backings;
    job.attachment = guard->attachment;
    retain_slice_trace_attachment(job.attachment);
    job.state = slice_async_job_state::enrolled;
    ++guard->enrolled_jobs;
    *handle = {.slot_index = index, .generation = job.generation};
    return true;
  }
  return false;
}

/// Mark an enrolled job as accepted by the scheduler.
export [[nodiscard]] inline bool mark_slice_async_job_scheduled_noexcept(
    slice_async_owner_guard *guard,
    slice_async_completion_handle handle) noexcept {
  if (!handle_matches(guard, handle)) {
    return false;
  }
  auto &job = guard->jobs[handle.slot_index];
  if (job.state != slice_async_job_state::enrolled) {
    return false;
  }
  job.state = slice_async_job_state::scheduled;
  return true;
}

/// Copy a worker error into its fixed-size slot, truncating a long message.
export inline void handoff_slice_async_job_error_noexcept(
    slice_async_owner_guard *guard, slice_async_completion_handle handle,
    std::uint32_t sqlstate, char const *message, bool incomplete) noexcept {
  if (!handle_matches(guard, handle)) {
    return;
  }
  auto &error = guard->jobs[handle.slot_index].error;
  error.sqlstate = sqlstate;
  error.incomplete = incomplete ? 1u : 0u;
  const std::size_t length =
      message == nullptr ? 0u
                         : std::min<std::size_t>(std::strlen(message),
                                                 error.message.size() - 1u);
  if (length != 0u) {
    std::memcpy(error.message.data(), message, length);
  }
  error.message[length] = '\0';
  error.message_length = static_cast<std::uint16_t>(length);
  std::atomic_ref<std::uint8_t>{error.published}.store(
      1u, std::memory_order_release);
}

/// Mark a scheduled job finished after the executor receives its completion.
export [[nodiscard]] inline bool mark_slice_async_job_finished_noexcept(
    slice_async_owner_guard *guard,
    slice_async_completion_handle handle) noexcept {
  /* The executor calls this after the scheduler reports completion.
   * Workers never access job.state. */
  if (!handle_matches(guard, handle)) {
    return false;
  }
  auto &job = guard->jobs[handle.slot_index];
  if (job.state != slice_async_job_state::scheduled) {
    return false;
  }
  job.state = slice_async_job_state::finished;
  return true;
}

/// Wait for a job, merge its completed counters, and release its resources.
/// Cancel unfinished work first. This cleanup does not allocate or throw.
export [[nodiscard]] inline bool drain_slice_async_job_noexcept(
    slice_async_owner_guard *guard, slice_async_completion_handle handle,
    bool *incomplete_out = nullptr,
    bioseq::parallelism::tracked_job_outcome *outcome_out = nullptr) noexcept {
  if (!handle_matches(guard, handle)) {
    return false;
  }
  auto &job = guard->jobs[handle.slot_index];
  bool forced_incomplete = false;
  if (job.state == slice_async_job_state::enrolled) {
    /* A job rejected by the scheduler still counts as an incomplete use. */
    forced_incomplete = true;
    job.state = slice_async_job_state::scheduled;
  }
  if (job.state == slice_async_job_state::scheduled) {
    /* Completion has not reached the executor, so stop and join this job.
     * Even successful cleanup leaves this execution incomplete. */
    forced_incomplete = true;
    invoke_slice_callback(job.cancel);
    invoke_slice_callback(job.join);
    job.state = slice_async_job_state::finished;
    job.state = slice_async_job_state::joined;
  }
  if (job.state == slice_async_job_state::finished) {
    invoke_slice_callback(job.join);
    job.state = slice_async_job_state::joined;
  }
  const bool error_published =
      std::atomic_ref<std::uint8_t>{job.error.published}.load(
          std::memory_order_acquire) != 0u;
  const bool incomplete =
      forced_incomplete || error_published || job.error.incomplete != 0u;
  if (outcome_out != nullptr) {
    outcome_out->is_incomplete = outcome_out->is_incomplete || incomplete;
    outcome_out->has_failure = outcome_out->has_failure || error_published;
    if (error_published) {
      outcome_out->sqlstate = job.error.sqlstate;
      outcome_out->message_length = job.error.message_length;
      std::memcpy(outcome_out->message.data(), job.error.message.data(),
                  job.error.message.size());
    }
  }
  if (job.state == slice_async_job_state::joined) {
    /* A cancelled or failed job may have written some counters before it
     * stopped. Add counters only after a complete result, or Actual Calls and
     * Bytes would include work that never finished. */
    if (!incomplete) {
      merge_job_counters_noexcept(guard, job);
    }
    job.state = slice_async_job_state::merged;
  }
  if (job.state == slice_async_job_state::merged) {
    invoke_slice_callback(job.backings);
    release_slice_trace_attachment(job.attachment);
    job.attachment = nullptr;
    job.state = slice_async_job_state::released;
  }
  if (job.state == slice_async_job_state::released) {
    rearm_job_noexcept(guard, job);
    if (incomplete_out != nullptr) {
      *incomplete_out =
          *incomplete_out || incomplete || guard->slot_generation_overflow;
    }
    return true;
  }
  return job.state == slice_async_job_state::unused;
}

namespace {

[[nodiscard]] inline slice_async_completion_handle
from_pool_handle(bioseq::parallelism::tracked_job_handle handle) noexcept {
  return {handle.slot_index, handle.generation};
}

[[nodiscard]] inline slice_noexcept_callback from_pool_callback(
    bioseq::parallelism::tracked_noexcept_callback callback) noexcept {
  return {callback.argument, callback.invoke};
}

[[nodiscard]] inline bool
pool_observer_enroll(void *context,
                     bioseq::parallelism::tracked_job_resources resources,
                     bioseq::parallelism::tracked_job_handle *handle) noexcept {
  if (handle == nullptr) {
    return false;
  }
  slice_async_completion_handle enrolled{};
  const bool accepted = enroll_slice_async_job_noexcept(
      static_cast<slice_async_owner_guard *>(context),
      {
          .cancel = from_pool_callback(resources.cancel),
          .join = from_pool_callback(resources.join),
          .backings = from_pool_callback(resources.backings),
      },
      std::addressof(enrolled));
  if (accepted) {
    *handle = {enrolled.slot_index, enrolled.generation};
  }
  return accepted;
}

[[nodiscard]] inline bool pool_observer_mark_scheduled(
    void *context, bioseq::parallelism::tracked_job_handle handle) noexcept {
  return mark_slice_async_job_scheduled_noexcept(
      static_cast<slice_async_owner_guard *>(context),
      from_pool_handle(handle));
}

[[nodiscard]] inline bool pool_observer_mark_finished(
    void *context, bioseq::parallelism::tracked_job_handle handle) noexcept {
  return mark_slice_async_job_finished_noexcept(
      static_cast<slice_async_owner_guard *>(context),
      from_pool_handle(handle));
}

[[nodiscard]] inline bool pool_observer_drain(
    void *context, bioseq::parallelism::tracked_job_handle handle,
    bioseq::parallelism::tracked_job_outcome *outcome) noexcept {
  bool incomplete = false;
  return drain_slice_async_job_noexcept(
      static_cast<slice_async_owner_guard *>(context), from_pool_handle(handle),
      std::addressof(incomplete), outcome);
}

inline void pool_observer_handoff_error(
    void *context, bioseq::parallelism::tracked_job_handle handle,
    std::uint32_t sqlstate, char const *message, bool incomplete) noexcept {
  handoff_slice_async_job_error_noexcept(
      static_cast<slice_async_owner_guard *>(context), from_pool_handle(handle),
      sqlstate, message, incomplete);
}

inline void pool_observer_worker_enter(
    void *context, bioseq::parallelism::tracked_job_handle handle,
    bioseq::parallelism::tracked_worker_scope_storage *storage) noexcept {
  static_assert(sizeof(slice_materialization_scope) <= sizeof(storage->bytes));
  static_assert(alignof(slice_materialization_scope) <=
                alignof(bioseq::parallelism::tracked_worker_scope_storage));
  if (storage == nullptr) {
    return;
  }
  const auto next = slice_async_job_scope_noexcept(
      static_cast<slice_async_owner_guard *>(context),
      from_pool_handle(handle));
  const auto previous = exchange_slice_materialization_scope(next);
  std::memcpy(storage->bytes.data(), std::addressof(previous),
              sizeof(previous));
}

inline void pool_observer_worker_leave(
    void *, bioseq::parallelism::tracked_job_handle,
    bioseq::parallelism::tracked_worker_scope_storage *storage) noexcept {
  if (storage == nullptr) {
    return;
  }
  slice_materialization_scope previous{};
  std::memcpy(std::addressof(previous), storage->bytes.data(),
              sizeof(previous));
  restore_slice_materialization_scope(previous);
}

} // namespace

/// Connect a live job owner to the pool's scheduling and cleanup callbacks.
export [[nodiscard]] inline bioseq::parallelism::tracked_job_observer
make_slice_async_pool_observer_noexcept(
    slice_async_owner_guard *owner) noexcept {
  if (owner == nullptr || owner->phase != slice_async_owner_phase::accepting ||
      owner->owner_thread_identity !=
          current_slice_async_owner_thread_identity_noexcept() ||
      owner->job_capacity == 0u) {
    return {};
  }
  return {
      .context = owner,
      .enroll = &pool_observer_enroll,
      .mark_scheduled = &pool_observer_mark_scheduled,
      .mark_finished = &pool_observer_mark_finished,
      .drain = &pool_observer_drain,
      .handoff_error = &pool_observer_handoff_error,
      .worker_enter = &pool_observer_worker_enter,
      .worker_leave = &pool_observer_worker_leave,
  };
}

/// Stop accepting jobs, finish their cleanup, and publish the shutdown result once.
export inline void quiesce_slice_async_owner_noexcept(
    slice_async_owner_guard *guard,
    slice_shutdown_disposition disposition) noexcept {
  if (guard == nullptr || guard->phase == slice_async_owner_phase::quiesced ||
      guard->phase == slice_async_owner_phase::terminal) {
    return;
  }
  guard->phase = slice_async_owner_phase::quiescing;
  bool incomplete = disposition == slice_shutdown_disposition::incomplete;
  for (std::uint32_t index = 0u; index < guard->job_capacity; ++index) {
    auto &job = guard->jobs[index];
    if (job.state == slice_async_job_state::unused) {
      continue;
    }
    const slice_async_completion_handle handle{index, job.generation};
    (void)drain_slice_async_job_noexcept(guard, handle,
                                         std::addressof(incomplete));
  }
  if (!guard->local_state_published) {
    invoke_slice_callback(incomplete ? guard->publish_incomplete
                                     : guard->publish_complete);
    guard->local_state_published = true;
  }
  guard->phase = slice_async_owner_phase::quiesced;
}

/// Stop all jobs, then detach their trace attachment and destroy the runtime.
export inline void finish_slice_async_owner_noexcept(
    slice_async_owner_guard *guard,
    slice_shutdown_disposition disposition) noexcept {
  if (guard == nullptr || guard->phase == slice_async_owner_phase::terminal) {
    return;
  }
  guard->terminal_requested = true;
  quiesce_slice_async_owner_noexcept(guard, disposition);
  auto *attachment = guard->attachment;
  guard->attachment = nullptr;
  auto *runtime = guard->runtime;
  guard->runtime = nullptr;
  if (attachment != nullptr && attachment->async_owner == guard &&
      attachment->async_owner_generation == guard->generation) {
    attachment->async_owner = nullptr;
    attachment->async_owner_generation = 0u;
  }
  invalidate_slice_trace_attachment_noexcept(attachment);
  release_slice_trace_attachment(attachment);
  destroy_slice_runtime_noexcept(runtime);
  guard->phase = slice_async_owner_phase::terminal;
}

/// Stop unfinished work when its PostgreSQL memory context resets.
export inline void slice_async_owner_reset(void *argument) noexcept {
  finish_slice_async_owner_noexcept(
      static_cast<slice_async_owner_guard *>(argument),
      slice_shutdown_disposition::incomplete);
}

/// Initialize preallocated job slots and link their owner to the trace attachment.
export [[nodiscard]] inline bool initialize_slice_async_owner_noexcept(
    slice_async_owner_guard *guard, slice_materialization_runtime *runtime,
    slice_trace_attachment *attachment, slice_async_job_slot *jobs,
    std::uint32_t job_capacity, slice_dsm_counter *job_counters,
    std::uint32_t counter_count) noexcept {
  if (guard == nullptr || runtime == nullptr || attachment == nullptr ||
      job_capacity > slice_async_max_live_jobs ||
      (job_capacity != 0u && jobs == nullptr) ||
      (job_capacity != 0u && counter_count != 0u && job_counters == nullptr) ||
      next_slice_async_owner_generation == 0u ||
      attachment->async_owner != nullptr) {
    return false;
  }
  std::memset(guard, 0, sizeof(*guard));
  guard->callback.func = slice_async_owner_reset;
  guard->callback.arg = guard;
  guard->runtime = runtime;
  guard->attachment = attachment;
  guard->jobs = jobs;
  guard->job_capacity = job_capacity;
  guard->generation = next_slice_async_owner_generation++;
  guard->owner_thread_identity =
      current_slice_async_owner_thread_identity_noexcept();
  guard->phase = slice_async_owner_phase::accepting;
  attachment->async_owner = guard;
  attachment->async_owner_generation = guard->generation;
  for (std::uint32_t slot_index = 0u; slot_index < job_capacity; ++slot_index) {
    auto &job = jobs[slot_index];
    job.generation = 0u;
    job.state = slice_async_job_state::unused;
    job.counters = counter_count == 0u
                       ? nullptr
                       : job_counters + static_cast<std::size_t>(slot_index) *
                                            counter_count;
    job.counter_count = counter_count;
    job.local_sink = {job.counters, job.counter_count};
    reset_error_handoff(job.error);
    job.cancel = {};
    job.join = {};
    job.backings = {};
    job.attachment = nullptr;
    for (std::uint32_t index = 0u; index < counter_count; ++index) {
      reset_slice_dsm_counter_noexcept(job.counters[index]);
    }
  }
  return true;
}

/// Accept jobs again for a rescan after every previous job has been cleaned up.
export [[nodiscard]] inline bool rearm_slice_async_owner_for_rescan_noexcept(
    slice_async_owner_guard *guard) noexcept {
  if (guard == nullptr || guard->phase != slice_async_owner_phase::quiesced ||
      guard->enrolled_jobs != 0u || guard->terminal_requested ||
      guard->slot_generation_overflow) {
    return false;
  }
  guard->local_state_published = false;
  guard->phase = slice_async_owner_phase::accepting;
  return true;
}

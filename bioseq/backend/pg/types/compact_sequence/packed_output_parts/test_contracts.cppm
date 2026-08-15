/**
 * @file test_contracts.cppm
 * @brief Deterministic trace, packing, and expanded-ownership contracts.
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

export module bioseq.postgres.types.compact_sequence:packed_output_test_contracts;

import :packed_output_materialization_trace;
import :packed_output_async_ownership;
import :packed_output_packing;
import :flat_abi;
import :source_abi;
import :tree_types;
import :tree_ops;
import bioseq.parallelism.pool;
import bioseq.storage.coordinates;
import bioseq.storage.lazy_sequence;
import bioseq.storage.layout;

namespace {

struct slice_async_lifecycle_test_publications {
  std::uint32_t complete{};
  std::uint32_t incomplete{};
};

inline void publish_slice_async_test_complete(void *argument) noexcept {
  ++static_cast<slice_async_lifecycle_test_publications *>(argument)->complete;
}

inline void publish_slice_async_test_incomplete(void *argument) noexcept {
  ++static_cast<slice_async_lifecycle_test_publications *>(argument)->incomplete;
}

[[nodiscard]] inline bool reject_slice_async_test_publication(
    void *, bioseq::parallelism::tracked_job_handle) noexcept {
  return false;
}

inline void run_slice_async_test_noop(void *) {}

struct slice_async_contention_test_task {
  std::atomic<std::uint32_t> *started;
  std::atomic<std::uint32_t> *cancel_observed;
  bioseq::parallelism::tracked_task_submission *submission;
};

inline void run_slice_async_contention_test(void *argument) {
  auto *task = static_cast<slice_async_contention_test_task *>(argument);
  task->started->fetch_add(1u, std::memory_order_release);
  task->started->notify_all();
  auto cancelled = std::atomic_ref<std::uint8_t>{
      task->submission->cancel_requested};
  while (cancelled.load(std::memory_order_acquire) == 0u) {
    /* The production cancel callback owns this byte.  An atomic poll keeps the
     * worker observably live until backend-thread Shutdown invokes that exact
     * callback; no test-only release can let it escape before quiesce. */
    std::atomic_signal_fence(std::memory_order_seq_cst);
  }
  task->cancel_observed->fetch_add(1u, std::memory_order_release);
  task->cancel_observed->notify_all();
}

[[nodiscard]] inline bool
run_slice_async_shutdown_end_reset_contention_test_noexcept() noexcept {
  const slice_materialization_boundary boundary{
      .boundary_id = 901u,
      .producer_value_id = 902u,
      .counter_index = 0u,
      .mechanism = materialization_mechanism::packed_full,
      .reason = materialization_reason::executor_copy,
      .reserved = {},
  };
  slice_dsm_counter cumulative{};
  slice_materialization_runtime runtime{};
  if (!initialize_slice_materialization_runtime_noexcept(
          std::addressof(runtime), std::addressof(boundary), 1u,
          std::addressof(cumulative), 1u, true, false)) {
    return false;
  }
  auto *attachment = make_slice_trace_attachment(std::addressof(runtime));
  slice_async_owner_guard guard{};
  slice_async_job_slot jobs[slice_async_max_live_jobs]{};
  slice_dsm_counter job_counters[slice_async_max_live_jobs]{};
  if (!initialize_slice_async_owner_noexcept(
          std::addressof(guard), std::addressof(runtime), attachment, jobs,
          slice_async_max_live_jobs, job_counters, 1u)) {
    invalidate_slice_trace_attachment_noexcept(attachment);
    release_slice_trace_attachment(attachment);
    return false;
  }

  slice_async_lifecycle_test_publications publications{};
  guard.publish_complete = {
      std::addressof(publications), &publish_slice_async_test_complete};
  guard.publish_incomplete = {
      std::addressof(publications), &publish_slice_async_test_incomplete};

  std::array<std::atomic<std::uint32_t>, slice_async_max_live_jobs> started{};
  std::array<std::atomic<std::uint32_t>, slice_async_max_live_jobs>
      cancel_observed{};
  for (auto &value : started) {
    value.store(0u, std::memory_order_relaxed);
  }
  for (auto &value : cancel_observed) {
    value.store(0u, std::memory_order_relaxed);
  }
  std::array<slice_async_contention_test_task, slice_async_max_live_jobs>
      tasks{};
  std::array<bioseq::parallelism::tracked_task_submission,
             slice_async_max_live_jobs>
      submissions{};
  std::array<bioseq::parallelism::tracked_submit_status,
             slice_async_max_live_jobs>
      statuses{};
  const auto observer =
      make_slice_async_pool_observer_noexcept(std::addressof(guard));
  auto &pool = bioseq::parallelism::global_pool::instance();
  tasks[0] = {std::addressof(started[0]),
              std::addressof(cancel_observed[0]),
              std::addressof(submissions[0])};
  statuses[0] = pool.submit_tracked_task_noexcept(
      std::addressof(submissions[0]),
      {std::addressof(tasks[0]), &run_slice_async_contention_test}, observer);
  const bool first_submitted = statuses[0]
      == bioseq::parallelism::tracked_submit_status::submitted;
  if (first_submitted) {
    std::uint32_t observed = started[0].load(std::memory_order_acquire);
    while (observed == 0u) {
      started[0].wait(0u, std::memory_order_acquire);
      observed = started[0].load(std::memory_order_acquire);
    }
  }

  tasks[1] = {std::addressof(started[1]),
              std::addressof(cancel_observed[1]),
              std::addressof(submissions[1])};
  statuses[1] = pool.submit_tracked_task_noexcept(
      std::addressof(submissions[1]),
      {std::addressof(tasks[1]), &run_slice_async_contention_test}, observer);
  const bool submitted = first_submitted && statuses[1]
      == bioseq::parallelism::tracked_submit_status::submitted;

  /* These are the exact executor lifecycle calls: Shutdown quiesces under
   * live/queued worker contention, End makes ownership terminal, and the
   * query-context callback is then harmlessly repeated. */
  quiesce_slice_async_owner_noexcept(
      std::addressof(guard), slice_shutdown_disposition::complete);
  const bool shutdown_valid = submitted
      && guard.phase == slice_async_owner_phase::quiesced
      && guard.enrolled_jobs == 0u && guard.local_state_published
      && publications.complete == 0u && publications.incomplete == 1u
      && cumulative.calls == 0u && cumulative.bytes == 0u
      && attachment->refcount == 1u
      && started[0].load(std::memory_order_acquire) == 1u
      && cancel_observed[0].load(std::memory_order_acquire) == 1u
      && std::all_of(
          submissions.begin(), submissions.end(),
          [](bioseq::parallelism::tracked_task_submission const &submission) {
            return submission.completion_published == 1u
                && submission.backing_released == 1u;
          })
      && std::all_of(
          std::begin(jobs), std::end(jobs),
          [](slice_async_job_slot const &job) {
            return job.state == slice_async_job_state::unused
                && job.generation == 1u && job.attachment == nullptr;
          });

  finish_slice_async_owner_noexcept(
      std::addressof(guard), slice_shutdown_disposition::complete);
  const bool end_valid = guard.phase == slice_async_owner_phase::terminal
      && guard.enrolled_jobs == 0u && guard.attachment == nullptr
      && guard.runtime == nullptr && publications.complete == 0u
      && publications.incomplete == 1u && runtime.boundaries == nullptr
      && runtime.cumulative_sink.entries == nullptr;
  slice_async_owner_reset(std::addressof(guard));
  const bool reset_valid = guard.phase == slice_async_owner_phase::terminal
      && publications.complete == 0u && publications.incomplete == 1u;
  return shutdown_valid && end_valid && reset_valid;
}

} // namespace

/* Integration seam for the pure fixed-record/state-machine invariants.  It
 * uses only stack-owned POD and never installs query-visible trace state. */
/// Exercise fixed-record tracing and asynchronous-owner state transitions.
export [[nodiscard]] inline bool slice_trace_self_test_noexcept() noexcept {
  bool valid = sizeof(slice_dsm_counter) == 64u;

  slice_dsm_counter overflow{};
  reset_slice_dsm_counter_noexcept(overflow);
  overflow.calls = (std::numeric_limits<std::uint64_t>::max)();
  update_slice_dsm_counter_noexcept(overflow, 1u, 1u, 1u, false, 9u, 1u);
  valid = valid &&
          overflow.calls == (std::numeric_limits<std::uint64_t>::max)() &&
          overflow.overflowed != 0u;

  slice_dsm_counter malformed{};
  malformed.reserved[3] = std::byte{1};
  merge_slice_dsm_counter_noexcept(overflow, malformed);
  valid = valid && overflow.overflowed != 0u &&
          slice_dsm_counter_reserved_valid(overflow);

  slice_dsm_counter cumulative{};
  const slice_materialization_boundary boundary{
      .boundary_id = 1u,
      .producer_value_id = 7u,
      .counter_index = 0u,
      .mechanism = materialization_mechanism::packed_full,
      .reason = materialization_reason::executor_copy,
      .reserved = {},
  };
  slice_materialization_runtime runtime{};
  valid = valid && initialize_slice_materialization_runtime_noexcept(
                       std::addressof(runtime), std::addressof(boundary), 1u,
                       std::addressof(cumulative), 1u, true, false);

  slice_trace_attachment attachment{
      .refcount = 32u,
      .generation = 11u,
      .runtime = std::addressof(runtime),
      .valid = true,
  };
  retain_slice_trace_attachment(std::addressof(attachment));
  valid = valid && attachment.refcount == 33u;
  release_slice_trace_attachment(std::addressof(attachment));
  valid = valid && attachment.refcount == 32u;

  slice_counter_sink local_sink{std::addressof(cumulative), 1u};
  const auto original_scope = get_current_slice_materialization_scope();
  valid = valid && original_scope.attachment == nullptr
      && original_scope.boundary_id == 0u
      && original_scope.producer_value_id == 0u
      && original_scope.mechanism
          == materialization_mechanism::backing_copy
      && original_scope.reason == materialization_reason::executor_copy
      && original_scope.local_sink == nullptr;
  const slice_materialization_scope outer{
      .attachment = std::addressof(attachment),
      .boundary_id = 1u,
      .producer_value_id = 7u,
      .mechanism = materialization_mechanism::packed_full,
      .reason = materialization_reason::executor_copy,
      .local_sink = std::addressof(local_sink),
  };
  const auto saved_outer = exchange_slice_materialization_scope(outer);
  const auto observed_outer = get_current_slice_materialization_scope();
  record_slice_materialization_success(
      std::addressof(attachment), 7u, materialization_mechanism::backing_copy,
      materialization_reason::client_output, 13u, 5u, 24u, true, 31u, 0u);
  valid = valid && cumulative.calls == 1u && cumulative.input_symbols == 13u &&
          cumulative.output_symbols == 5u && cumulative.bytes == 24u &&
          cumulative.reused_calls == 1u &&
          cumulative.peak_retained_bytes == 31u;
  const auto saved_inner = exchange_slice_materialization_scope({});
  restore_slice_materialization_scope(saved_inner);
  const auto restored_outer = get_current_slice_materialization_scope();
  restore_slice_materialization_scope(saved_outer);
  const auto scope_equal =
      [](slice_materialization_scope const &left,
         slice_materialization_scope const &right) noexcept {
        return left.attachment == right.attachment &&
               left.boundary_id == right.boundary_id &&
               left.producer_value_id == right.producer_value_id &&
               left.mechanism == right.mechanism &&
               left.reason == right.reason &&
               left.local_sink == right.local_sink;
      };
  valid =
      valid && scope_equal(observed_outer, outer) &&
      scope_equal(restored_outer, outer) &&
      scope_equal(get_current_slice_materialization_scope(), original_scope);
  reset_slice_dsm_counter_noexcept(cumulative);

  /* One producer may fan out into several independently-accounted consumers.
   * Exercise the production admission token rather than mutating counters as
   * a test shortcut: success publishes once, while ERROR/cancel abandonment
   * and a repeated terminal callback publish nothing. */
  constexpr std::uint32_t shared_producer_value_id = 77u;
  constexpr std::array<slice_materialization_boundary, 5>
      consumer_boundaries{{
          {101u, shared_producer_value_id, 0u,
           materialization_mechanism::text,
           materialization_reason::client_output, {}},
          {102u, shared_producer_value_id, 1u,
           materialization_mechanism::packed_full,
           materialization_reason::index_tuple, {}},
          {103u, shared_producer_value_id, 2u,
           materialization_mechanism::packed_full,
           materialization_reason::mmsearch_target, {}},
          {104u, shared_producer_value_id, 3u,
           materialization_mechanism::packed_region,
           materialization_reason::resident_algorithm, {}},
          {105u, shared_producer_value_id, 4u,
           materialization_mechanism::packed_full,
           materialization_reason::executor_copy, {}},
      }};
  std::array<slice_dsm_counter, consumer_boundaries.size()>
      consumer_counters{};
  slice_materialization_runtime consumer_runtime{};
  valid = valid && initialize_slice_materialization_runtime_noexcept(
                       std::addressof(consumer_runtime),
                       consumer_boundaries.data(),
                       static_cast<std::uint32_t>(consumer_boundaries.size()),
                       consumer_counters.data(),
                       static_cast<std::uint32_t>(consumer_counters.size()),
                       true, false);
  slice_trace_attachment consumer_attachment{
      .refcount = 1u,
      .generation = 12u,
      .runtime = std::addressof(consumer_runtime),
      .async_owner = nullptr,
      .async_owner_generation = 0u,
      .valid = true,
  };
  slice_counter_sink consumer_sink{
      consumer_counters.data(),
      static_cast<std::uint32_t>(consumer_counters.size())};
  constexpr std::array<std::uint64_t, consumer_boundaries.size()>
      initial_bytes{{11u, 22u, 33u, 44u, 55u}};
  for (std::size_t index = 0u; index < consumer_boundaries.size(); ++index) {
    const auto &consumer = consumer_boundaries[index];
    const slice_materialization_scope consumer_scope{
        .attachment = std::addressof(consumer_attachment),
        .boundary_id = consumer.boundary_id,
        .producer_value_id = consumer.producer_value_id,
        .mechanism = consumer.mechanism,
        .reason = consumer.reason,
        .local_sink = std::addressof(consumer_sink),
    };
    const auto saved_scope =
        exchange_slice_materialization_scope(consumer_scope);
    auto admission = admit_slice_materialization_noexcept(
        std::addressof(consumer_attachment), shared_producer_value_id,
        consumer.mechanism, consumer.reason);
    /* Publication remains attached to the admitted stable boundary even when
     * an intervening caller temporarily restores the ambient TLS scope. */
    const auto admitted_scope = exchange_slice_materialization_scope({});
    const bool published = publish_slice_materialization_success_noexcept(
        admission, 100u + index, 10u + index, initial_bytes[index], false,
        initial_bytes[index]);
    restore_slice_materialization_scope(admitted_scope);
    const bool duplicate_rejected =
        !publish_slice_materialization_success_noexcept(
            admission, 1u, 1u, 1u, false, 1u);
    restore_slice_materialization_scope(saved_scope);
    valid = valid && published && duplicate_rejected &&
            consumer.producer_value_id == shared_producer_value_id &&
            consumer_counters[index].calls == 1u &&
            consumer_counters[index].bytes == initial_bytes[index];
  }

  /* Table-AM index build/validation cannot attach a plan-owned boundary, so
   * it installs the classification-only scope used by FormIndexDatum and the
   * index callback.  Prove the generic EOH packed-full observer is redirected
   * from executor_copy to the unique index_tuple consumer and that restoration
   * leaves the caller's ambient scope byte-exact. */
  constexpr std::size_t index_consumer = 1u;
  const auto index_calls_before = consumer_counters[index_consumer].calls;
  const auto index_bytes_before = consumer_counters[index_consumer].bytes;
  const slice_materialization_scope index_tuple_scope{
      .attachment = nullptr,
      .boundary_id = 0u,
      .producer_value_id = 0u,
      .mechanism = materialization_mechanism::packed_full,
      .reason = materialization_reason::index_tuple,
      .local_sink = nullptr,
  };
  const auto saved_index_scope =
      exchange_slice_materialization_scope(index_tuple_scope);
  const auto index_observer = make_slice_payload_allocation_observer_noexcept(
      std::addressof(consumer_attachment), shared_producer_value_id,
      materialization_mechanism::packed_full,
      materialization_reason::executor_copy);
  const bool index_observed =
      bioseq::storage::publish_sequence_allocation(
          std::addressof(index_observer),
          bioseq::storage::sequence_payload_allocation_kind::flat_sequence,
          64u, 64u, 64u, 64u);
  restore_slice_materialization_scope(saved_index_scope);
  valid = valid && index_observed
      && consumer_counters[index_consumer].calls == index_calls_before + 1u
      && consumer_counters[index_consumer].bytes == index_bytes_before + 64u
      && scope_equal(
          get_current_slice_materialization_scope(), original_scope);

  const auto exercise_abandoned_admission =
      [&consumer_attachment, &consumer_boundaries, &consumer_counters,
       &consumer_sink](std::size_t index) noexcept {
        const auto &consumer = consumer_boundaries[index];
        const auto calls_before = consumer_counters[index].calls;
        const auto bytes_before = consumer_counters[index].bytes;
        const slice_materialization_scope consumer_scope{
            .attachment = std::addressof(consumer_attachment),
            .boundary_id = consumer.boundary_id,
            .producer_value_id = consumer.producer_value_id,
            .mechanism = consumer.mechanism,
            .reason = consumer.reason,
            .local_sink = std::addressof(consumer_sink),
        };
        const auto saved_scope =
            exchange_slice_materialization_scope(consumer_scope);
        auto admission = admit_slice_materialization_noexcept(
            std::addressof(consumer_attachment), shared_producer_value_id,
            consumer.mechanism, consumer.reason);
        const bool abandoned = abandon_slice_materialization_noexcept(admission);
        const bool repeated_abandonment_rejected =
            !abandon_slice_materialization_noexcept(admission);
        const bool late_publication_rejected =
            !publish_slice_materialization_success_noexcept(
                admission, 999u, 999u, 999u, false, 999u);
        restore_slice_materialization_scope(saved_scope);
        return abandoned && repeated_abandonment_rejected &&
               late_publication_rejected &&
               consumer_counters[index].calls == calls_before &&
               consumer_counters[index].bytes == bytes_before;
      };
  /* Separate attempts model an injected allocator ERROR and cancellation. */
  valid = valid && exercise_abandoned_admission(1u) &&
          exercise_abandoned_admission(2u);

  /* A rescan contributes only its fresh delta to the same stable boundary. */
  const auto &rescan_consumer = consumer_boundaries.front();
  const slice_materialization_scope rescan_scope{
      .attachment = std::addressof(consumer_attachment),
      .boundary_id = rescan_consumer.boundary_id,
      .producer_value_id = rescan_consumer.producer_value_id,
      .mechanism = rescan_consumer.mechanism,
      .reason = rescan_consumer.reason,
      .local_sink = std::addressof(consumer_sink),
  };
  const auto saved_rescan_scope =
      exchange_slice_materialization_scope(rescan_scope);
  auto rescan_admission = admit_slice_materialization_noexcept(
      std::addressof(consumer_attachment), shared_producer_value_id,
      rescan_consumer.mechanism, rescan_consumer.reason);
  const bool rescan_published = publish_slice_materialization_success_noexcept(
      rescan_admission, 7u, 7u, 7u, true, 7u);
  restore_slice_materialization_scope(saved_rescan_scope);
  valid = valid && rescan_published && consumer_counters[0].calls == 2u &&
          consumer_counters[0].bytes == initial_bytes[0] + 7u &&
          consumer_counters[0].reused_calls == 1u;

  slice_async_owner_guard guard{};
  slice_async_job_slot jobs[2]{};
  slice_dsm_counter job_counters[2]{};
  valid = valid && initialize_slice_async_owner_noexcept(
                       std::addressof(guard), std::addressof(runtime),
                       std::addressof(attachment), jobs, 2u, job_counters, 1u);
  slice_async_completion_handle capacity_handles[2]{};
  const bool first_capacity_enrolled = enroll_slice_async_job_noexcept(
      std::addressof(guard), {}, std::addressof(capacity_handles[0]));
  const bool second_capacity_enrolled = enroll_slice_async_job_noexcept(
      std::addressof(guard), {}, std::addressof(capacity_handles[1]));
  valid = valid && first_capacity_enrolled && second_capacity_enrolled &&
          capacity_handles[0].slot_index != capacity_handles[1].slot_index;
  if (first_capacity_enrolled && second_capacity_enrolled) {
    for (auto const handle : capacity_handles) {
      valid = valid &&
              mark_slice_async_job_scheduled_noexcept(std::addressof(guard),
                                                      handle) &&
              mark_slice_async_job_finished_noexcept(std::addressof(guard),
                                                     handle) &&
              drain_slice_async_job_noexcept(std::addressof(guard), handle);
    }
    valid = valid && guard.enrolled_jobs == 0u && attachment.refcount == 32u;
  }

  struct async_trace_probe {
    std::atomic<std::uint8_t> *release;
    bioseq::storage::sequence_payload_allocation_observer observer;
    std::uint32_t iterations;
  };
  constexpr std::uint32_t probe_iterations = 4096u;
  std::atomic<std::uint8_t> release_async_probes{0u};
  const auto async_payload_observer =
      make_slice_payload_allocation_observer_noexcept(
          std::addressof(attachment), 7u,
          materialization_mechanism::packed_full,
          materialization_reason::executor_copy);
  std::array<async_trace_probe, 2> probe_contexts{{
      {std::addressof(release_async_probes), async_payload_observer,
       probe_iterations},
      {std::addressof(release_async_probes), async_payload_observer,
       probe_iterations},
  }};
  std::array<bioseq::parallelism::tracked_task_submission, 2>
      probe_submissions{};
  const auto probe_task = +[](void *argument) {
    auto *probe = static_cast<async_trace_probe *>(argument);
    std::uint8_t released = probe->release->load(std::memory_order_acquire);
    while (released == 0u) {
      probe->release->wait(0u, std::memory_order_acquire);
      released = probe->release->load(std::memory_order_acquire);
    }
    for (std::uint32_t iteration = 0u; iteration < probe->iterations;
         ++iteration) {
      (void)bioseq::storage::publish_sequence_allocation(
          std::addressof(probe->observer),
          bioseq::storage::sequence_payload_allocation_kind::flat_sequence,
          1u, 1u, 1u, 1u);
    }
  };
  const auto observer =
      make_slice_async_pool_observer_noexcept(std::addressof(guard));
  std::array<bioseq::parallelism::tracked_submit_status, 2> probe_statuses{};
  auto &pool = bioseq::parallelism::global_pool::instance();
  for (std::size_t index = 0u; index < probe_submissions.size(); ++index) {
    probe_statuses[index] = pool.submit_tracked_task_noexcept(
        std::addressof(probe_submissions[index]),
        {std::addressof(probe_contexts[index]), probe_task}, observer);
  }
  valid = valid &&
          static_cast<bool>(async_payload_observer) &&
          probe_statuses[0] ==
              bioseq::parallelism::tracked_submit_status::submitted &&
          probe_statuses[1] ==
              bioseq::parallelism::tracked_submit_status::submitted &&
          guard.enrolled_jobs == 2u;
  release_async_probes.store(1u, std::memory_order_release);
  release_async_probes.notify_all();
  for (auto &submission : probe_submissions) {
    const auto outcome =
        pool.join_and_drain_tracked_task_noexcept(std::addressof(submission));
    valid = valid && outcome.is_drained && !outcome.is_incomplete &&
            !outcome.has_failure;
  }
  const std::uint64_t expected_probe_calls =
      static_cast<std::uint64_t>(probe_iterations) * 2u;
  valid = valid && cumulative.calls == expected_probe_calls &&
          cumulative.input_symbols == expected_probe_calls &&
          cumulative.output_symbols == expected_probe_calls &&
          cumulative.bytes == expected_probe_calls &&
          cumulative.peak_retained_bytes == 1u && guard.enrolled_jobs == 0u &&
          attachment.refcount == 32u;
  reset_slice_dsm_counter_noexcept(cumulative);

  slice_async_completion_handle first_handle{};
  for (std::uint32_t use = 0u; use < 128u; ++use) {
    slice_async_completion_handle handle{};
    const bool enrolled = enroll_slice_async_job_noexcept(
        std::addressof(guard), {}, std::addressof(handle));
    if (use == 0u) {
      first_handle = handle;
    }
    valid =
        valid && enrolled &&
        mark_slice_async_job_scheduled_noexcept(std::addressof(guard),
                                                handle) &&
        mark_slice_async_job_finished_noexcept(std::addressof(guard), handle);
    if (!enrolled) {
      break;
    }
    update_slice_dsm_counter_noexcept(guard.jobs[handle.slot_index].counters[0],
                                      1u, 1u, 8u, false, 8u, 0u);
    valid = valid &&
            drain_slice_async_job_noexcept(std::addressof(guard), handle) &&
            guard.enrolled_jobs == 0u;
    if ((use + 1u) % 32u == 0u) {
      quiesce_slice_async_owner_noexcept(
          std::addressof(guard), slice_shutdown_disposition::complete);
      valid = valid && guard.phase == slice_async_owner_phase::quiesced
          && guard.enrolled_jobs == 0u
          && rearm_slice_async_owner_for_rescan_noexcept(
              std::addressof(guard))
          && guard.phase == slice_async_owner_phase::accepting;
    }
  }
  valid = valid && cumulative.calls == 128u && attachment.refcount == 32u &&
          !mark_slice_async_job_scheduled_noexcept(std::addressof(guard),
                                                   first_handle) &&
          !guard.slot_generation_overflow;
  slice_async_completion_handle cancelled_handle{};
  bool cancelled_incomplete = false;
  const bool cancelled_enrolled = enroll_slice_async_job_noexcept(
      std::addressof(guard), {}, std::addressof(cancelled_handle));
  const bool cancelled_scheduled =
      cancelled_enrolled && mark_slice_async_job_scheduled_noexcept(
                                std::addressof(guard), cancelled_handle);
  if (cancelled_scheduled) {
    update_slice_dsm_counter_noexcept(
        guard.jobs[cancelled_handle.slot_index].counters[0],
        9u, 9u, 72u, false, 72u, 0u);
  }
  const bool cancelled_drained =
      cancelled_scheduled && drain_slice_async_job_noexcept(
                                 std::addressof(guard), cancelled_handle,
                                 std::addressof(cancelled_incomplete));
  valid = valid && cancelled_drained && cancelled_incomplete &&
          guard.enrolled_jobs == 0u && attachment.refcount == 32u &&
          cumulative.calls == 128u && cumulative.bytes == 1024u;

  slice_async_completion_handle failed_handle{};
  bool failed_incomplete = false;
  bioseq::parallelism::tracked_job_outcome failed_outcome{};
  const bool failed_enrolled = enroll_slice_async_job_noexcept(
      std::addressof(guard), {}, std::addressof(failed_handle));
  const bool failed_scheduled =
      failed_enrolled && mark_slice_async_job_scheduled_noexcept(
                             std::addressof(guard), failed_handle);
  if (failed_scheduled) {
    update_slice_dsm_counter_noexcept(
        guard.jobs[failed_handle.slot_index].counters[0],
        10u, 10u, 80u, false, 80u, 0u);
    handoff_slice_async_job_error_noexcept(
        std::addressof(guard), failed_handle, 123u,
        "injected trace accounting failure", true);
  }
  const bool failed_finished =
      failed_scheduled && mark_slice_async_job_finished_noexcept(
                              std::addressof(guard), failed_handle);
  const bool failed_drained =
      failed_finished && drain_slice_async_job_noexcept(
                             std::addressof(guard), failed_handle,
                             std::addressof(failed_incomplete),
                             std::addressof(failed_outcome));
  valid = valid && failed_drained && failed_incomplete &&
          failed_outcome.is_incomplete && failed_outcome.has_failure &&
          failed_outcome.sqlstate == 123u && guard.enrolled_jobs == 0u &&
          attachment.refcount == 32u && cumulative.calls == 128u &&
          cumulative.bytes == 1024u;

  /* The scheduler has accepted/enqueued this task, but publication of the
   * scheduled transition is injected to fail.  Drain must treat the enrolled
   * record as incomplete, release all retained state, and make its generation
   * stale without merging any tentative counter prefix. */
  auto rejecting_observer =
      make_slice_async_pool_observer_noexcept(std::addressof(guard));
  rejecting_observer.mark_scheduled =
      &reject_slice_async_test_publication;
  bioseq::parallelism::tracked_task_submission publication_submission{};
  const auto publication_status =
      pool.submit_tracked_task_noexcept(
          std::addressof(publication_submission),
          {nullptr, &run_slice_async_test_noop}, rejecting_observer);
  const auto stale_publication_handle = slice_async_completion_handle{
      publication_submission.handle.slot_index,
      publication_submission.handle.generation};
  const bool publication_slot_valid = publication_submission.enrolled != 0u
      && stale_publication_handle.slot_index < guard.job_capacity;
  const auto publication_outcome =
      pool.join_and_drain_tracked_task_noexcept(
          std::addressof(publication_submission));
  const bool publication_generation_advanced = publication_slot_valid
      && jobs[stale_publication_handle.slot_index].generation
          == stale_publication_handle.generation + 1u;
  valid = valid
      && publication_status
          == bioseq::parallelism::tracked_submit_status::transition_rejected
      && publication_outcome.is_drained && publication_outcome.is_incomplete
      && !publication_outcome.has_failure
      && publication_submission.backing_released == 1u
      && guard.enrolled_jobs == 0u && attachment.refcount == 32u
      && cumulative.calls == 128u && cumulative.bytes == 1024u
      && publication_generation_advanced;

  /* Reuse the exact slot, then deliver both an early-generation completion
   * and error.  Neither stale publication may mutate the current use. */
  slice_async_completion_handle reused_handle{};
  const bool reused_enrolled = enroll_slice_async_job_noexcept(
      std::addressof(guard), {}, std::addressof(reused_handle));
  const bool reused_slot_valid = reused_enrolled
      && reused_handle.slot_index < guard.job_capacity;
  const bool stale_finished_rejected = publication_slot_valid
      && !mark_slice_async_job_finished_noexcept(
          std::addressof(guard), stale_publication_handle);
  if (publication_slot_valid) {
    handoff_slice_async_job_error_noexcept(
        std::addressof(guard), stale_publication_handle, 999u,
        "stale early-generation error", true);
  }
  const bool stale_publications_ignored = reused_slot_valid
      && publication_slot_valid
      && reused_handle.slot_index == stale_publication_handle.slot_index
      && reused_handle.generation == stale_publication_handle.generation + 1u
      && stale_finished_rejected
      && jobs[reused_handle.slot_index].state
          == slice_async_job_state::enrolled
      && jobs[reused_handle.slot_index].error.published == 0u
      && jobs[reused_handle.slot_index].error.sqlstate == 0u;
  if (reused_slot_valid) {
    update_slice_dsm_counter_noexcept(
        guard.jobs[reused_handle.slot_index].counters[0],
        2u, 2u, 16u, false, 16u, 0u);
  }
  const bool reused_scheduled = reused_slot_valid
      && mark_slice_async_job_scheduled_noexcept(
          std::addressof(guard), reused_handle);
  const bool reused_finished = reused_scheduled
      && mark_slice_async_job_finished_noexcept(
          std::addressof(guard), reused_handle);
  const bool reused_drained = reused_finished
      && drain_slice_async_job_noexcept(
          std::addressof(guard), reused_handle);
  valid = valid && stale_publications_ignored && reused_drained
      && guard.enrolled_jobs == 0u && attachment.refcount == 32u
      && cumulative.calls == 129u && cumulative.bytes == 1040u;

  /* A slot generation must fail closed instead of wrapping to zero.  The
   * completed delta is merged once, cleanup still releases the job, and every
   * subsequent enrollment is rejected while the saturated identity remains
   * distinguishable from a fresh slot. */
  slice_async_job_slot *overflow_job = reused_slot_valid
      ? std::addressof(jobs[reused_handle.slot_index]) : nullptr;
  if (overflow_job != nullptr) {
    overflow_job->generation = (std::numeric_limits<std::uint64_t>::max)();
  }
  slice_async_completion_handle overflow_handle{};
  const bool overflow_enrolled = overflow_job != nullptr
      && enroll_slice_async_job_noexcept(
          std::addressof(guard), {}, std::addressof(overflow_handle));
  const bool overflow_slot_valid = overflow_enrolled
      && overflow_handle.slot_index < guard.job_capacity;
  if (overflow_slot_valid) {
    update_slice_dsm_counter_noexcept(
        guard.jobs[overflow_handle.slot_index].counters[0],
        3u, 3u, 24u, false, 24u, 0u);
  }
  const bool overflow_scheduled = overflow_slot_valid
      && mark_slice_async_job_scheduled_noexcept(
          std::addressof(guard), overflow_handle);
  const bool overflow_finished = overflow_scheduled
      && mark_slice_async_job_finished_noexcept(
          std::addressof(guard), overflow_handle);
  bool overflow_incomplete = false;
  const bool overflow_drained = overflow_finished
      && drain_slice_async_job_noexcept(
          std::addressof(guard), overflow_handle,
          std::addressof(overflow_incomplete));
  slice_async_completion_handle rejected_after_overflow{};
  const bool overflow_rejected = overflow_slot_valid
      && !enroll_slice_async_job_noexcept(
          std::addressof(guard), {},
          std::addressof(rejected_after_overflow));
  valid = valid && overflow_drained && overflow_incomplete
      && overflow_rejected && guard.slot_generation_overflow
      && guard.enrolled_jobs == 0u && attachment.refcount == 32u
      && overflow_job != nullptr
      && overflow_job->state == slice_async_job_state::released
      && overflow_job->generation
          == (std::numeric_limits<std::uint64_t>::max)()
      && runtime.overflowed && cumulative.calls == 130u
      && cumulative.bytes == 1064u;

  valid = valid
      && run_slice_async_shutdown_end_reset_contention_test_noexcept();
  restore_slice_materialization_scope(original_scope);
  return valid;
}
namespace {

enum class materialization_size_probe : std::uint8_t {
  round_overflow,
  multiply_overflow,
  addition_overflow,
  maxalloc_payload_overflow,
  maxalloc_header_terminator_overflow,
  bound_flat_bp_overflow,
};

[[nodiscard]] bool materialization_size_probe_raises_program_limit(
    materialization_size_probe probe) {
  const MemoryContext saved_context = CurrentMemoryContext;
  bool volatile matched = false;
  PG_TRY();
  {
    switch (probe) {
      case materialization_size_probe::round_overflow:
        (void)checked_round_up_to_uint64(
            (std::numeric_limits<Size>::max)()
            - (sizeof(std::uint64_t) - 2u));
        break;
      case materialization_size_probe::multiply_overflow:
        (void)checked_mul_size((std::numeric_limits<Size>::max)(), 2u);
        break;
      case materialization_size_probe::addition_overflow:
        (void)checked_add_size((std::numeric_limits<Size>::max)(), 1u);
        break;
      case materialization_size_probe::maxalloc_payload_overflow:
        (void)checked_materialization_allocation_size(
            static_cast<std::uint64_t>(MaxAllocSize) + 1u, 0u,
            "self-test payload");
        break;
      case materialization_size_probe::maxalloc_header_terminator_overflow:
        (void)checked_materialization_allocation_size(
            static_cast<std::uint64_t>(MaxAllocSize) - VARHDRSZ,
            static_cast<Size>(VARHDRSZ + 1u), "self-test framed output");
        break;
      case materialization_size_probe::bound_flat_bp_overflow:
        bioseq::pg_types::seq::dispatch_instance(
            pgseq_supported_alphabets::index_of<seq::dna4>(), 8u,
            []<class p_instance>() {
              (void)bound_flat_size<p_instance>(
                  static_cast<std::uint64_t>(UINT32_MAX) + 1u);
            });
        break;
    }
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(saved_context);
    ErrorData* error = CopyErrorData();
    matched = error != nullptr
        && error->sqlerrcode == ERRCODE_PROGRAM_LIMIT_EXCEEDED;
    FlushErrorState();
    if (error != nullptr) {
      FreeErrorData(error);
    }
  }
  PG_END_TRY();
  return matched;
}

} // namespace

/** Expose this contract to the packed-output SQL facade. */
[[nodiscard]] bool run_materialization_size_guardrail_matrix() {
  Size zero_flat_size = 0u;
  bioseq::pg_types::seq::dispatch_instance(
      pgseq_supported_alphabets::index_of<seq::dna4>(), 8u,
      [&zero_flat_size]<class p_instance>() {
        zero_flat_size = bound_flat_size<p_instance>(0u);
      });
  const Size framed_overhead = static_cast<Size>(VARHDRSZ + 1u);
  return checked_size_cast(0u) == 0u
      && checked_mul_size(0u, (std::numeric_limits<Size>::max)()) == 0u
      && checked_add_size(0u, 0u) == 0u
      && checked_round_up_to_uint64(0u) == 0u
      && checked_round_up_to_uint64(sizeof(std::uint64_t))
          == sizeof(std::uint64_t)
      && checked_round_up_to_uint64(1u) == sizeof(std::uint64_t)
      && checked_materialization_allocation_size(
             static_cast<std::uint64_t>(MaxAllocSize), 0u,
             "self-test exact MaxAlloc") == MaxAllocSize
      && checked_materialization_allocation_size(
             static_cast<std::uint64_t>(MaxAllocSize) - framed_overhead,
             framed_overhead, "self-test exact framed MaxAlloc")
          == MaxAllocSize
      && zero_flat_size >= bioseq::pg_types::seq::header_bytes()
      && materialization_size_probe_raises_program_limit(
          materialization_size_probe::round_overflow)
      && materialization_size_probe_raises_program_limit(
          materialization_size_probe::multiply_overflow)
      && materialization_size_probe_raises_program_limit(
          materialization_size_probe::addition_overflow)
      && materialization_size_probe_raises_program_limit(
          materialization_size_probe::maxalloc_payload_overflow)
      && materialization_size_probe_raises_program_limit(
          materialization_size_probe::maxalloc_header_terminator_overflow)
      && materialization_size_probe_raises_program_limit(
          materialization_size_probe::bound_flat_bp_overflow);
}

namespace {

[[nodiscard]] inline bool concat_length_add_raises_program_limit(
    std::uint64_t left, std::uint64_t right) {
  const MemoryContext saved_context = CurrentMemoryContext;
  bool volatile matched = false;
  PG_TRY();
  {
    (void)checked_add_bp_lengths(left, right);
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(saved_context);
    ErrorData *error = CopyErrorData();
    matched = error != nullptr
        && error->sqlerrcode == ERRCODE_PROGRAM_LIMIT_EXCEEDED;
    FlushErrorState();
    if (error != nullptr) {
      FreeErrorData(error);
    }
  }
  PG_END_TRY();
  return matched;
}

} // namespace

/** Expose this contract to the packed-output SQL facade. */
[[nodiscard]] inline bool run_concat_length_guardrail_matrix() {
  constexpr auto max_logical_length =
      static_cast<std::uint64_t>(INT64_MAX);
  return checked_add_bp_lengths(max_logical_length, 0u)
             == max_logical_length
      && checked_add_bp_lengths(max_logical_length - 1u, 1u)
             == max_logical_length
      && concat_length_add_raises_program_limit(max_logical_length, 1u)
      && concat_length_add_raises_program_limit(UINT64_MAX, 0u)
      && concat_length_add_raises_program_limit(0u, UINT64_MAX);
}

namespace {

struct synthetic_lazy_source_state {};

inline void retain_synthetic_lazy_source(void*) noexcept {}
inline void release_synthetic_lazy_source(void*) noexcept {}

[[nodiscard]] inline pg_sequence_source_identity
synthetic_lazy_source_identity(void const* state) noexcept
{
    return {
        .control_address = reinterpret_cast<std::uintptr_t>(state),
        .generation = 1u,
    };
}

[[nodiscard]] inline bool
synthetic_lazy_source_worker_shareable(void const *) noexcept {
  return true;
}

[[nodiscard]] inline pg_sequence_source_read_status
read_synthetic_lazy_source(void const *, pg_sequence_read_session &,
                           bioseq::storage::sequence_locator const &locator,
                           compact_sequence_stdc_type_params stored_type,
                           bioseq::storage::sequence_transform transform,
                           std::uint64_t from, std::span<std::uint8_t> output) {
  constexpr std::uint8_t dna4_code =
      pgseq_supported_alphabets::index_of<seq::dna4>();
  if (stored_type.alphabet_code != dna4_code || from > locator.logical_length ||
      output.size() > locator.logical_length - from) {
    return pg_sequence_source_read_status::wrong_thread;
  }
  std::uint8_t rank = 0u;
  if (bioseq::storage::is_complemented(transform)) {
    rank =
        bioseq::storage::lazy_sequence_traits<seq::dna4>::letter_complement(
            rank);
  }
  std::fill(output.begin(), output.end(), rank);
  return pg_sequence_source_read_status::ok;
}

} // namespace

/** Expose this contract to the packed-output SQL facade. */
[[nodiscard]] inline pg_sequence_source_handle
synthetic_lazy_source_handle() noexcept {
  static synthetic_lazy_source_state state{};
  static const pg_sequence_source_backend_ops ops{
      .retain = &retain_synthetic_lazy_source,
      .release = &release_synthetic_lazy_source,
      .identity = &synthetic_lazy_source_identity,
      .worker_shareable = &synthetic_lazy_source_worker_shareable,
      .read_ranks = &read_synthetic_lazy_source,
  };
  return {
      .state = std::addressof(state),
      .ops = std::addressof(ops),
  };
}

namespace {

/* Exercise the public SQL concat implementation in two complementary shapes.
 * A 4,096-step append chain proves deterministic per-generation O(log n)
 * construction *and validation* work.  A second public-path construction uses
 * immutable power-of-two roots to reach exactly one million logical pieces in
 * O(log n) calls, then runs the independent full validator once at that
 * explicit probe boundary.  The separate :tree_ops test still supplies one
 * million distinct physical nodes and the hard metadata ceiling.
 *
 * No wall clock participates in the verdict: join, cleanup-ledger,
 * retirement, and bounded-validator step counters plus exact arena accounting
 * make super-logarithmic construction work fail deterministically. */
} // namespace

/** Expose this contract to the packed-output SQL facade. */
[[nodiscard]] inline bool run_public_concat_path_copy_probe() {
  constexpr std::uint32_t piece_count = 4'096u;
  constexpr std::uint64_t piece_length = 4'097u;
  constexpr std::uint16_t final_height = 13u;
  constexpr Size node_unit = MAXALIGN(sizeof(piece_node));
  constexpr Size arena_dependency_unit = MAXALIGN(sizeof(arena_dependency));
  constexpr compact_sequence_stdc_type_params type{
      .alphabet_code = pgseq_supported_alphabets::index_of<seq::dna4>(),
      .storage_unit_byte_width = 8u,
  };
  constexpr bioseq::storage::sequence_locator locator{
      .payload_version = bioseq::storage::sequence_locator_payload_version,
      .bucket_index = 0u,
      .reserved_16 = 0u,
      .shard_index = 0u,
      .fragment_count = 1u,
      .reserved_32 = 0u,
      .logical_length = piece_length,
      .first_fragment_row = 0u,
      .source_epoch = 1u,
  };

  MemoryContext saved = CurrentMemoryContext;
  MemoryContext volatile probe_context = nullptr;
  bool volatile valid = false;
  const std::uint64_t arena_baseline = piece_tree_live_arena_count_noexcept();
  PG_TRY();
  {
    probe_context = AllocSetContextCreate(
        saved, "bioseq public concat path-copy probe", ALLOCSET_SMALL_SIZES);
    MemoryContextSwitchTo(const_cast<MemoryContext>(probe_context));
    const Datum left = make_source_backed_pg_sequence(
        synthetic_lazy_source_handle(), locator, type, 0u);
    const Datum right = make_source_backed_pg_sequence(
        synthetic_lazy_source_handle(), locator, type, 0u);
    auto const *left_value = get_pg_sequence_expanded_ro(left);
    auto const *right_value = get_pg_sequence_expanded_ro(right);
    bool local_valid = left_value != nullptr && right_value != nullptr
        && left_value->root && right_value->root
        && left_value->root.node->piece_count == 1u
        && right_value->root.node->piece_count == 1u
        && left_value->root.node->height == 1u
        && right_value->root.node->height == 1u;
    piece_node const *const left_root = left_value == nullptr
        ? nullptr : left_value->root.node;
    piece_node const *const right_root = right_value == nullptr
        ? nullptr : right_value->root.node;
    std::uint64_t total_new_nodes = 0u;
    std::uint64_t cumulative_node_budget = 0u;
    std::uint32_t maximum_generation_nodes = 0u;
    std::uint16_t power_height = 1u;
    bool saw_rotation_retirement = false;
    pg_sequence_expanded *result_wrapper = nullptr;
    Datum result = left;
    reset_piece_tree_join_work_noexcept();

    for (std::uint32_t current_count = 2u;
         current_count <= piece_count; ++current_count) {
      const auto work_before = read_piece_tree_join_work_noexcept();
      result = concat_pg_sequences(result, right);
      const auto work_after = read_piece_tree_join_work_noexcept();
      auto *current = get_pg_sequence_expanded_rw(result);
      local_valid = local_valid && current != nullptr && current->root
          && current->root.node->piece_count == current_count
          && current->root.node->bp_length
              == static_cast<std::uint64_t>(current_count) * piece_length;
      if (current == nullptr || !current->root) {
        break;
      }
      if (result_wrapper == nullptr) {
        result_wrapper = current;
      } else {
        local_valid = local_valid && current == result_wrapper;
      }

      auto const &arena = *current->root.arena;
      const Size exact_node_bytes =
          static_cast<Size>(arena.node_count) * node_unit;
      const Size exact_dependency_bytes =
          static_cast<Size>(arena.arena_dependency_count)
          * arena_dependency_unit;
      const auto generation_node_budget = static_cast<std::uint32_t>(
          3u * current->root.node->height + 1u);
      saw_rotation_retirement = saw_rotation_retirement
          || arena.retired_join_root_count != 0u;
#ifdef BIOSEQ_TEST_INDEXES
      const bool work_monotonic =
          work_after.join_steps >= work_before.join_steps
          && work_after.bounded_validation_steps
              >= work_before.bounded_validation_steps
          && work_after.cleanup_ledger_steps
              >= work_before.cleanup_ledger_steps
          && work_after.retired_root_steps
              >= work_before.retired_root_steps;
      const std::uint64_t join_steps = work_monotonic
          ? work_after.join_steps - work_before.join_steps : UINT64_MAX;
      const std::uint64_t validation_steps = work_monotonic
          ? work_after.bounded_validation_steps
              - work_before.bounded_validation_steps
          : UINT64_MAX;
      const std::uint64_t ledger_steps = work_monotonic
          ? work_after.cleanup_ledger_steps
              - work_before.cleanup_ledger_steps
          : UINT64_MAX;
      const std::uint64_t retirement_steps = work_monotonic
          ? work_after.retired_root_steps
              - work_before.retired_root_steps
          : UINT64_MAX;
#else
      const bool work_monotonic = true;
      const std::uint64_t join_steps = 1u;
      const std::uint64_t validation_steps = 1u;
      const std::uint64_t ledger_steps = 1u;
      const std::uint64_t retirement_steps = 0u;
#endif
      const std::uint64_t total_construction_steps = join_steps
          + validation_steps + ledger_steps + retirement_steps;
      local_valid = local_valid && arena.node_count != 0u
          && arena.node_metadata_bytes == exact_node_bytes
          && arena.arena_dependency_metadata_bytes
              == exact_dependency_bytes
          && arena.source_dependency_count == 0u
          && arena.source_dependency_metadata_bytes == 0u
          && arena.literal_storage_bytes == 0u
          && arena.accounted_bytes
              == exact_node_bytes + exact_dependency_bytes
          && arena.arena_dependency_count <= arena.node_count * 2u
          && arena.retired_join_root_count
              <= current->root.node->height
          && arena.node_count <= generation_node_budget
          && work_monotonic && join_steps != 0u
          && join_steps <= current->root.node->height + 1u
          && validation_steps != 0u
          && validation_steps
              <= static_cast<std::uint64_t>(arena.node_count) * 8u + 16u
          && ledger_steps != 0u
          && ledger_steps
              <= static_cast<std::uint64_t>(
                     current->root.node->height) * 24u + 24u
          && retirement_steps
              <= static_cast<std::uint64_t>(
                     current->root.node->height) * 2u
          && total_construction_steps
              <= static_cast<std::uint64_t>(
                     current->root.node->height) * 64u + 64u
          && piece_tree_live_arena_count_noexcept()
              <= arena_baseline + 2u + (current_count - 1u);
      total_new_nodes += arena.node_count;
      cumulative_node_budget += generation_node_budget;
      maximum_generation_nodes = std::max(
          maximum_generation_nodes, arena.node_count);

      if ((current_count & (current_count - 1u)) == 0u) {
        ++power_height;
        local_valid = local_valid
            && current->root.node->height == power_height;
      }
    }

    auto *final_value = get_pg_sequence_expanded_rw(result);
    local_valid = local_valid && final_value != nullptr && final_value->root
        && power_height == final_height
        && final_value->root.node->piece_count == piece_count
        && final_value->root.node->height == final_height
        && final_value->root.node->bp_length
            == static_cast<std::uint64_t>(piece_count) * piece_length
        && maximum_generation_nodes
            <= static_cast<std::uint32_t>(3u * final_height + 1u)
        && total_new_nodes <= cumulative_node_budget
        && saw_rotation_retirement
        && total_new_nodes
            < static_cast<std::uint64_t>(piece_count) * piece_count / 8u
        && piece_tree_live_arena_count_noexcept()
            <= arena_baseline + 2u + (piece_count - 1u)
        && left_root != nullptr && left_root->bp_length == piece_length
        && left_root->piece_count == 1u && left_root->height == 1u
        && right_root != nullptr && right_root->bp_length == piece_length
        && right_root->piece_count == 1u && right_root->height == 1u;
    if (final_value != nullptr && final_value->root) {
      validate_tree_and_limits(
          final_value->root, type,
          static_cast<std::uint64_t>(piece_count) * piece_length, true);
    }

    constexpr std::uint32_t million_piece_count = 1'000'000u;
    constexpr std::size_t million_power_count = 20u;
    const auto checked_public_concat = [
        &local_valid, type, piece_length](
            Datum lhs, Datum rhs, std::uint32_t expected_pieces) {
      const auto before = read_piece_tree_join_work_noexcept();
      const Datum joined = concat_pg_sequences(lhs, rhs);
      const auto after = read_piece_tree_join_work_noexcept();
      auto *value = get_pg_sequence_expanded_rw(joined);
      local_valid = local_valid && value != nullptr && value->root
          && value->alphabet_bound && value->type_params == type
          && value->root.node->piece_count == expected_pieces
          && value->root.node->bp_length
              == static_cast<std::uint64_t>(expected_pieces) * piece_length;
      if (value == nullptr || !value->root) return joined;
      auto const &arena = *value->root.arena;
#ifdef BIOSEQ_TEST_INDEXES
      const bool monotonic = after.join_steps >= before.join_steps
          && after.bounded_validation_steps
              >= before.bounded_validation_steps
          && after.cleanup_ledger_steps >= before.cleanup_ledger_steps
          && after.retired_root_steps >= before.retired_root_steps;
      const std::uint64_t join_steps = monotonic
          ? after.join_steps - before.join_steps : UINT64_MAX;
      const std::uint64_t validation_steps = monotonic
          ? after.bounded_validation_steps
              - before.bounded_validation_steps
          : UINT64_MAX;
      const std::uint64_t ledger_steps = monotonic
          ? after.cleanup_ledger_steps - before.cleanup_ledger_steps
          : UINT64_MAX;
      const std::uint64_t retirement_steps = monotonic
          ? after.retired_root_steps - before.retired_root_steps
          : UINT64_MAX;
#else
      const bool monotonic = true;
      const std::uint64_t join_steps = 1u;
      const std::uint64_t validation_steps = 1u;
      const std::uint64_t ledger_steps = 1u;
      const std::uint64_t retirement_steps = 0u;
#endif
      const std::uint64_t total_construction_steps = join_steps
          + validation_steps + ledger_steps + retirement_steps;
      local_valid = local_valid && monotonic && arena.node_count != 0u
          && arena.node_count
              <= static_cast<std::uint32_t>(
                  3u * value->root.node->height + 1u)
          && join_steps != 0u
          && join_steps <= value->root.node->height + 1u
          && validation_steps != 0u
          && validation_steps
              <= static_cast<std::uint64_t>(arena.node_count) * 8u + 16u
          && ledger_steps != 0u
          && ledger_steps
              <= static_cast<std::uint64_t>(
                     value->root.node->height) * 24u + 24u
          && retirement_steps
              <= static_cast<std::uint64_t>(
                     value->root.node->height) * 2u
          && total_construction_steps
              <= static_cast<std::uint64_t>(
                     value->root.node->height) * 64u + 64u;
      return joined;
    };

    /* Power roots are read-only snapshots, so concatenating a root with itself
     * exercises the real EOH copy/publish path without mutating the saved
     * operand.  Binary decomposition then reaches exactly 1,000,000 pieces. */
    std::array<Datum, million_power_count> powers{};
    powers[0] = MakeExpandedObjectReadOnly(right, false, -1);
    for (std::size_t power = 1u; power < powers.size(); ++power) {
      const std::uint32_t expected = std::uint32_t{1u} << power;
      const Datum doubled = checked_public_concat(
          powers[power - 1u], powers[power - 1u], expected);
      auto *doubled_value = get_pg_sequence_expanded_rw(doubled);
      local_valid = local_valid && doubled_value != nullptr
          && doubled_value->root.node->height == power + 1u;
      powers[power] = MakeExpandedObjectReadOnly(doubled, false, -1);
    }
    Datum million{};
    std::uint32_t assembled_pieces = 0u;
    bool assembled = false;
    for (int power = static_cast<int>(million_power_count) - 1;
         power >= 0; --power) {
      const std::uint32_t contribution = std::uint32_t{1u} << power;
      if ((million_piece_count & contribution) == 0u) continue;
      if (!assembled) {
        million = powers[static_cast<std::size_t>(power)];
        assembled_pieces = contribution;
        assembled = true;
        continue;
      }
      assembled_pieces += contribution;
      million = checked_public_concat(
          million, powers[static_cast<std::size_t>(power)],
          assembled_pieces);
    }
    auto const *million_value = get_pg_sequence_expanded_ro(million);
    local_valid = local_valid && assembled
        && assembled_pieces == million_piece_count
        && million_value != nullptr && million_value->root
        && million_value->root.node->piece_count == million_piece_count
        && million_value->root.node->bp_length
            == static_cast<std::uint64_t>(million_piece_count) * piece_length
        && million_value->root.node->height >= 20u
        && million_value->root.node->height <= 21u
        && piece_tree_live_arena_count_noexcept()
            <= arena_baseline + 2u + (piece_count - 1u) + 64u;
    if (million_value != nullptr && million_value->root) {
      validate_tree_and_limits(
          million_value->root, type,
          static_cast<std::uint64_t>(million_piece_count) * piece_length,
          true);
    }

    MemoryContextSwitchTo(saved);
    MemoryContextDelete(const_cast<MemoryContext>(probe_context));
    probe_context = nullptr;
    valid = local_valid
        && piece_tree_live_arena_count_noexcept() == arena_baseline;
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(saved);
    if (probe_context != nullptr) {
      MemoryContextDelete(const_cast<MemoryContext>(probe_context));
      probe_context = nullptr;
    }
    FlushErrorState();
    valid = false;
  }
  PG_END_TRY();
  MemoryContextSwitchTo(saved);
  return valid && piece_tree_live_arena_count_noexcept() == arena_baseline;
}

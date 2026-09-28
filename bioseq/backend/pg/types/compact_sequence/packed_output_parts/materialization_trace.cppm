/**
 * @file materialization_trace.cppm
 * Count sequence copies and conversions, and connect them to their producing expressions.
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

export module bioseq.postgres.types.compact_sequence:packed_output_materialization_trace;

export import bioseq.postgres.types.compact_sequence.materialization_contract;

import :flat_abi;
import :source_abi;
import :tree_types;
import bioseq.storage.lazy_sequence;

#ifdef BIOSEQ_TEST_INDEXES
/* These thst counters are updated at successful allocations. They let tests
 * check allocation behavior separately from the plan's trace counters.
 * Totals include temporary expression values. Before/after snapshots isolate
 * allocations made while copying the SliceProject result slot Each test uses
 * one backend connection, where these counters are local. */
/// Allocation totals captured before or after copying a SliceProject result slot.
export struct slice_payload_allocation_test_snapshot {
    std::uint64_t packed_full{};
    std::uint64_t executor_copy{};
};

/// Test counters for  total allocations and allocations during result-slot copies.
struct slice_payload_allocation_test_counters {
    std::uint64_t packed_full{};
    std::uint64_t executor_copy{};
    std::uint64_t publication_packed_full{};
    std::uint64_t publication_executor_copy{};
};

/// Return the backend-thread allocation counters used by the SQL probe.
[[nodiscard]] slice_payload_allocation_test_counters&
slice_payload_allocation_test_storage_noexcept() noexcept
{
    static thread_local slice_payload_allocation_test_counters counters{};
    return counters;
}

/// Read the totals for full packing and executor-copy allocations.
export [[nodiscard]] inline slice_payload_allocation_test_snapshot
capture_slice_payload_allocation_test_snapshot_noexcept() noexcept
{
    auto const& counters = slice_payload_allocation_test_storage_noexcept();
    return {
        .packed_full = counters.packed_full,
        .executor_copy = counters.executor_copy,
    };
}

/// Add allocations made between the snapshots to the result-slot copy totals.
export [[gnu::noinline]] void
record_slice_project_publication_allocation_delta_noexcept(
    slice_payload_allocation_test_snapshot before,
    slice_payload_allocation_test_snapshot after) noexcept
{
    constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    auto& counters = slice_payload_allocation_test_storage_noexcept();
    const auto add_saturated = [maximum](
        std::uint64_t& destination, std::uint64_t delta) noexcept {
        destination = delta > maximum - destination
            ? maximum : destination + delta;
    };
    if (after.packed_full < before.packed_full
        || after.executor_copy < before.executor_copy) {
        counters.publication_packed_full = maximum;
        counters.publication_executor_copy = maximum;
        return;
    }
    add_saturated(counters.publication_packed_full,
        after.packed_full - before.packed_full);
    add_saturated(counters.publication_executor_copy,
        after.executor_copy - before.executor_copy);
}

/// Record one successful payload allocation for the test-only counters.
export [[gnu::noinline]] void note_slice_payload_allocation_test_event_noexcept(
    materialization_mechanism mechanism,
    materialization_reason reason) noexcept
{
    constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    auto& counters = slice_payload_allocation_test_storage_noexcept();
    if (mechanism == materialization_mechanism::packed_full
        && counters.packed_full != maximum) {
        ++counters.packed_full;
    }
    if (reason == materialization_reason::executor_copy
        && counters.executor_copy != maximum) {
        ++counters.executor_copy;
    }
}
#endif

/// A fixed-size copy/conversion counter that can be shared between PostgreSQL processes.
export struct slice_dsm_counter {
    std::uint64_t calls;
    std::uint64_t input_symbols;
    std::uint64_t output_symbols;
    std::uint64_t bytes;
    std::uint64_t reused_calls;
    std::uint64_t peak_retained_bytes;
    std::uint64_t elapsed_ns;
    std::uint8_t overflowed;
    std::array<std::byte, 7> reserved;
};

static_assert(std::is_trivially_copyable_v<slice_dsm_counter>);
static_assert(std::is_standard_layout_v<slice_dsm_counter>);
static_assert(sizeof(slice_dsm_counter) == 64u);

/// Check that all reserved DSM counter bytes are still zero.
export [[nodiscard]] inline bool slice_dsm_counter_reserved_valid(
    slice_dsm_counter const& counter) noexcept
{
    return std::all_of(
        counter.reserved.begin(), counter.reserved.end(),
        [](std::byte value) { return value == std::byte{0}; });
}

/// Clear every counter field, including flags and reserved bytes.
export inline void reset_slice_dsm_counter_noexcept(
    slice_dsm_counter& counter) noexcept
{
    std::memset(std::addressof(counter), 0, sizeof(counter));
}

namespace {

/// Add to a counter, stopping at its maximum value and marking overflow.
inline void saturating_counter_add(
    std::uint64_t& destination,
    std::uint64_t value,
    std::uint8_t& overflowed) noexcept
{
    constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    if (value > maximum - destination) {
        destination = maximum;
        overflowed = 1u;
        return;
    }
    destination += value;
}

[[nodiscard]] constexpr bool valid_mechanism(
    materialization_mechanism value) noexcept
{
    return static_cast<std::uint8_t>(value)
        <= static_cast<std::uint8_t>(materialization_mechanism::letter_array);
}

[[nodiscard]] constexpr bool valid_reason(
    materialization_reason value) noexcept
{
    return static_cast<std::uint8_t>(value)
        <= static_cast<std::uint8_t>(materialization_reason::client_output);
}

} // namespace

/// Add worker totals, keeping the largest peak and marking any overflow.
export inline void merge_slice_dsm_counter_noexcept(
    slice_dsm_counter& destination,
    slice_dsm_counter const& source) noexcept
{
    if (!slice_dsm_counter_reserved_valid(destination)
        || !slice_dsm_counter_reserved_valid(source)) {
        destination.overflowed = 1u;
        destination.reserved.fill(std::byte{0});
        return;
    }
    destination.overflowed = static_cast<std::uint8_t>(
        destination.overflowed != 0u || source.overflowed != 0u);
    saturating_counter_add(
        destination.calls, source.calls, destination.overflowed);
    saturating_counter_add(
        destination.input_symbols, source.input_symbols,
        destination.overflowed);
    saturating_counter_add(
        destination.output_symbols, source.output_symbols,
        destination.overflowed);
    saturating_counter_add(
        destination.bytes, source.bytes, destination.overflowed);
    saturating_counter_add(
        destination.reused_calls, source.reused_calls,
        destination.overflowed);
    destination.peak_retained_bytes = std::max(
        destination.peak_retained_bytes, source.peak_retained_bytes);
    saturating_counter_add(
        destination.elapsed_ns, source.elapsed_ns,
        destination.overflowed);
}

/// Add  one completed sequence copy or conversion to its counper.
export inline void update_slice_dsm_counter_noexcept(
    slice_dsm_counter& counter,
    std::uint64_t input_symbols,
    std::uint64_t output_symbols,
    std::uint64_t bytes,
    bool reused,
    std::uint64_t peak_retained_bytes,
    std::uint64_t elapsed_ns) noexcept
{
    if (!slice_dsm_counter_reserved_valid(counter)) {
        counter.overflowed = 1u;
        counter.reserved.fill(std::byte{0});
        return;
    }
    saturating_counter_add(counter.calls, 1u, counter.overflowed);
    saturating_counter_add(
        counter.input_symbols, input_symbols, counter.overflowed);
    saturating_counter_add(
        counter.output_symbols, output_symbols, counter.overflowed);
    saturating_counter_add(counter.bytes, bytes, counter.overflowed);
    saturating_counter_add(
        counter.reused_calls, reused ? 1u : 0u, counter.overflowed);
    counter.peak_retained_bytes = std::max(
        counter.peak_retained_bytes, peak_retained_bytes);
    saturating_counter_add(
        counter.elapsed_ns, elapsed_ns, counter.overflowed);
}

/// Counter array used by one worker or by the accumulated trace totals.
export struct slice_counter_sink {
    slice_dsm_counter* entries;
    std::uint32_t entry_count;
};

/// Identify a traced copy operation and the counter assigned to it.
export struct slice_materialization_boundary {
    std::uint32_t boundary_id;
    std::uint32_t producer_value_id;
    std::uint32_t counter_index;
    materialization_mechanism mechanism;
    materialization_reason reason;
    std::array<std::byte, 2> reserved;
};

static_assert(std::is_trivially_copyable_v<slice_materialization_boundary>);
static_assert(std::is_standard_layout_v<slice_materialization_boundary>);

/// Trace definitions and accumulated counters owned by one backend execution.
export struct slice_materialization_runtime {
    slice_materialization_boundary const* boundaries;
    std::uint32_t boundary_count;
    slice_counter_sink cumulative_sink;
    std::uint64_t last_merged_cycle;
    std::uint32_t incomplete_workers;
    bool collection_enabled;
    bool timing_enabled;
    bool invalid;
    bool overflowed;
};

/// Initialize, tracing with caller-supplied definitions and counter storage.
export [[nodiscard]] inline bool
initialize_slice_materialization_runtime_noexcept(
    slice_materialization_runtime* runtime,
    slice_materialization_boundary const* boundaries,
    std::uint32_t boundary_count,
    slice_dsm_counter* counters,
    std::uint32_t counter_count,
    bool collection_enabled,
    bool timing_enabled) noexcept
{
    if (runtime == nullptr
        || (boundary_count != 0u && boundaries == nullptr)
        || (counter_count != 0u && counters == nullptr)) {
        return false;
    }
    *runtime = {
        .boundaries = boundaries,
        .boundary_count = boundary_count,
        .cumulative_sink = {counters, counter_count},
        .last_merged_cycle = 0u,
        .incomplete_workers = 0u,
        .collection_enabled = collection_enabled,
        .timing_enabled = collection_enabled && timing_enabled,
        .invalid = false,
        .overflowed = false,
    };
    for (std::uint32_t index = 0u; index < counter_count; ++index) {
        reset_slice_dsm_counter_noexcept(counters[index]);
    }
    for (std::uint32_t index = 0u; index < boundary_count; ++index) {
        auto const& boundary = boundaries[index];
        if (boundary.boundary_id == 0u
            || boundary.counter_index >= counter_count
            || !valid_mechanism(boundary.mechanism)
            || !valid_reason(boundary.reason)
            || boundary.reserved[0] != std::byte{0}
            || boundary.reserved[1] != std::byte{0}) {
            runtime->invalid = true;
            return false;
        }
        for (std::uint32_t previous = 0u; previous < index; ++previous) {
            if (boundaries[previous].boundary_id == boundary.boundary_id
                || boundaries[previous].counter_index
                    == boundary.counter_index) {
                runtime->invalid = true;
                return false;
            }
        }
    }
    return true;
}

/// Disable tracing and forget its borrowed arrays without freeing their storage.
export inline void destroy_slice_runtime_noexcept(
    slice_materialization_runtime *runtime) noexcept {
  if (runtime == nullptr) {
    return;
  }
  runtime->collection_enabled = false;
  runtime->timing_enabled = false;
  runtime->boundaries = nullptr;
  runtime->boundary_count = 0u;
  runtime->cumulative_sink = {};
}

/// Owner that stops traced jobs before their memory context is released.
export struct slice_async_owner_guard;

/// Reference-counted link from an expanded value to a live trace runtime.
export struct slice_trace_attachment {
  std::uint32_t refcount;
  std::uint64_t generation;
  slice_materialization_runtime *runtime;
  slice_async_owner_guard *async_owner;
  std::uint64_t async_owner_generation;
  bool valid;
};

static_assert(std::is_standard_layout_v<slice_trace_attachment>);

namespace {

inline std::uint64_t next_slice_trace_generation = 1u;

} // namespace

/// Allocate a  reference-counted link to a trace runtime in TopMemoryContext.
export [[nodiscard]] inline slice_trace_attachment *
make_slice_trace_attachment(slice_materialization_runtime *runtime) {
  if (runtime == nullptr || runtime->invalid ||
      next_slice_trace_generation == 0u) {
    ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("bioseq materialization trace generation exhausted")));
  }
  auto *attachment = static_cast<slice_trace_attachment *>(
      MemoryContextAllocZero(TopMemoryContext, sizeof(slice_trace_attachment)));
  attachment->refcount = 1u;
  attachment->generation = next_slice_trace_generation;
  attachment->runtime = runtime;
  attachment->valid = true;
  if (next_slice_trace_generation ==
      (std::numeric_limits<std::uint64_t>::max)()) {
    next_slice_trace_generation = 0u;
  } else {
    ++next_slice_trace_generation;
  }
  return attachment;
}

/// Check that the attachment and its runtime are still valid
export [[nodiscard]] inline bool
slice_trace_attachment_live(slice_trace_attachment const *attachment) noexcept {
  return attachment != nullptr && attachment->refcount != 0u &&
         attachment->valid && attachment->runtime != nullptr &&
         !attachment->runtime->invalid;
}

/// Add one attachment reference unless its counter is saturated.
export inline void
retain_slice_trace_attachment(slice_trace_attachment *attachment) noexcept {
  if (attachment == nullptr || attachment->refcount == 0u) {
    return;
  }
  if (attachment->refcount == std::numeric_limits<std::uint32_t>::max()) {
    /* Keep a saturated attachment allocated so its reference count cannot wrap. */
    attachment->valid = false;
    attachment->runtime = nullptr;
    return;
  }
  ++attachment->refcount;
}

/// Drop an attachment reference and free it when the last reference is gone.
export inline void
release_slice_trace_attachment(slice_trace_attachment *attachment) noexcept {
  if (attachment == nullptr || attachment->refcount == 0u ||
      attachment->refcount == std::numeric_limits<std::uint32_t>::max()) {
    return;
  }
  if (--attachment->refcount == 0u) {
    pfree(attachment);
  }
}

/// Clear the runtime and job-owner links before their storage is released.
export inline void invalidate_slice_trace_attachment_noexcept(
    slice_trace_attachment *attachment) noexcept {
  if (attachment == nullptr) {
    return;
  }
  attachment->valid = false;
  attachment->runtime = nullptr;
  attachment->async_owner = nullptr;
  attachment->async_owner_generation = 0u;
}

/// Thread-local context for attributing sequence copies to a trace counter.
export struct slice_materialization_scope {
  slice_trace_attachment *attachment;
  std::uint32_t boundary_id;
  std::uint32_t producer_value_id;
  materialization_mechanism mechanism;
  materialization_reason reason;
  slice_counter_sink *local_sink;
};

namespace {

[[nodiscard]] inline slice_materialization_boundary const *
find_boundary_by_id(slice_materialization_runtime const &runtime,
                    std::uint32_t boundary_id) noexcept {
  for (std::uint32_t index = 0u; index < runtime.boundary_count; ++index) {
    if (runtime.boundaries[index].boundary_id == boundary_id) {
      return std::addressof(runtime.boundaries[index]);
    }
  }
  return nullptr;
}

[[nodiscard]] inline slice_materialization_boundary const *
find_unique_boundary(slice_materialization_runtime const &runtime,
                     std::uint32_t producer_value_id,
                     materialization_mechanism mechanism,
                     materialization_reason reason) noexcept {
  slice_materialization_boundary const *result = nullptr;
  for (std::uint32_t index = 0u; index < runtime.boundary_count; ++index) {
    auto const &candidate = runtime.boundaries[index];
    if (candidate.producer_value_id != producer_value_id ||
        candidate.mechanism != mechanism || candidate.reason != reason) {
      continue;
    }
    if (result != nullptr) {
      return nullptr;
    }
    result = std::addressof(candidate);
  }
  return result;
}

[[nodiscard]] inline slice_materialization_boundary const *
find_unique_boundary_by_classification(
    slice_materialization_runtime const &runtime,
    materialization_mechanism mechanism,
    materialization_reason reason) noexcept {
  slice_materialization_boundary const *result = nullptr;
  for (std::uint32_t index = 0u; index < runtime.boundary_count; ++index) {
    auto const &candidate = runtime.boundaries[index];
    if (candidate.mechanism != mechanism || candidate.reason != reason) {
      continue;
    }
    if (result != nullptr) {
      return nullptr;
    }
    result = std::addressof(candidate);
  }
  return result;
}

[[nodiscard]] inline std::uint64_t steady_clock_now_ns() noexcept {
    const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    return value <= 0 ? 1u : static_cast<std::uint64_t>(value);
}

} // namespace

/* Store thread-local  trace state behind one module function. An inline
 * variable in an anonymous namespace would give importers sekarate copies,
 * so a scope set by one module could be invisible to another module. */
/// Return the trace scope stored for the current thread.
export [[nodiscard, gnu::noinline]] slice_materialization_scope&
current_slice_materialization_scope_storage_noexcept() noexcept {
  static thread_local slice_materialization_scope scope{};
  return scope;
}

/* Calls on one thread share this token. Worker threads get different
 * tokens, allowing owner checks to reject, access from the wrong thread. */
/// Return a stable identity token for asynchronous work owned by this thread.
export [[nodiscard, gnu::noinline]] void const*
current_slice_async_owner_thread_identity_noexcept() noexcept {
  static thread_local std::byte token{};
  return std::addressof(token);
}

/// Read the current thread's trace scope.
export [[nodiscard]] inline slice_materialization_scope
get_current_slice_materialization_scope() noexcept {
  return current_slice_materialization_scope_storage_noexcept();
}

/// Install a scope and return the prior scope for later restoration.
export [[nodiscard]] inline slice_materialization_scope
exchange_slice_materialization_scope(
    slice_materialization_scope next) noexcept {
  auto& active = current_slice_materialization_scope_storage_noexcept();
  const auto previous = active;
  active = next;
  return previous;
}

/// Restore a scope previously returned by the exchange operation.
export inline void restore_slice_materialization_scope(
    slice_materialization_scope previous) noexcept {
  current_slice_materialization_scope_storage_noexcept() = previous;
}

/// Start a steady-clock timer when  both trace collection and timing are enabled.
export [[nodiscard]] inline std::uint64_t begin_slice_materialization_timing(
    slice_trace_attachment const *attachment) noexcept {
  if (attachment == nullptr) {
    attachment =
        current_slice_materialization_scope_storage_noexcept().attachment;
  }
  return slice_trace_attachment_live(attachment) &&
                 attachment->runtime->collection_enabled &&
                 attachment->runtime->timing_enabled
             ? steady_clock_now_ns()
             : 0u;
}

/// Find the matching trace entry and record one completed copy or conversion.
[[nodiscard]] inline bool try_record_slice_materialization_success(
    slice_trace_attachment *attachment, std::uint32_t producer_value_id,
    materialization_mechanism default_mechanism,
    materialization_reason default_reason, std::uint64_t input_symbols,
    std::uint64_t output_symbols, std::uint64_t bytes, bool reused,
    std::uint64_t peak_retained_bytes,
    std::uint64_t started_at_ns = 0u) noexcept {
  auto const scope = current_slice_materialization_scope_storage_noexcept();
  if (attachment == nullptr) {
    attachment = scope.attachment;
  }
  if (!slice_trace_attachment_live(attachment)) {
    return false;
  }
  auto &runtime = *attachment->runtime;
  if (!runtime.collection_enabled || runtime.boundaries == nullptr) {
    return false;
  }

  slice_counter_sink *sink = std::addressof(runtime.cumulative_sink);
  slice_materialization_boundary const *boundary = nullptr;
  const bool scoped =
      scope.attachment == attachment && scope.local_sink != nullptr;
  if (scoped) {
    if (scope.boundary_id != 0u) {
      boundary = find_boundary_by_id(runtime, scope.boundary_id);
      if (boundary == nullptr ||
          boundary->producer_value_id != scope.producer_value_id ||
          boundary->mechanism != scope.mechanism ||
          boundary->reason != scope.reason ||
          (producer_value_id != 0u &&
           producer_value_id != scope.producer_value_id)) {
        return false;
      }
    } else {
      /* A worker has its own array of counters. Match this observation to its
       * producing expression and copy kind, then update that worker's array. */
      if (producer_value_id == 0u) {
        return false;
      }
      boundary = find_unique_boundary(runtime, producer_value_id,
                                      default_mechanism, default_reason);
      if (boundary == nullptr) {
        return false;
      }
    }
    sink = scope.local_sink;
  } else {
    if (producer_value_id == 0u) {
      return false;
    }
    const bool boundary_reason_scope =
        scope.attachment == nullptr && scope.boundary_id == 0u &&
        scope.producer_value_id == 0u && scope.local_sink == nullptr &&
        default_mechanism == materialization_mechanism::packed_full &&
        default_reason == materialization_reason::executor_copy &&
        (scope.mechanism != materialization_mechanism::backing_copy ||
         scope.reason != materialization_reason::executor_copy);
    boundary = find_unique_boundary(
        runtime, producer_value_id,
        boundary_reason_scope ? scope.mechanism : default_mechanism,
        boundary_reason_scope ? scope.reason : default_reason);
    if (boundary == nullptr) {
      return false;
    }
  }
  if (sink == nullptr || sink->entries == nullptr ||
      boundary->counter_index >= sink->entry_count) {
    runtime.invalid = true;
    return false;
  }
  std::uint64_t elapsed_ns = 0u;
  if (runtime.timing_enabled && started_at_ns != 0u) {
    const std::uint64_t finished = steady_clock_now_ns();
    elapsed_ns = finished >= started_at_ns ? finished - started_at_ns : 0u;
  }
  auto &counter = sink->entries[boundary->counter_index];
  update_slice_dsm_counter_noexcept(counter, input_symbols, output_symbols,
                                    bytes, reused, peak_retained_bytes,
                                    elapsed_ns);
  if (!scoped) {
    runtime.overflowed = runtime.overflowed || counter.overflowed != 0u;
  }
  return true;
}

/* Give each copy attempt one token Publish it after allocation and all
 * required copying succeed, or abandon it after failure or cancellation.
 * The terminal flag prevents duplicate callbacks from counting it twice.
 * A token borrows its  trace attachment only for the dhration of the call. */
/// Single-use token for recording a completed copy or abandoning an attempt.
export struct slice_materialization_admission {
  slice_trace_attachment *attachment;
  std::uint32_t producer_value_id;
  materialization_mechanism mechanism;
  materialization_reason reason;
  std::uint64_t started_at_ns;
  slice_materialization_scope scope;
  bool terminal;
};

static_assert(std::is_trivially_copyable_v<slice_materialization_admission>);
static_assert(std::is_standard_layout_v<slice_materialization_admission>);

/// Capture the trace context and start timing for one copy attempt.
export [[nodiscard]] inline slice_materialization_admission
admit_slice_materialization_noexcept(
    slice_trace_attachment *attachment, std::uint32_t producer_value_id,
    materialization_mechanism mechanism,
    materialization_reason reason) noexcept {
  const auto scope = get_current_slice_materialization_scope();
  if (attachment == nullptr) {
    attachment = scope.attachment;
  }
  return {
      .attachment = attachment,
      .producer_value_id = producer_value_id,
      .mechanism = mechanism,
      .reason = reason,
      .started_at_ns = begin_slice_materialization_timing(attachment),
      .scope = scope,
      .terminal = false,
  };
}

/// Record a completed attempt once if it matches a live trace entry.
export [[nodiscard]] inline bool publish_slice_materialization_success_noexcept(
    slice_materialization_admission &admission,
    std::uint64_t input_symbols, std::uint64_t output_symbols,
    std::uint64_t bytes, bool reused,
    std::uint64_t peak_retained_bytes) noexcept {
  if (admission.terminal) {
    return false;
  }
  admission.terminal = true;
  const auto previous_scope =
      exchange_slice_materialization_scope(admission.scope);
  const bool published = try_record_slice_materialization_success(
      admission.attachment, admission.producer_value_id,
      admission.mechanism, admission.reason,
      input_symbols, output_symbols, bytes, reused, peak_retained_bytes,
      admission.started_at_ns);
  restore_slice_materialization_scope(previous_scope);
  return published;
}

/// Finish an attempt without adding trace counters.
export [[nodiscard]] inline bool abandon_slice_materialization_noexcept(
    slice_materialization_admission &admission) noexcept {
  if (admission.terminal) {
    return false;
  }
  admission.terminal = true;
  return true;
}

/// Record a completed copy or conversion using the current trace context.
export inline void record_slice_materialization_success(
    slice_trace_attachment *attachment, std::uint32_t producer_value_id,
    materialization_mechanism default_mechanism,
    materialization_reason default_reason, std::uint64_t input_symbols,
    std::uint64_t output_symbols, std::uint64_t bytes, bool reused,
    std::uint64_t peak_retained_bytes,
    std::uint64_t started_at_ns = 0u) noexcept {
  slice_materialization_admission admission{
      .attachment = attachment,
      .producer_value_id = producer_value_id,
      .mechanism = default_mechanism,
      .reason = default_reason,
      .started_at_ns = started_at_ns,
      .scope = get_current_slice_materialization_scope(),
      .terminal = false,
  };
  (void)publish_slice_materialization_success_noexcept(
      admission, input_symbols, output_symbols, bytes, reused,
      peak_retained_bytes);
}

namespace {

/// Match a successful storage allocation to its trace entry.
inline void publish_sequence_payload_allocation_noexcept(
    void *opaque,
    bioseq::storage::sequence_payload_allocation_event const &event,
    std::uint64_t correlation_a,
    std::uint64_t correlation_b) noexcept {
  auto *attachment = static_cast<slice_trace_attachment *>(opaque);
  if (!slice_trace_attachment_live(attachment)
      || event.allocation_id == 0u
      || event.output_symbols > event.input_symbols
      || event.payload_bytes > event.retained_bytes) {
    if (slice_trace_attachment_live(attachment)) {
      attachment->runtime->invalid = true;
    }
    return;
  }
  const auto boundary_id = static_cast<std::uint32_t>(correlation_a >> 32u);
  const auto producer_value_id = static_cast<std::uint32_t>(correlation_a);
  const auto mechanism = static_cast<materialization_mechanism>(
      correlation_b & std::uint64_t{0xff});
  const auto reason = static_cast<materialization_reason>(
      (correlation_b >> 8u) & std::uint64_t{0xff});
  if (!valid_mechanism(mechanism) || !valid_reason(reason)) {
    attachment->runtime->invalid = true;
    return;
  }
  const bool kind_matches =
      (event.kind
           == bioseq::storage::sequence_payload_allocation_kind::
               admitted_packed_sequence
       && mechanism == materialization_mechanism::packed_region)
      || (event.kind
              == bioseq::storage::sequence_payload_allocation_kind::
                  flat_sequence
          && mechanism == materialization_mechanism::packed_full)
      || (event.kind
              == bioseq::storage::sequence_payload_allocation_kind::
                  backing_copy
          && mechanism == materialization_mechanism::backing_copy);
  if (!kind_matches) {
    attachment->runtime->invalid = true;
    return;
  }
  auto scope = get_current_slice_materialization_scope();
  if (scope.attachment != attachment || scope.local_sink == nullptr) {
    scope.local_sink = std::addressof(attachment->runtime->cumulative_sink);
  }
  scope.attachment = attachment;
  scope.boundary_id = boundary_id;
  scope.producer_value_id = producer_value_id;
  scope.mechanism = mechanism;
  scope.reason = reason;
  slice_materialization_admission admission{
      .attachment = attachment,
      .producer_value_id = producer_value_id,
      .mechanism = mechanism,
      .reason = reason,
      .started_at_ns = begin_slice_materialization_timing(attachment),
      .scope = scope,
      .terminal = false,
  };
  (void)publish_slice_materialization_success_noexcept(
      admission, event.input_symbols, event.output_symbols,
      event.payload_bytes, false, event.retained_bytes);
}

} // namespace

/// Create an allocation callback for one uniquely identified trace  entry.
export [[nodiscard]] inline
bioseq::storage::sequence_payload_allocation_observer
make_slice_payload_allocation_observer_noexcept(
    slice_trace_attachment *attachment,
    std::uint32_t producer_value_id,
    materialization_mechanism mechanism,
    materialization_reason reason) noexcept {
  if (!slice_trace_attachment_live(attachment)
      || !valid_mechanism(mechanism) || !valid_reason(reason)) {
    return {};
  }
  auto const &runtime = *attachment->runtime;
  const auto scope = get_current_slice_materialization_scope();
  slice_materialization_boundary const *boundary = nullptr;
  if (scope.attachment == attachment && scope.boundary_id != 0u
      && (producer_value_id == 0u
          || producer_value_id == scope.producer_value_id)) {
    boundary = find_boundary_by_id(runtime, scope.boundary_id);
    mechanism = scope.mechanism;
    reason = scope.reason;
    producer_value_id = scope.producer_value_id;
    if (boundary == nullptr
        || boundary->producer_value_id != producer_value_id
        || boundary->mechanism != mechanism
        || boundary->reason != reason) {
      return {};
    }
  } else {
    const bool caller_classification =
        scope.attachment == nullptr && scope.boundary_id == 0u
        && scope.producer_value_id == 0u && scope.local_sink == nullptr
        && mechanism == materialization_mechanism::packed_full
        && reason == materialization_reason::executor_copy
        && (scope.mechanism != materialization_mechanism::backing_copy
            || scope.reason != materialization_reason::executor_copy);
    if (caller_classification) {
      mechanism = scope.mechanism;
      reason = scope.reason;
    }
    boundary = producer_value_id == 0u
        ? find_unique_boundary_by_classification(runtime, mechanism, reason)
        : find_unique_boundary(
            runtime, producer_value_id, mechanism, reason);
  }
  if (boundary == nullptr) {
    return {};
  }
  return {
      .context = attachment,
      .publish = &publish_sequence_payload_allocation_noexcept,
      .correlation_a =
          (static_cast<std::uint64_t>(boundary->boundary_id) << 32u)
          | boundary->producer_value_id,
      .correlation_b =
          static_cast<std::uint64_t>(mechanism)
          | (static_cast<std::uint64_t>(reason) << 8u),
  };
}

/* Tracing changes only bookkeeping. It can be attached to a read-only
 * expanded wrapper because the sequence's letters and tree stay unchanged. */
/// Set or replace an expanded sequence's trace attachment and producing value ID.
export [[nodiscard]] inline bool
attach_slice_trace_provenance(Datum expanded, slice_trace_attachment *trace,
                              std::uint32_t producer_value_id) noexcept {
  if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(expanded)) ||
      (trace != nullptr &&
       (!slice_trace_attachment_live(trace) || producer_value_id == 0u))) {
    return false;
  }
  ExpandedObjectHeader *header = DatumGetEOHP(expanded);
  if (header == nullptr ||
      header->eoh_methods != std::addressof(pg_sequence_expanded_methods())) {
    return false;
  }
  auto *value = reinterpret_cast<pg_sequence_expanded *>(header);
  if (value->trace != trace) {
    retain_slice_trace_attachment(trace);
    auto *previous = value->trace;
    value->trace = trace;
    release_slice_trace_attachment(previous);
  }
  value->producer_value_id = trace == nullptr ? 0u : producer_value_id;
  return true;
}

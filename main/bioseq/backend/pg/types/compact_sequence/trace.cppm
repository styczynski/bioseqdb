/* Fixed, allocation-free materialization tracing shared by type and plan code. */
module;

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <type_traits>

#include "headers/pg_compat.h"
#include "fmgr_boundary.h"

export module bioseq.types.pg_compact_sequence:trace;

import bioseq.parallelism.pool;
import bioseq.storage.lazy;

export enum class materialization_mechanism : std::uint8_t {
    backing_copy = 0u,
    packed_full,
    packed_region,
    text,
    binary,
    letter_array,
};

export enum class materialization_reason : std::uint8_t {
    executor_copy = 0u,
    heap_tuple,
    index_tuple,
    mmsearch_target,
    mmsearch_query,
    resident_algorithm,
    sql_conversion,
    client_output,
};

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

export [[nodiscard]] inline bool slice_dsm_counter_reserved_valid(
    slice_dsm_counter const& counter) noexcept
{
    return std::all_of(
        counter.reserved.begin(), counter.reserved.end(),
        [](std::byte value) { return value == std::byte{0}; });
}

export inline void reset_slice_dsm_counter_noexcept(
    slice_dsm_counter& counter) noexcept
{
    std::memset(std::addressof(counter), 0, sizeof(counter));
}

namespace {

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

export struct slice_counter_sink {
    slice_dsm_counter* entries;
    std::uint32_t entry_count;
};

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

export struct slice_async_owner_guard;

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

export [[nodiscard]] inline bool
slice_trace_attachment_live(slice_trace_attachment const *attachment) noexcept {
  return attachment != nullptr && attachment->refcount != 0u &&
         attachment->valid && attachment->runtime != nullptr &&
         !attachment->runtime->invalid;
}

export inline void
retain_slice_trace_attachment(slice_trace_attachment *attachment) noexcept {
  if (attachment == nullptr || attachment->refcount == 0u) {
    return;
  }
  if (attachment->refcount == UINT32_MAX) {
    /* Prefer a bounded leak to wrapping into a use-after-free. */
    attachment->valid = false;
    attachment->runtime = nullptr;
    return;
  }
  ++attachment->refcount;
}

export inline void
release_slice_trace_attachment(slice_trace_attachment *attachment) noexcept {
  if (attachment == nullptr || attachment->refcount == 0u ||
      attachment->refcount == UINT32_MAX) {
    return;
  }
  if (--attachment->refcount == 0u) {
    pfree(attachment);
  }
}

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
  using namespace std::chrono;
  const auto value =
      duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
          .count();
  return value <= 0 ? 1u : static_cast<std::uint64_t>(value);
}

} // namespace

/* Keep trace TLS behind module-owned, out-of-line accessors.  Anonymous-
 * namespace inline TLS variables are replicated in every importer that
 * instantiates these exported inline helpers, which disconnects a scope set
 * in one TU from an observation in another and makes address identity
 * importer-local. */
export [[nodiscard, gnu::noinline]] slice_materialization_scope&
current_slice_materialization_scope_storage_noexcept() noexcept {
  static thread_local slice_materialization_scope scope{};
  return scope;
}

/* The token is shared by all calls on one thread while remaining distinct on
 * a worker thread, preserving the off-backend tripwire. */
export [[nodiscard, gnu::noinline]] void const*
current_slice_async_owner_thread_identity_noexcept() noexcept {
  static thread_local std::byte token{};
  return std::addressof(token);
}

export [[nodiscard]] inline slice_materialization_scope
get_current_slice_materialization_scope() noexcept {
  return current_slice_materialization_scope_storage_noexcept();
}

export [[nodiscard]] inline slice_materialization_scope
exchange_slice_materialization_scope(
    slice_materialization_scope next) noexcept {
  auto& active = current_slice_materialization_scope_storage_noexcept();
  const auto previous = active;
  active = next;
  return previous;
}

export inline void restore_slice_materialization_scope(
    slice_materialization_scope previous) noexcept {
  current_slice_materialization_scope_storage_noexcept() = previous;
}

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
      /* An asynchronous job owns an isolated full counter sink, not one
       * plan boundary. Resolve each observation exactly as the local
       * executor sink would, then publish into the job-local delta. */
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

/* One allocation/admission attempt owns one terminal token.  Callers publish
 * it only after the allocation and complete touch have succeeded; ERROR,
 * cancellation and early-stop paths abandon it.  The terminal bit makes a
 * repeated cleanup/commit callback harmless and keeps Actual Calls/Bytes
 * bijective with completed materializations.  Tokens are call-lifetime views
 * and deliberately do not retain the query-owned attachment. */
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

export [[nodiscard]] inline bool abandon_slice_materialization_noexcept(
    slice_materialization_admission &admission) noexcept {
  if (admission.terminal) {
    return false;
  }
  admission.terminal = true;
  return true;
}

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
               admitted_packed_region
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

/* Fixed scheduler-independent owner state.  Concrete executors install only
 * preconstructed, no-throw callbacks; no state transition allocates. */
export enum class slice_async_owner_phase : std::uint8_t {
  accepting = 0u,
  quiescing,
  quiesced,
  terminal,
};

export inline constexpr std::uint32_t slice_async_max_live_jobs = 2u;

export enum class slice_async_job_state : std::uint8_t {
  unused = 0u,
  enrolled,
  scheduled,
  finished,
  joined,
  merged,
  released,
};

export enum class slice_shutdown_disposition : std::uint8_t {
  complete = 0u,
  incomplete,
};

export struct slice_async_completion_handle {
  std::uint32_t slot_index;
  std::uint64_t generation;
};

export struct slice_noexcept_callback {
  void *argument;
  void (*invoke)(void *) noexcept;
};

export using preconstructed_cancel_handle = slice_noexcept_callback;
export using preconstructed_join_handle = slice_noexcept_callback;
export using retained_job_backings = slice_noexcept_callback;

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

export struct slice_async_job_resources {
  preconstructed_cancel_handle cancel;
  preconstructed_join_handle join;
  retained_job_backings backings;
};

export struct slice_async_job_slot {
  std::uint64_t generation;
  /* Executor-thread owned.  Workers publish only counters/error bytes and
   * return the immutable completion handle through the scheduler. */
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
  /* Validate the entire delta and destination before committing any prefix.
   * This mirrors the DSM two-pass merge contract: malformed accounting is
   * visible as invalid/overflowed, never as a plausible partial success. */
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

/* A scheduler installs this scope in the concrete worker submission.  The
 * slot owns a private delta sink, so worker observations never race the
 * executor/DSM cumulative counters. */
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

/* Backend-thread-only generation-checked owner recovery for upper consumers
 * (notably BatchSort) that receive a traced EOH rather than a downward
 * executor binding. */
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

export [[nodiscard]] inline bool mark_slice_async_job_finished_noexcept(
    slice_async_owner_guard *guard,
    slice_async_completion_handle handle) noexcept {
  /* Called by the executor only after scheduler completion publication;
   * workers never read or write job.state. */
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
    /* A scheduler rejection is still a visible incomplete job use. */
    forced_incomplete = true;
    job.state = slice_async_job_state::scheduled;
  }
  if (job.state == slice_async_job_state::scheduled) {
    /* Reaching drain before executor-side completion publication means
     * shutdown/early-stop had to cancel this use.  It cannot be reported
     * as a complete cycle even when cancellation and join both succeed. */
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
    outcome_out->incomplete = outcome_out->incomplete || incomplete;
    outcome_out->failed = outcome_out->failed || error_published;
    if (error_published) {
      outcome_out->sqlstate = job.error.sqlstate;
      outcome_out->message_length = job.error.message_length;
      std::memcpy(outcome_out->message.data(), job.error.message.data(),
                  job.error.message.size());
    }
  }
  if (job.state == slice_async_job_state::joined) {
    /* A job-local sink is a tentative delta until the worker has published a
     * complete outcome.  Cancellation, ERROR, scheduler rejection, and an
     * early drain can all leave a valid-looking prefix in that sink; merging
     * it would invent a completed materialization in Actual Calls/Bytes. */
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

export inline void slice_async_owner_reset(void *argument) noexcept {
  finish_slice_async_owner_noexcept(
      static_cast<slice_async_owner_guard *>(argument),
      slice_shutdown_disposition::incomplete);
}

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

/* Integration seam for the pure fixed-record/state-machine invariants.  It
 * uses only stack-owned POD and never installs query-visible trace state. */
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
      bioseq::storage::observe_sequence_payload_allocation_noexcept(
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
      [&](std::size_t index) noexcept {
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
      (void)bioseq::storage::observe_sequence_payload_allocation_noexcept(
          std::addressof(probe->observer),
          bioseq::storage::sequence_payload_allocation_kind::flat_sequence,
          1u, 1u, 1u, 1u);
    }
  };
  const auto observer =
      make_slice_async_pool_observer_noexcept(std::addressof(guard));
  std::array<bioseq::parallelism::tracked_submit_status, 2> probe_statuses{};
  auto &pool = bioseq::parallelism::GlobalPool::instance();
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
    valid = valid && outcome.drained && !outcome.incomplete && !outcome.failed;
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
          failed_outcome.incomplete && failed_outcome.failed &&
          failed_outcome.sqlstate == 123u && guard.enrolled_jobs == 0u &&
          attachment.refcount == 32u && cumulative.calls == 128u &&
          cumulative.bytes == 1024u;
  restore_slice_materialization_scope(original_scope);
  return valid;
}

export inline Datum slice_trace_self_test_impl(FunctionCallInfo) {
  PG_RETURN_BOOL(slice_trace_self_test_noexcept());
}

extern "C" {
CXX_PG_FUNCTION_INFO_V1(slice_trace_self_test);
PGDLLEXPORT Datum slice_trace_self_test(PG_FUNCTION_ARGS)
{
    return bioseq_invoke_pg_sequence_fmgr_boundary(&slice_trace_self_test_impl, fcinfo);
}
} // extern "C"

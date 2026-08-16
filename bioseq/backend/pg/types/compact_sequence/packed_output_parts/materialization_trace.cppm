/**
 * @file materialization_trace.cppm
 * @brief Fixed-record materialization tracing and attachment ownership.
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

import :flat_abi;
import :source_abi;
import :tree_types;
import bioseq.storage.lazy_sequence;

/// Mechanism that produced one compact-sequence materialization.
export enum class materialization_mechanism : std::uint8_t {
    backing_copy = 0u,
    packed_full,
    packed_region,
    text,
    binary,
    letter_array,
};

/// Consumer boundary that required a compact-sequence materialization.
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

#ifdef BIOSEQ_TEST_INDEXES
/* Independent allocator-side allocation evidence. These counters are not
 * plan instrumentation: allocation sites increment them only after a payload
 * allocation succeeds.  The global totals remain truthful for intermediate
 * expression adoption, while the publication deltas isolate the shallow
 * SliceProject slot-copy block.  Backend-local storage is intentional; the
 * SQL probe is used only around one query on one integration-test connection. */
/// Backend-local allocation totals captured around one slice publication.
export struct slice_payload_allocation_test_snapshot {
    std::uint64_t packed_full{};
    std::uint64_t executor_copy{};
};

/// Mutable allocation counters backing the integration-test snapshot.
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

/// Capture successful full-pack and executor-copy allocation totals.
export [[nodiscard]] inline slice_payload_allocation_test_snapshot
capture_slice_payload_allocation_test_snapshot_noexcept() noexcept
{
    auto const& counters = slice_payload_allocation_test_storage_noexcept();
    return {
        .packed_full = counters.packed_full,
        .executor_copy = counters.executor_copy,
    };
}

/// Accumulate allocation deltas observed during one result-slot publication.
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

/// Fixed-layout materialization counter shared through PostgreSQL DSM.
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

/// Check that all reserved DSM counter bytes retain their zero value.
export [[nodiscard]] inline bool slice_dsm_counter_reserved_valid(
    slice_dsm_counter const& counter) noexcept
{
    return std::all_of(
        counter.reserved.begin(), counter.reserved.end(),
        [](std::byte value) { return value == std::byte{0}; });
}

/// Reset a DSM counter to its zero wire representation.
export inline void reset_slice_dsm_counter_noexcept(
    slice_dsm_counter& counter) noexcept
{
    std::memset(std::addressof(counter), 0, sizeof(counter));
}

namespace {

/// Add a counter field and set the shared overflow marker on saturation.
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

/// Saturating-merge one worker counter into its coordinator counter.
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

/// Add one completed materialization to a DSM counter.
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

/// Optional local or DSM destination for materialization counters.
export struct slice_counter_sink {
    slice_dsm_counter* entries;
    std::uint32_t entry_count;
};

/// Immutable classification and counters for one traced consumer boundary.
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

/// Backend-owned fixed-capacity runtime for materialization tracing.
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

/// Initialize a trace runtime in the supplied PostgreSQL memory context.
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

/// Destroy trace-runtime allocations without raising PostgreSQL errors.
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

/// Memory-context owner for traced asynchronous jobs.
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

/// Allocate one trace attachment and retain its runtime association.
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

/// Check whether an attachment still names its original live runtime generation.
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
    /* Prefer a bounded leak to wrapping into a use-after-free. */
    attachment->valid = false;
    attachment->runtime = nullptr;
    return;
  }
  ++attachment->refcount;
}

/// Release one attachment reference and free the final attachment.
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

/// Detach an attachment from a runtime that is being destroyed.
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

/// Thread-local tracing scope propagated into synchronous or asynchronous work.
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

/* Keep trace TLS behind module-owned, out-of-line accessors.  Anonymous-
 * namespace inline TLS variables are replicated in every importer that
 * instantiates these exported inline helpers, which disconnects a scope set
 * in one TU from an observation in another and makes address identity
 * importer-local. */
/// Return the backend-thread materialization scope storage.
export [[nodiscard, gnu::noinline]] slice_materialization_scope&
current_slice_materialization_scope_storage_noexcept() noexcept {
  static thread_local slice_materialization_scope scope{};
  return scope;
}

/* The token is shared by all calls on one thread while remaining distinct on
 * a worker thread, preserving the off-backend tripwire. */
/// Return a stable identity token for asynchronous work owned by this thread.
export [[nodiscard, gnu::noinline]] void const*
current_slice_async_owner_thread_identity_noexcept() noexcept {
  static thread_local std::byte token{};
  return std::addressof(token);
}

/// Read the current backend-thread materialization scope.
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

/// Start monotonic timing when tracing is active.
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

/// Atomically validate and publish one traced materialization observation.
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
/// Single-use token for publishing or abandoning one admitted materialization.
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

/// Admit one materialization against its unique traced boundary.
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

/// Publish one admitted materialization exactly once.
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

/// Consume an admission token without publishing counters.
export [[nodiscard]] inline bool abandon_slice_materialization_noexcept(
    slice_materialization_admission &admission) noexcept {
  if (admission.terminal) {
    return false;
  }
  admission.terminal = true;
  return true;
}

/// Record one successful materialization in the active trace scope.
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

/// Translate a storage allocation event into its traced boundary counter.
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

/// Build an allocation observer bound to one unique materialization boundary.
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

/* Trace metadata is observational and may be installed on an otherwise
 * read-only EOH wrapper without changing its logical sequence value. */
/// Attach trace provenance when the expanded value has no conflicting owner.
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

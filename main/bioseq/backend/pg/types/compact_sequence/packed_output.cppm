/* Expanded-object ownership and the sole bioseq::pg_types::seq flattening boundary. */
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

#include "headers/pg_compat.h"
extern "C" {
#include "storage/shmem.h"
#include "utils/datum.h"
#include "utils/expandeddatum.h"
}

export module bioseq.types.pg_compact_sequence:packed_output;

import bioseq.codegen.sql.annotation_model;
import :flat_abi;
import :source_abi;
import :tree_types;
import :tree_ops;
import :view_session;
import bioseq.pg_utils;
import bioseq.parallelism.pool;
import bioseq.sequences.rank_pack;
import bioseq.storage.coordinates;
import bioseq.storage.lazy;

/* Fixed, allocation-free materialization tracing and provenance accounting.
 * This belongs to the sole packed-output/materialization boundary; the tree
 * POD partition carries only a pointer-only forward declaration. */
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

#ifdef BIOSEQ_TEST_INDEXES
/* Independent allocator-side evidence for §17.5.  These counters are not
 * plan instrumentation: allocation sites increment them only after a payload
 * allocation succeeds.  The global totals remain truthful for intermediate
 * expression adoption, while the publication deltas isolate the shallow
 * SliceProject slot-copy block.  Backend-local storage is intentional; the
 * SQL probe is used only around one query on one integration-test connection. */
export struct slice_payload_allocation_test_snapshot {
    std::uint64_t packed_full{};
    std::uint64_t executor_copy{};
};

struct slice_payload_allocation_test_counters {
    std::uint64_t packed_full{};
    std::uint64_t executor_copy{};
    std::uint64_t publication_packed_full{};
    std::uint64_t publication_executor_copy{};
};

[[nodiscard]] slice_payload_allocation_test_counters&
slice_payload_allocation_test_storage_noexcept() noexcept
{
    static thread_local slice_payload_allocation_test_counters counters{};
    return counters;
}

export [[nodiscard]] inline slice_payload_allocation_test_snapshot
capture_slice_payload_allocation_test_snapshot_noexcept() noexcept
{
    auto const& counters = slice_payload_allocation_test_storage_noexcept();
    return {
        .packed_full = counters.packed_full,
        .executor_copy = counters.executor_copy,
    };
}

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
  auto &pool = bioseq::parallelism::GlobalPool::instance();
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
          failed_outcome.incomplete && failed_outcome.failed &&
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
      && publication_outcome.drained && publication_outcome.incomplete
      && !publication_outcome.failed
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

export namespace bioseq::pg_types::sql_routines {

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::execute,
    .from = bioseq::sql::public_role,
})]]
bool slice_trace_self_test() noexcept {
  return slice_trace_self_test_noexcept();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .parallel = bioseq::sql::parallel_safety::restricted,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::execute,
    .from = bioseq::sql::public_role,
})]]
std::int64_t slice_payload_allocation_probe(std::int32_t action) {
#ifdef BIOSEQ_TEST_INDEXES
  auto& counters = slice_payload_allocation_test_storage_noexcept();
  switch (action) {
    case 0:
      counters = {};
      return 0;
    case 1:
      return static_cast<std::int64_t>(std::min(
          counters.packed_full,
          static_cast<std::uint64_t>(
              (std::numeric_limits<std::int64_t>::max)())));
    case 2:
      return static_cast<std::int64_t>(std::min(
          counters.executor_copy,
          static_cast<std::uint64_t>(
              (std::numeric_limits<std::int64_t>::max)())));
    case 3:
      return static_cast<std::int64_t>(std::min(
          counters.publication_packed_full,
          static_cast<std::uint64_t>(
              (std::numeric_limits<std::int64_t>::max)())));
    case 4:
      return static_cast<std::int64_t>(std::min(
          counters.publication_executor_copy,
          static_cast<std::uint64_t>(
              (std::numeric_limits<std::int64_t>::max)())));
    default:
      ereport(ERROR,
              (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
               errmsg("invalid slice payload-allocation probe action"),
               errdetail("expected 0 (reset), 1 (packed full), or 2 "
                         "(executor copy), 3 (publication packed full), or 4 "
                         "(publication executor copy); received %d", action)));
  }
#else
  ereport(ERROR,
          (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
           errmsg("slice payload-allocation probe is unavailable"),
           errdetail("The extension was built without BIOSEQ_TEST_INDEXES.")));
#endif
  pg_unreachable();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::execute,
    .from = bioseq::sql::public_role,
})]]
bool slice_trace_worker_failure_probe(std::int32_t action) {
#ifdef BIOSEQ_TEST_INDEXES
  static_cast<void>(action);
  if (IsParallelWorker()) {
    ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("injected SliceProject parallel worker failure"),
             errdetail("the original worker ERROR must survive DSM cleanup")));
  }
  return true;
#else
  static_cast<void>(action);
  ereport(ERROR,
          (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
           errmsg("SliceProject worker failure probe is unavailable"),
           errdetail("The extension was built without BIOSEQ_TEST_INDEXES.")));
#endif
  pg_unreachable();
}

}  // namespace bioseq::pg_types::sql_routines

export [[nodiscard]] inline std::uint64_t checked_ceil_div(
    std::uint64_t numerator,
    std::uint64_t denominator)
{
    if (denominator == 0u) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("zero divisor in bioseq size calculation")));
    }
    return numerator / denominator + (numerator % denominator != 0u);
}

export [[nodiscard]] inline Size checked_size_cast(std::uint64_t value)
{
    if (value > std::numeric_limits<Size>::max()) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq size does not fit PostgreSQL Size")));
    }
    return static_cast<Size>(value);
}

export [[nodiscard]] inline Size checked_mul_size(Size left, Size right)
{
    return mul_size(left, right);
}

export [[nodiscard]] inline Size checked_add_size(Size left, Size right)
{
    return add_size(left, right);
}

/* PostgreSQL's palloc accepts Size, but rejects requests above MaxAllocSize
 * with an internal allocation error.  Every concrete sequence conversion
 * must turn that condition into the public PROGRAM_LIMIT_EXCEEDED contract
 * before it asks a varlena, StringInfo, array, or flat pgseq allocator to
 * grow.  Comparing the uint64 payload before narrowing also covers size_t /
 * Size overflow on narrower PostgreSQL builds. */
export [[nodiscard]] inline Size checked_materialization_allocation_size(
    std::uint64_t payload_bytes,
    Size overhead_bytes,
    char const* target)
{
    if (overhead_bytes > MaxAllocSize
        || payload_bytes > static_cast<std::uint64_t>(
            MaxAllocSize - overhead_bytes)) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq %s materialization exceeds MaxAllocSize",
                        target)));
    }
    return overhead_bytes + static_cast<Size>(payload_bytes);
}

export [[nodiscard]] inline Size checked_round_up_to_uint64(Size value)
{
    constexpr Size alignment = sizeof(std::uint64_t);
    if (value > std::numeric_limits<Size>::max() - (alignment - 1u)) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq aligned payload size overflow")));
    }
    return round_up_to_uint64(value);
}

export template<class Instance>
[[nodiscard]] inline Size bound_flat_size(std::uint64_t bp_length)
{
    using range = compact_sequence_range<Instance>;
    using traits = typename range::traits;
    if (bp_length > std::numeric_limits<bioseq::pg_types::seq::bp_size_t>::max()) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq sequence is too long to flatten")));
    }
    const auto units = checked_ceil_div(
        bp_length, traits::letters_per_unit);
    if (units > std::numeric_limits<std::uint32_t>::max()) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq packed unit count exceeds uint32")));
    }
    const Size payload = checked_mul_size(
        checked_size_cast(units), sizeof(typename traits::storage_unit));
    const Size total = checked_materialization_allocation_size(
        checked_round_up_to_uint64(payload), bioseq::pg_types::seq::header_bytes(),
        "flat sequence");
    return total;
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
            []<class Instance>() {
              (void)bound_flat_size<Instance>(
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

[[nodiscard]] bool run_materialization_size_guardrail_matrix() {
  Size zero_flat_size = 0u;
  bioseq::pg_types::seq::dispatch_instance(
      pgseq_supported_alphabets::index_of<seq::dna4>(), 8u,
      [&zero_flat_size]<class Instance>() {
        zero_flat_size = bound_flat_size<Instance>(0u);
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

[[nodiscard]] inline Size expanded_flat_size(pg_sequence_expanded const& value)
{
    if (!value.alphabet_bound) {
        if (value.bp_length
            > std::numeric_limits<bioseq::pg_types::seq::bp_size_t>::max()) {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("unbound bioseq sequence is too long to flatten")));
        }
        const Size total = checked_materialization_allocation_size(
            checked_round_up_to_uint64(checked_size_cast(value.bp_length)),
            bioseq::pg_types::seq::header_bytes(), "flat unbound sequence");
        return total;
    }
    Size size = 0u;
    bioseq::pg_types::seq::dispatch_instance(
        value.type_params.alphabet_code,
        value.type_params.storage_unit_byte_width,
        [&size, &value]<class Instance>() {
            size = bound_flat_size<Instance>(value.bp_length);
        });
    return size;
}

[[nodiscard]] bool emit_unbound_ascii(
    piece_edge edge,
    std::uint64_t from,
    std::uint64_t to,
    std::span<std::byte> output)
{
    if (from > to || output.size() != to - from) {
        return false;
    }
    if (!edge) {
        return from == 0 && to == 0 && output.empty();
    }
    if (to > edge.node->bp_length
        || bioseq::storage::has_complement(edge.transform)) {
        return false;
    }
    if (from == to) {
        return true;
    }
    if (edge.node->node_kind == piece_node::kind::branch) {
        const auto children = logical_children(edge);
        const std::uint64_t split = children.first.node->bp_length;
        const std::uint64_t first_from = std::min(from, split);
        const std::uint64_t first_to = std::min(to, split);
        std::size_t emitted = 0u;
        if (first_to > first_from) {
            emitted = static_cast<std::size_t>(first_to - first_from);
            if (!emit_unbound_ascii(
                    children.first, first_from, first_to,
                    output.first(emitted))) {
                return false;
            }
        }
        if (to > split) {
            return emit_unbound_ascii(
                children.second,
                from > split ? from - split : 0u,
                to - split,
                output.subspan(emitted));
        }
        return true;
    }
    if (edge.node->node_kind != piece_node::kind::literal) {
        return false;
    }
    const auto& literal = edge.node->payload.literal;
    if (literal.encoding != literal_encoding::unbound_ascii) {
        return false;
    }
    const bool reverse = bioseq::storage::has_reverse(edge.transform);
    for (std::uint64_t index = from; index < to; ++index) {
        const std::uint64_t physical = !reverse
            ? literal.symbol_from + index
            : literal.symbol_to - 1u - index;
        output[static_cast<std::size_t>(index - from)] =
            literal.bytes[physical];
    }
    return true;
}

[[noreturn]] inline void rank_pack_error(
    bioseq::sequences::rank_pack_status status)
{
    using bioseq::sequences::rank_pack_status;
    switch (status) {
        case rank_pack_status::invalid_rank:
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq tree emitted an invalid alphabet rank")));
            __builtin_unreachable();
        case rank_pack_status::capacity_overflow:
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("bioseq packed output capacity overflow")));
            __builtin_unreachable();
        case rank_pack_status::length_mismatch:
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq packed output length mismatch")));
            __builtin_unreachable();
        case rank_pack_status::unit_count_mismatch:
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq packed output unit-count mismatch")));
            __builtin_unreachable();
        case rank_pack_status::ok:
            break;
    }
    __builtin_unreachable();
}

template<class Instance, class View>
void pack_view_into(bioseq::pg_types::seq& destination, View view, std::uint64_t length)
{
    using range = compact_sequence_range<Instance>;
    using traits = typename range::traits;
    using unit = typename traits::storage_unit;
    const std::uint64_t unit_count = checked_ceil_div(
        length, traits::letters_per_unit);
    auto units = std::span<unit>{
        reinterpret_cast<unit*>(destination.data),
        static_cast<std::size_t>(unit_count)};
    using sink = bioseq::sequences::fixed_rank_pack_unit_sink<unit>;
    using writer_type = bioseq::sequences::checked_rank_pack_sink<
        Instance,
        bioseq::sequences::invalid_rank_policy::reject,
        bioseq::sequences::pack_capacity_policy::fixed_span,
        sink>;
    writer_type writer{sink{units}};
    bioseq::storage::stream_transformed_packed<
        bioseq::storage::sequence_transform::identity>(
            view, 0u, length, writer);
    const auto result = writer.finish(length, unit_count);
    if (!result) {
        rank_pack_error(result.status);
    }
    destination.alphabet_code =
        pgseq_supported_alphabets::template index_of<
            typename Instance::alphabet_type>();
    destination.storage_unit_byte_width = Instance::byte_width;
    destination.padding_letters = result.padding_letters;
    destination.bp_size = static_cast<bioseq::pg_types::seq::bp_size_t>(length);
}

inline void pg_sequence_expanded_reset(void* argument) noexcept
{
    auto* value = static_cast<pg_sequence_expanded*>(argument);
    if (value == nullptr) {
        return;
    }
    piece_edge root = value->root;
    value->root = {};
    release_edge(root);
    auto* trace = value->trace;
    value->trace = nullptr;
    release_slice_trace_attachment(trace);
}

} // namespace

export [[nodiscard]] inline bool emit_pg_unbound_ascii(
    piece_edge root,
    std::uint64_t from,
    std::uint64_t to,
    std::span<std::byte> output)
{
    return emit_unbound_ascii(root, from, to, output);
}

export [[nodiscard]] inline Size pg_sequence_get_flat_size(
    ExpandedObjectHeader* header)
{
    auto* value = reinterpret_cast<pg_sequence_expanded*>(header);
    return expanded_flat_size(*value);
}

export inline void pg_sequence_flatten_into(
    ExpandedObjectHeader* header,
    void* result,
    Size allocated_size)
{
    auto& value = *reinterpret_cast<pg_sequence_expanded*>(header);
    /* Persistent concat validates only its newly allocated AVL path.  A flat
     * payload is already O(total symbols), so retain the independent full-tree
     * audit at this external materialization boundary. */
    validate_tree_and_limits(
        value.root, value.type_params, value.bp_length,
        value.alphabet_bound);
    const Size expected = expanded_flat_size(value);
    if (allocated_size != expected) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq EOH flatten size changed between callbacks")));
    }
    std::memset(result, 0, allocated_size);
    auto& destination = *static_cast<bioseq::pg_types::seq*>(result);
    SET_VARSIZE(&destination, allocated_size);

    if (!value.alphabet_bound) {
        destination.alphabet_code = pgseq_unbound_alphabet_code;
        destination.storage_unit_byte_width = pgseq_unbound_width_tag;
        destination.padding_letters = 0u;
        destination.bp_size = static_cast<bioseq::pg_types::seq::bp_size_t>(value.bp_length);
        if (!emit_unbound_ascii(
                value.root, 0u, value.bp_length,
                std::span<std::byte>{
                    reinterpret_cast<std::byte*>(destination.data),
                    static_cast<std::size_t>(value.bp_length)})) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("unbound bioseq tree cannot be flattened")));
        }
#ifdef BIOSEQ_TEST_INDEXES
        note_slice_payload_allocation_test_event_noexcept(
            materialization_mechanism::packed_full,
            materialization_reason::executor_copy);
#endif
        const auto observer = make_slice_payload_allocation_observer_noexcept(
            value.trace, value.producer_value_id,
            materialization_mechanism::packed_full,
            materialization_reason::executor_copy);
        (void)bioseq::storage::observe_sequence_payload_allocation_noexcept(
            std::addressof(observer),
            bioseq::storage::sequence_payload_allocation_kind::flat_sequence,
            value.bp_length, value.bp_length,
            static_cast<std::uint64_t>(allocated_size),
            static_cast<std::uint64_t>(allocated_size));
        return;
    }

    pg_sequence_descriptor descriptor{
        .kind = pg_sequence_datum_kind::expanded_read_only,
        .alphabet_code = value.type_params.alphabet_code,
        .storage_unit_byte_width = value.type_params.storage_unit_byte_width,
        .alphabet_bound = true,
        .bp_length = value.bp_length,
    };
    ErrorData* callback_exact_error = nullptr;
    bool callback_standard_error = false;
    bool callback_unknown_error = false;
    char callback_detail[512] = "std::exception without detail";
    pg_sequence_read_session* volatile session = nullptr;
    PG_TRY();
    {
        auto* local_session = begin_pg_sequence_read_session(
            CurrentMemoryContext);
        session = local_session;
        try {
            bioseq::pg_types::seq::dispatch_instance(
                value.type_params.alphabet_code,
                value.type_params.storage_unit_byte_width,
                [&destination, &value, local_session, header,
                 descriptor]<class Instance>() {
                    using alphabet = typename Instance::alphabet_type;
                    auto* owner = make_pg_sequence_call_owner<alphabet>(
                        *local_session, EOHPGetRODatum(header), descriptor);
                    pack_view_into<Instance>(
                        destination, owner->view(), value.bp_length);
                });
            finish_pg_sequence_read_session(local_session, true);
        } catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
            callback_exact_error = error.release_error_data();
        } catch (std::exception const& error) {
            strlcpy(callback_detail,
                    error.what() == nullptr
                        ? "std::exception without detail"
                        : error.what(),
                    sizeof(callback_detail));
            callback_standard_error = true;
        } catch (...) {
            callback_unknown_error = true;
        }
        if (callback_exact_error != nullptr || callback_standard_error
            || callback_unknown_error) {
            abort_pg_sequence_read_session_noexcept(local_session);
        }
        session = nullptr;
    }
    PG_CATCH();
    {
        abort_pg_sequence_read_session_noexcept(
            const_cast<pg_sequence_read_session*>(session));
        session = nullptr;
        PG_RE_THROW();
    }
    PG_END_TRY();
    if (callback_exact_error != nullptr) {
        ReThrowError(callback_exact_error);
    }
    if (callback_standard_error || callback_unknown_error) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq sequence callback raised a C++ exception"),
                 errdetail("%s", callback_unknown_error
                         ? "unknown non-standard C++ exception"
                         : callback_detail)));
    }
#ifdef BIOSEQ_TEST_INDEXES
    note_slice_payload_allocation_test_event_noexcept(
        materialization_mechanism::packed_full,
        materialization_reason::executor_copy);
#endif
    const auto observer = make_slice_payload_allocation_observer_noexcept(
        value.trace, value.producer_value_id,
        materialization_mechanism::packed_full,
        materialization_reason::executor_copy);
    (void)bioseq::storage::observe_sequence_payload_allocation_noexcept(
        std::addressof(observer),
        bioseq::storage::sequence_payload_allocation_kind::flat_sequence,
        value.bp_length, value.bp_length,
        static_cast<std::uint64_t>(allocated_size),
        static_cast<std::uint64_t>(allocated_size));
}

export [[nodiscard, gnu::used, gnu::noinline]] ExpandedObjectMethods const&
pg_sequence_expanded_methods() noexcept
{
    static const ExpandedObjectMethods methods{
        &pg_sequence_get_flat_size,
        &pg_sequence_flatten_into,
    };
    return methods;
}

export [[nodiscard]] inline pg_sequence_expanded* make_pg_sequence_expanded(
    compact_sequence_stdc_type_params params,
    bool alphabet_bound)
{
    MemoryContext object_context = AllocSetContextCreate(
        CurrentMemoryContext, "bioseq lazy sequence", ALLOCSET_SMALL_SIZES);
    auto* result = static_cast<pg_sequence_expanded*>(
        MemoryContextAllocZero(object_context, sizeof(pg_sequence_expanded)));
    result->object_context = object_context;
    result->type_params = params;
    result->alphabet_bound = alphabet_bound;
    EOH_init_header(
        &result->hdr, &pg_sequence_expanded_methods(), object_context);
    result->reset_callback.func = pg_sequence_expanded_reset;
    result->reset_callback.arg = result;
    MemoryContextRegisterResetCallback(
        object_context, &result->reset_callback);
    return result;
}

export [[nodiscard]] inline Datum pg_sequence_rw_datum(
    pg_sequence_expanded* value) noexcept
{
    return EOHPGetRWDatum(&value->hdr);
}

export [[nodiscard]] inline Datum pg_sequence_ro_datum(
    pg_sequence_expanded* value) noexcept
{
    return EOHPGetRODatum(&value->hdr);
}

export [[nodiscard]] inline slice_trace_attachment*
pg_sequence_trace_attachment_for_datum(Datum input)
{
    auto const* expanded = get_pg_sequence_expanded_ro(input);
    return expanded == nullptr ? nullptr : expanded->trace;
}

export struct pending_root_cleanup {
    MemoryContextCallback callback;
    piece_tree_arena* destination_arena;
    piece_edge_cleanup_ledger build_edges;
    piece_edge root;
    piece_edge_cleanup_handle root_cleanup;
    bool armed;
};

export inline void pending_root_reset(void* argument) noexcept
{
    auto* guard = static_cast<pending_root_cleanup*>(argument);
    if (guard == nullptr || !guard->armed) {
        return;
    }
    guard->armed = false;
    release_piece_edge_cleanup_ledger_noexcept(guard->build_edges);
    release_edge(guard->root);
    guard->root = {};
    release_arena(guard->destination_arena);
    guard->destination_arena = nullptr;
}

template<class Build>
[[nodiscard]] inline tracked_piece_edge invoke_piece_root_build(
    Build&& build,
    piece_tree_arena& arena,
    piece_edge_cleanup_ledger& ledger)
{
    /* A root builder must publish every returned owning edge to the reset
     * callback's ledger before it may perform another PostgreSQL operation.
     * Requiring the tracked return type makes that contract structural. */
    tracked_piece_edge built = std::forward<Build>(build)(arena, ledger);
    return built;
}

inline void transfer_built_root_from_ledger(
    pending_root_cleanup& guard)
{
    if (guard.root_cleanup.generation != 0u) {
        transfer_piece_edge_cleanup_root_noexcept(
            guard.build_edges, guard.root_cleanup, guard.root);
        guard.root_cleanup = {};
    }
    if (!piece_edge_cleanup_ledger_empty_noexcept(guard.build_edges)) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq concat construction left unowned edge tokens"),
                 errdetail("token_count=%u",
                           static_cast<unsigned>(
                               guard.build_edges.token_count))));
    }
}

export template<class Build>
inline void replace_root_error_safe(
    pg_sequence_expanded& value,
    compact_sequence_stdc_type_params type,
    std::uint64_t length,
    bool alphabet_bound,
    Build&& build)
{
    MemoryContext build_context = AllocSetContextCreate(
        CurrentMemoryContext, "bioseq piece build", ALLOCSET_SMALL_SIZES);
    MemoryContext saved_context = CurrentMemoryContext;
    PG_TRY();
    {
        MemoryContextSwitchTo(build_context);
        auto* guard = static_cast<pending_root_cleanup*>(
            palloc0(sizeof(pending_root_cleanup)));
        guard->callback.func = pending_root_reset;
        guard->callback.arg = guard;
        guard->armed = true;
        MemoryContextRegisterResetCallback(
            build_context, &guard->callback);
        MemoryContextSwitchTo(saved_context);

        /* Arena storage is independent of both caller and EOH contexts so a
         * second EOH can retain/share it after this EOH is reset. */
        guard->destination_arena = make_piece_tree_arena();
        const tracked_piece_edge built = invoke_piece_root_build(
            std::forward<Build>(build), *guard->destination_arena,
            guard->build_edges);
        guard->root = built.edge;
        guard->root_cleanup = built.cleanup;
        transfer_built_root_from_ledger(*guard);
        validate_tree_and_limits(
            guard->root, type, length, alphabet_bound);

        piece_edge old = value.root;
        publish_root_noexcept(
            value, guard->root, type, length, alphabet_bound);
        guard->root = {};
        release_arena(guard->destination_arena);
        guard->destination_arena = nullptr;
        guard->armed = false;
        MemoryContextDelete(build_context);
        release_edge(old);
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_context);
        MemoryContextDelete(build_context);
        PG_RE_THROW();
    }
    PG_END_TRY();
}

/* Error-safe publication for the one operation whose existing inputs are
 * immutable, previously validated roots.  The destination arena contains only
 * the height-guided concat path, so validation remains O(log pieces) while
 * preserving the same publish-after-validation and unwind ownership contract
 * as replace_root_error_safe. */
template<class Build>
inline void replace_root_trusted_join_error_safe(
    pg_sequence_expanded& value,
    compact_sequence_stdc_type_params type,
    std::uint64_t length,
    bool alphabet_bound,
    piece_edge trusted_left,
    std::uint64_t left_length,
    piece_edge trusted_right,
    std::uint64_t right_length,
    Build&& build)
{
    MemoryContext build_context = AllocSetContextCreate(
        CurrentMemoryContext, "bioseq trusted join build", ALLOCSET_SMALL_SIZES);
    MemoryContext saved_context = CurrentMemoryContext;
    PG_TRY();
    {
        MemoryContextSwitchTo(build_context);
        auto* guard = static_cast<pending_root_cleanup*>(
            palloc0(sizeof(pending_root_cleanup)));
        guard->callback.func = pending_root_reset;
        guard->callback.arg = guard;
        guard->armed = true;
        MemoryContextRegisterResetCallback(
            build_context, &guard->callback);
        MemoryContextSwitchTo(saved_context);

        guard->destination_arena = make_piece_tree_arena();
        const tracked_piece_edge built = invoke_piece_root_build(
            std::forward<Build>(build), *guard->destination_arena,
            guard->build_edges);
        guard->root = built.edge;
        guard->root_cleanup = built.cleanup;
        transfer_built_root_from_ledger(*guard);
        (void)validate_trusted_join_and_limits(
            *guard->destination_arena, guard->root,
            trusted_left, left_length, trusted_right, right_length,
            type, length, alphabet_bound);

        piece_edge old = value.root;
        publish_root_noexcept(
            value, guard->root, type, length, alphabet_bound);
        guard->root = {};
        release_arena(guard->destination_arena);
        guard->destination_arena = nullptr;
        guard->armed = false;
        MemoryContextDelete(build_context);
        release_edge(old);
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_context);
        MemoryContextDelete(build_context);
        PG_RE_THROW();
    }
    PG_END_TRY();
}

namespace {

[[nodiscard]] inline literal_piece copy_flat_literal(
    piece_tree_arena& arena,
    bioseq::pg_types::seq const& flat,
    pg_sequence_descriptor descriptor,
    slice_trace_attachment* trace,
    std::uint32_t producer_value_id)
{
    const bool unbound = !descriptor.alphabet_bound;
    const std::size_t count = unbound
        ? static_cast<std::size_t>(descriptor.bp_length)
        : static_cast<std::size_t>(VARSIZE(&flat)) - bioseq::pg_types::seq::header_bytes();
    const std::size_t allocation_bytes = std::max<std::size_t>(count, 1u);
    auto* bytes = allocate_piece_literal_storage(
        arena, allocation_bytes);
    if (count != 0u) {
        std::memcpy(bytes, flat.data, count);
    }
#ifdef BIOSEQ_TEST_INDEXES
    note_slice_payload_allocation_test_event_noexcept(
        materialization_mechanism::backing_copy,
        materialization_reason::executor_copy);
#endif
    const auto observer = make_slice_payload_allocation_observer_noexcept(
        trace, producer_value_id,
        materialization_mechanism::backing_copy,
        materialization_reason::executor_copy);
    (void)bioseq::storage::observe_sequence_payload_allocation_noexcept(
        std::addressof(observer),
        bioseq::storage::sequence_payload_allocation_kind::backing_copy,
        descriptor.bp_length, descriptor.bp_length,
        static_cast<std::uint64_t>(allocation_bytes),
        static_cast<std::uint64_t>(allocation_bytes));
    return {
        .storage_arena = std::addressof(arena),
        .bytes = bytes,
        .symbol_from = 0u,
        .symbol_to = descriptor.bp_length,
        .stored_type_params = unbound
            ? compact_sequence_stdc_type_params{
                .alphabet_code = pgseq_unbound_alphabet_code,
                .storage_unit_byte_width = pgseq_unbound_width_tag,
              }
            : flat.get_type_params(),
        .encoding = unbound
            ? literal_encoding::unbound_ascii
            : literal_encoding::packed,
        .validation = unbound
            ? literal_validation::unvalidated
            : literal_validation::alphabet_validated,
    };
}

inline void reconcile_concat_trace_provenance_noexcept(
    pg_sequence_expanded& result,
    pg_sequence_expanded const& right) noexcept
{
    const bool left_live = slice_trace_attachment_live(result.trace)
        && result.producer_value_id != 0u;
    const bool right_live = slice_trace_attachment_live(right.trace)
        && right.producer_value_id != 0u;

    slice_trace_attachment* selected_trace = nullptr;
    std::uint32_t selected_producer = 0u;
    if (left_live && !right_live) {
        selected_trace = result.trace;
        selected_producer = result.producer_value_id;
    } else if (!left_live && right_live) {
        selected_trace = right.trace;
        selected_producer = right.producer_value_id;
    } else if (left_live && right_live
               && result.trace == right.trace
               && result.producer_value_id == right.producer_value_id) {
        selected_trace = result.trace;
        selected_producer = result.producer_value_id;
    }
    /* Two different live producer identities cannot be attributed to one
     * result without its planned result id.  Clear the observation metadata;
     * BioseqSliceProject installs the exact result provenance after evaluation. */
    if (result.trace != selected_trace) {
        retain_slice_trace_attachment(selected_trace);
        auto* previous = result.trace;
        result.trace = selected_trace;
        release_slice_trace_attachment(previous);
    }
    result.producer_value_id = selected_producer;
}

} // namespace

export [[nodiscard]] inline acquired_sequence acquire_writable_pg_sequence(
    Datum input,
    slice_trace_attachment const* trace = nullptr,
    std::uint32_t producer_value_id = 0u)
{
    if (auto* rw = get_pg_sequence_expanded_rw(input); rw != nullptr) {
        if (trace != nullptr && rw->trace != trace) {
            auto* replacement = const_cast<slice_trace_attachment*>(trace);
            retain_slice_trace_attachment(replacement);
            auto* previous = rw->trace;
            rw->trace = replacement;
            release_slice_trace_attachment(previous);
        }
        if (producer_value_id != 0u) {
            rw->producer_value_id = producer_value_id;
        }
        return {rw, true};
    }
    if (auto const* ro = get_pg_sequence_expanded_ro(input); ro != nullptr) {
        auto* copy = make_pg_sequence_expanded(
            ro->type_params, ro->alphabet_bound);
        copy->bp_length = ro->bp_length;
        copy->root = retain_edge(ro->root);
        copy->producer_value_id = producer_value_id != 0u
            ? producer_value_id : ro->producer_value_id;
        copy->trace = const_cast<slice_trace_attachment*>(
            trace != nullptr ? trace : ro->trace);
        retain_slice_trace_attachment(copy->trace);
        return {copy, false};
    }

    auto* flat = reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(input));
    const Datum flat_datum = PointerGetDatum(flat);
    const auto descriptor = classify_and_describe_sequence(flat_datum);
    if (descriptor.kind == pg_sequence_datum_kind::locator) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("cannot resolve a bioseq locator outside its owning table scan"),
                 errdetail("relation, column, snapshot, and cache ownership are unavailable")));
    }
    const compact_sequence_stdc_type_params type = descriptor.alphabet_bound
        ? flat->get_type_params()
        : compact_sequence_stdc_type_params{
            .alphabet_code = pgseq_unbound_alphabet_code,
            .storage_unit_byte_width = pgseq_unbound_width_tag,
          };
    auto* result = make_pg_sequence_expanded(type, descriptor.alphabet_bound);
    result->trace = const_cast<slice_trace_attachment*>(trace);
    result->producer_value_id = producer_value_id;
    retain_slice_trace_attachment(result->trace);
    replace_root_error_safe(
        *result, type, descriptor.bp_length, descriptor.alphabet_bound,
        [flat, descriptor, result](
            piece_tree_arena& arena,
            piece_edge_cleanup_ledger& ledger) {
            return make_literal_node_error_atomic(
                arena, ledger,
                copy_flat_literal(
                    arena, *flat, descriptor,
                    result->trace, result->producer_value_id));
        });
    if (reinterpret_cast<Pointer>(flat) != DatumGetPointer(input)) {
        pfree(flat);
    }
    return {result, false};
}

export [[nodiscard]] inline pg_sequence_read_handle acquire_sequence_for_read(
    Datum input,
    slice_trace_attachment const* trace = nullptr,
    std::uint32_t producer_value_id = 0u)
{
    if (auto const* expanded = get_pg_sequence_expanded_ro(input);
        expanded != nullptr) {
        return {
            .backing = expanded,
            .root_value = expanded->root,
            .type_params = expanded->type_params,
            .logical_length = expanded->bp_length,
            .alphabet_bound = expanded->alphabet_bound,
        };
    }
    acquired_sequence adopted = acquire_writable_pg_sequence(
        input, trace, producer_value_id);
    return {
        .backing = adopted.value,
        .root_value = adopted.value->root,
        .type_params = adopted.value->type_params,
        .logical_length = adopted.value->bp_length,
        .alphabet_bound = adopted.value->alphabet_bound,
    };
}

export [[nodiscard]] inline std::uint64_t checked_add_bp_lengths(
    std::uint64_t left,
    std::uint64_t right)
{
    constexpr auto max_logical_length =
        static_cast<std::uint64_t>(INT64_MAX);
    if (left > max_logical_length || right > max_logical_length
        || right > max_logical_length - left) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq concatenation exceeds bigint logical length")));
    }
    return left + right;
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

} // namespace

export [[nodiscard]] inline compact_sequence_stdc_type_params
resolve_concat_type(
    pg_sequence_expanded const* left,
    pg_sequence_read_handle const& right)
{
    if (left->alphabet_bound && right.alphabet_bound) {
        if (left->type_params.alphabet_code
            != right.type_params.alphabet_code) {
            const auto left_name =
                pgseq_supported_alphabets::name_of_alphabet_code(
                    left->type_params.alphabet_code);
            const auto right_name =
                pgseq_supported_alphabets::name_of_alphabet_code(
                    right.type_params.alphabet_code);
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("cannot concatenate bioseq alphabets %.*s and %.*s",
                            static_cast<int>(left_name.size()),
                            left_name.data(),
                            static_cast<int>(right_name.size()),
                            right_name.data())));
        }
        return left->type_params;
    }
    if (left->alphabet_bound) {
        return left->type_params;
    }
    if (right.alphabet_bound) {
        return right.type_params;
    }
    return {
        .alphabet_code = pgseq_unbound_alphabet_code,
        .storage_unit_byte_width = pgseq_unbound_width_tag,
    };
}

export [[nodiscard]] Datum concat_pg_sequences(
    Datum left_datum,
    Datum right_datum)
{
    auto left = acquire_writable_pg_sequence(left_datum);
    auto right = acquire_sequence_for_read(right_datum);
    const auto result_type = resolve_concat_type(left.value, right);
    const bool bound = left.value->alphabet_bound || right.alphabet_bound;
    const auto result_length = checked_add_bp_lengths(
        left.value->bp_length, right.bp_length());
    const bool bind_left = bound && !left.value->alphabet_bound;
    const bool bind_right = bound && !right.alphabet_bound;
    const piece_edge left_root = left.value->root;
    const piece_edge right_root = right.root();
    const std::uint64_t left_length = left.value->bp_length;
    const std::uint64_t right_length = right.bp_length();
    if (bind_left || bind_right) {
        /* Alphabet adoption necessarily visits the unbound literal side and
         * retains the full validator for that type-changing operation. */
        replace_root_error_safe(
            *left.value, result_type, result_length, bound,
            [bind_left, bind_right, result_type,
             left_root, right_root](
                piece_tree_arena& arena,
                piece_edge_cleanup_ledger& ledger) {
                tracked_piece_edge bound_left = bind_left
                    ? bind_literals_copy_error_atomic(
                        arena, left_root, result_type, ledger)
                    : retain_piece_edge_error_atomic(
                        ledger, left_root);
                tracked_piece_edge bound_right = bind_right
                    ? bind_literals_copy_error_atomic(
                        arena, right_root, result_type, ledger)
                    : retain_piece_edge_error_atomic(
                        ledger, right_root);
                return join_tracked_edges_error_atomic(
                    arena, ledger, bound_left, bound_right);
            });
    } else {
        replace_root_trusted_join_error_safe(
            *left.value, result_type, result_length, bound,
            left_root, left_length, right_root, right_length,
            [left_root, right_root](
                piece_tree_arena& arena,
                piece_edge_cleanup_ledger& ledger) {
                /* Existing subtrees remain immutable.  The tracked join
                 * retains both borrowed operands and every local intermediate
                 * before any operation capable of PostgreSQL ERROR. */
                return join_edges_error_atomic(
                    arena, ledger, left_root, right_root);
            });
    }
    reconcile_concat_trace_provenance_noexcept(
        *left.value, *right.backing);
    return pg_sequence_rw_datum(left.value);
}

export [[nodiscard]] Datum slice_pg_sequence(
    Datum input,
    std::uint64_t from,
    std::uint64_t to)
{
    auto acquired = acquire_writable_pg_sequence(input);
    if (from > to || to > acquired.value->bp_length) {
        ereport(ERROR,
                (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                 errmsg("bioseq slice is outside its logical sequence")));
    }
    replace_root_error_safe(
        *acquired.value, acquired.value->type_params, to - from,
        acquired.value->alphabet_bound,
        [&acquired, from, to](
            piece_tree_arena& arena,
            piece_edge_cleanup_ledger& ledger) {
            return slice_tree(
                arena, ledger, acquired.value->root, from, to);
        });
    return pg_sequence_rw_datum(acquired.value);
}

export [[nodiscard]] Datum transform_pg_sequence(
    Datum input,
    bioseq::storage::sequence_transform transform)
{
    auto acquired = acquire_writable_pg_sequence(input);
    if (!acquired.value->alphabet_bound
        && bioseq::storage::has_complement(transform)) {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("reverse complement requires a bound bioseq alphabet"),
                 errhint("cast the sequence as bioseq.seq('alphabet', width) first")));
    }
    replace_root_error_safe(
        *acquired.value, acquired.value->type_params,
        acquired.value->bp_length, acquired.value->alphabet_bound,
        [&acquired, transform](
            piece_tree_arena&,
            piece_edge_cleanup_ledger& ledger) {
            return transform_piece_edge_error_atomic(
                ledger, acquired.value->root, transform);
        });
    return pg_sequence_rw_datum(acquired.value);
}

export [[nodiscard]] bool alphabet_has_complete_complement(
    std::uint8_t alphabet_code)
{
    bool complete = true;
    pgseq_supported_alphabets::dispatch_alphabet_by_code(
        alphabet_code,
        [alphabet_code, &complete]<class Alphabet>() {
            for (std::uint16_t rank = 0u; rank <= UINT8_MAX; ++rank) {
                char original = '\0';
                if (!pgseq_symbol_for_rank(
                        alphabet_code, static_cast<std::uint8_t>(rank),
                        original)) {
                    continue;
                }
                const auto complemented =
                    bioseq::storage::lazy_rank_stream_traits<Alphabet>::
                        letter_complement(static_cast<std::uint8_t>(rank));
                char symbol = '\0';
                if (!pgseq_symbol_for_rank(
                        alphabet_code, complemented, symbol)) {
                    complete = false;
                    return;
                }
            }
        });
    return complete;
}

/* Table-AM promotion entrypoint.  The caller has already acquired a retained
 * source-domain handle from the verified binding registry. */
export [[nodiscard]] inline Datum make_source_backed_pg_sequence(
    pg_sequence_source_handle source,
    bioseq::storage::seq_locator locator,
    compact_sequence_stdc_type_params stored_type,
    std::uint16_t last_padding_letters,
    slice_trace_attachment* trace = nullptr,
    std::uint32_t producer_value_id = 0u)
{
    if (!pg_sequence_source_handle_valid(source)
        || locator.total_bp_size > static_cast<std::uint64_t>(INT64_MAX)) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("invalid source binding for bioseq locator promotion")));
    }
    auto* result = make_pg_sequence_expanded(stored_type, true);
    result->trace = trace;
    result->producer_value_id = producer_value_id;
    retain_slice_trace_attachment(trace);
    replace_root_error_safe(
        *result, stored_type, locator.total_bp_size, true,
        [source, locator, stored_type,
         last_padding_letters](
            piece_tree_arena& arena,
            piece_edge_cleanup_ledger& ledger) {
            return make_source_node_error_atomic(
                arena, ledger,
                source_piece{
                    .source = source,
                    .locator = locator,
                    .stored_type_params = stored_type,
                    .last_padding_letters = last_padding_letters,
                    .from = 0u,
                    .to = locator.total_bp_size,
                });
        });
    return pg_sequence_ro_datum(result);
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
                           bioseq::storage::seq_locator const &locator,
                           compact_sequence_stdc_type_params stored_type,
                           bioseq::storage::sequence_transform transform,
                           std::uint64_t from, std::span<std::uint8_t> output) {
  constexpr std::uint8_t dna4_code =
      pgseq_supported_alphabets::index_of<seq::dna4>();
  if (stored_type.alphabet_code != dna4_code || from > locator.total_bp_size ||
      output.size() > locator.total_bp_size - from) {
    return pg_sequence_source_read_status::wrong_thread;
  }
  std::uint8_t rank = 0u;
  if (bioseq::storage::has_complement(transform)) {
    rank =
        bioseq::storage::lazy_rank_stream_traits<seq::dna4>::letter_complement(
            rank);
  }
  std::fill(output.begin(), output.end(), rank);
  return pg_sequence_source_read_status::ok;
}

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
  constexpr bioseq::storage::seq_locator locator{
      .version = bioseq::storage::kLocatorVersion,
      .bucket_idx = 0u,
      ._pad_2 = 0u,
      .shard_idx = 0u,
      .num_fragments = 1u,
      ._pad_4 = 0u,
      .total_bp_size = piece_length,
      .first_sub_row_no = 0u,
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

} // namespace

/* Internal acceptance seam for logical-length guardrails. Its immutable
 * process-lifetime source synthesizes DNA4 rank zero on demand, so tests can
 * exercise bigint metadata without allocating the logical payload. */
export namespace bioseq::pg_types::sql_routines {

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::restricted,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::execute,
    .from = bioseq::sql::public_role,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
})]]
Datum synthetic_lazy_sequence(std::int64_t requested) {
  if (requested < 0) {
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("synthetic bioseq length must be nonnegative")));
  }
  constexpr compact_sequence_stdc_type_params type{
      .alphabet_code = pgseq_supported_alphabets::index_of<::seq::dna4>(),
      .storage_unit_byte_width = 8u,
  };
  const auto length = static_cast<std::uint64_t>(requested);
  return make_source_backed_pg_sequence(
      synthetic_lazy_source_handle(),
      bioseq::storage::seq_locator{
          .version = bioseq::storage::kLocatorVersion,
          .bucket_idx = 0u,
          ._pad_2 = 0u,
          .shard_idx = 0u,
          .num_fragments = length == 0u ? 0u : 1u,
          ._pad_4 = 0u,
          .total_bp_size = length,
          .first_sub_row_no = 0u,
          .source_epoch = 1u,
      },
      type, 0u);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::execute,
    .from = bioseq::sql::public_role,
})]]
bool materialization_size_self_test() {
  return run_materialization_size_guardrail_matrix();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::restricted,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::execute,
    .from = bioseq::sql::public_role,
})]]
std::int64_t sequence_piece_count(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<bioseq::pg_types::seq>,
    })]]
    Datum input) {
  const auto value = acquire_sequence_for_read(input);
  return value.root()
      ? static_cast<std::int64_t>(value.root().node->piece_count)
      : 0;
}

/* Direct PostgreSQL ownership-protocol seam: RW datumTransfer reparents the
 * EOH, while RO datumCopy produces an independent flat value.  Both must
 * survive reset/deletion of the context that originally owned the EOH. */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::execute,
    .from = bioseq::sql::public_role,
})]]
bool expanded_ownership_self_test() {
  MemoryContext saved = CurrentMemoryContext;
  MemoryContext volatile transfer_input = nullptr;
  MemoryContext volatile transfer_parent = nullptr;
  MemoryContext volatile copy_input = nullptr;
  MemoryContext volatile copy_parent = nullptr;
  bool volatile valid = false;
  const std::uint64_t arena_baseline = piece_tree_live_arena_count_noexcept();
  const bool concat_length_guardrail_matrix =
      run_concat_length_guardrail_matrix();
  const bool public_concat_path_copy =
      run_public_concat_path_copy_probe();
#ifdef BIOSEQ_TEST_INDEXES
  const bool read_session_parent_reset =
      pg_sequence_read_session_parent_reset_self_test();
#else
  const bool read_session_parent_reset = true;
#endif
  PG_TRY();
  {
    transfer_input = AllocSetContextCreate(saved, "bioseq EOH transfer input",
                                           ALLOCSET_SMALL_SIZES);
    transfer_parent = AllocSetContextCreate(saved, "bioseq EOH transfer parent",
                                            ALLOCSET_SMALL_SIZES);
    copy_input = AllocSetContextCreate(saved, "bioseq EOH copy input",
                                       ALLOCSET_SMALL_SIZES);
    copy_parent = AllocSetContextCreate(saved, "bioseq EOH copy parent",
                                        ALLOCSET_SMALL_SIZES);
    constexpr compact_sequence_stdc_type_params unbound_type{
        .alphabet_code = pgseq_unbound_alphabet_code,
        .storage_unit_byte_width = pgseq_unbound_width_tag,
    };
    const auto make_probe = [unbound_type](MemoryContext input,
                                           std::array<std::byte, 3> bytes) {
      MemoryContext previous = MemoryContextSwitchTo(input);
      auto *value = make_pg_sequence_expanded(unbound_type, false);
      replace_root_error_safe(
          *value, unbound_type, bytes.size(), false,
          [bytes](piece_tree_arena &arena,
                  piece_edge_cleanup_ledger &ledger) {
            auto *storage = allocate_piece_literal_storage(arena, bytes.size());
            std::memcpy(storage, bytes.data(), bytes.size());
            return make_literal_node_error_atomic(
                arena, ledger,
                literal_piece{
                    .storage_arena = std::addressof(arena),
                    .bytes = storage,
                    .symbol_from = 0u,
                    .symbol_to = bytes.size(),
                    .stored_type_params =
                        {
                            .alphabet_code = pgseq_unbound_alphabet_code,
                            .storage_unit_byte_width = pgseq_unbound_width_tag,
                        },
                    .encoding = literal_encoding::unbound_ascii,
                    .validation = literal_validation::unvalidated,
                });
          });
      MemoryContextSwitchTo(previous);
      return value;
    };

    auto *transferred_value =
        make_probe(const_cast<MemoryContext>(transfer_input),
                   {std::byte{0x41}, std::byte{0x43}, std::byte{0x47}});
    MemoryContextSwitchTo(const_cast<MemoryContext>(transfer_parent));
    const Datum transferred =
        datumTransfer(pg_sequence_rw_datum(transferred_value), false, -1);
    MemoryContextSwitchTo(saved);
    const bool reparented =
        get_pg_sequence_expanded_rw(transferred) == transferred_value &&
        MemoryContextGetParent(transferred_value->object_context) ==
            transfer_parent;
    MemoryContextReset(const_cast<MemoryContext>(transfer_input));
    std::array<std::byte, 3> transferred_text{};
    const bool transfer_survived =
        emit_pg_unbound_ascii(transferred_value->root, 0u,
                              transferred_text.size(), transferred_text) &&
        transferred_text == std::array<std::byte, 3>{std::byte{0x41},
                                                     std::byte{0x43},
                                                     std::byte{0x47}};

    auto *copied_value =
        make_probe(const_cast<MemoryContext>(copy_input),
                   {std::byte{0x54}, std::byte{0x47}, std::byte{0x41}});
    const Datum read_only = MakeExpandedObjectReadOnly(
        pg_sequence_rw_datum(copied_value), false, -1);
    MemoryContextSwitchTo(const_cast<MemoryContext>(copy_parent));
    const Datum copied = datumCopy(read_only, false, -1);
    MemoryContextSwitchTo(saved);
    MemoryContextReset(const_cast<MemoryContext>(copy_input));
    auto const *copied_flat =
        reinterpret_cast<bioseq::pg_types::seq const *>(DatumGetPointer(copied));
    const bool copy_survived =
        !VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(copied)) &&
        VARSIZE(copied_flat) >=
            bioseq::pg_types::seq::header_bytes() + transferred_text.size() &&
        copied_flat->alphabet_code == pgseq_unbound_alphabet_code &&
        copied_flat->storage_unit_byte_width == pgseq_unbound_width_tag &&
        copied_flat->bp_size == transferred_text.size() &&
        std::memcmp(copied_flat->data, "TGA", transferred_text.size()) == 0;
    valid = concat_length_guardrail_matrix && public_concat_path_copy
        && read_session_parent_reset
        && reparented && transfer_survived && copy_survived;

    MemoryContextDelete(const_cast<MemoryContext>(copy_input));
    copy_input = nullptr;
    MemoryContextDelete(const_cast<MemoryContext>(transfer_input));
    transfer_input = nullptr;
    MemoryContextDelete(const_cast<MemoryContext>(copy_parent));
    copy_parent = nullptr;
    MemoryContextDelete(const_cast<MemoryContext>(transfer_parent));
    transfer_parent = nullptr;
    valid = valid && piece_tree_live_arena_count_noexcept() == arena_baseline;
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(saved);
    if (copy_input != nullptr) {
      MemoryContextDelete(const_cast<MemoryContext>(copy_input));
    }
    if (transfer_input != nullptr) {
      MemoryContextDelete(const_cast<MemoryContext>(transfer_input));
    }
    if (copy_parent != nullptr) {
      MemoryContextDelete(const_cast<MemoryContext>(copy_parent));
    }
    if (transfer_parent != nullptr) {
      MemoryContextDelete(const_cast<MemoryContext>(transfer_parent));
    }
    FlushErrorState();
    valid = false;
  }
  PG_END_TRY();
  MemoryContextSwitchTo(saved);
  return valid;
}

}  // namespace bioseq::pg_types::sql_routines

/* Trace metadata is observational and may be installed on an otherwise
 * read-only EOH wrapper without changing its logical sequence value. */
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

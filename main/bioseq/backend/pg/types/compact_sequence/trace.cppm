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

export module bioseq.types.pg_compact_sequence:trace;

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
    slice_materialization_runtime* runtime) noexcept
{
    if (runtime == nullptr) {
        return;
    }
    runtime->collection_enabled = false;
    runtime->timing_enabled = false;
    runtime->boundaries = nullptr;
    runtime->boundary_count = 0u;
    runtime->cumulative_sink = {};
}

export struct slice_trace_attachment {
    std::uint32_t refcount;
    std::uint64_t generation;
    slice_materialization_runtime* runtime;
    bool valid;
};

static_assert(std::is_standard_layout_v<slice_trace_attachment>);

namespace {

inline std::uint64_t next_slice_trace_generation = 1u;

} // namespace

export [[nodiscard]] inline slice_trace_attachment*
make_slice_trace_attachment(slice_materialization_runtime* runtime)
{
    if (runtime == nullptr || runtime->invalid
        || next_slice_trace_generation == 0u) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq materialization trace generation exhausted")));
    }
    auto* attachment = static_cast<slice_trace_attachment*>(
        MemoryContextAllocZero(TopMemoryContext,
                               sizeof(slice_trace_attachment)));
    attachment->refcount = 1u;
    attachment->generation = next_slice_trace_generation;
    attachment->runtime = runtime;
    attachment->valid = true;
    if (next_slice_trace_generation
        == (std::numeric_limits<std::uint64_t>::max)()) {
        next_slice_trace_generation = 0u;
    } else {
        ++next_slice_trace_generation;
    }
    return attachment;
}

export [[nodiscard]] inline bool slice_trace_attachment_live(
    slice_trace_attachment const* attachment) noexcept
{
    return attachment != nullptr && attachment->refcount != 0u
        && attachment->valid && attachment->runtime != nullptr
        && !attachment->runtime->invalid;
}

export inline void retain_slice_trace_attachment(
    slice_trace_attachment* attachment) noexcept
{
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

export inline void release_slice_trace_attachment(
    slice_trace_attachment* attachment) noexcept
{
    if (attachment == nullptr || attachment->refcount == 0u
        || attachment->refcount == UINT32_MAX) {
        return;
    }
    if (--attachment->refcount == 0u) {
        pfree(attachment);
    }
}

export inline void invalidate_slice_trace_attachment_noexcept(
    slice_trace_attachment* attachment) noexcept
{
    if (attachment == nullptr) {
        return;
    }
    attachment->valid = false;
    attachment->runtime = nullptr;
}

export struct slice_materialization_scope {
    slice_trace_attachment* attachment;
    std::uint32_t boundary_id;
    std::uint32_t producer_value_id;
    materialization_mechanism mechanism;
    materialization_reason reason;
    slice_counter_sink* local_sink;
};

namespace {

inline thread_local slice_materialization_scope active_slice_scope{};

[[nodiscard]] inline slice_materialization_boundary const*
find_boundary_by_id(
    slice_materialization_runtime const& runtime,
    std::uint32_t boundary_id) noexcept
{
    for (std::uint32_t index = 0u; index < runtime.boundary_count; ++index) {
        if (runtime.boundaries[index].boundary_id == boundary_id) {
            return std::addressof(runtime.boundaries[index]);
        }
    }
    return nullptr;
}

[[nodiscard]] inline slice_materialization_boundary const*
find_unique_boundary(
    slice_materialization_runtime const& runtime,
    std::uint32_t producer_value_id,
    materialization_mechanism mechanism,
    materialization_reason reason) noexcept
{
    slice_materialization_boundary const* result = nullptr;
    for (std::uint32_t index = 0u; index < runtime.boundary_count; ++index) {
        auto const& candidate = runtime.boundaries[index];
        if (candidate.producer_value_id != producer_value_id
            || candidate.mechanism != mechanism
            || candidate.reason != reason) {
            continue;
        }
        if (result != nullptr) {
            return nullptr;
        }
        result = std::addressof(candidate);
    }
    return result;
}

[[nodiscard]] inline std::uint64_t steady_clock_now_ns() noexcept
{
    using namespace std::chrono;
    const auto value = duration_cast<nanoseconds>(
        steady_clock::now().time_since_epoch()).count();
    return value <= 0 ? 1u : static_cast<std::uint64_t>(value);
}

} // namespace

export [[nodiscard]] inline slice_materialization_scope
get_current_slice_materialization_scope() noexcept
{
    return active_slice_scope;
}

export [[nodiscard]] inline slice_materialization_scope
exchange_slice_materialization_scope(
    slice_materialization_scope next) noexcept
{
    const auto previous = active_slice_scope;
    active_slice_scope = next;
    return previous;
}

export inline void restore_slice_materialization_scope(
    slice_materialization_scope previous) noexcept
{
    active_slice_scope = previous;
}

export [[nodiscard]] inline std::uint64_t
begin_slice_materialization_timing(
    slice_trace_attachment const* attachment) noexcept
{
    if (attachment == nullptr) {
        attachment = active_slice_scope.attachment;
    }
    return slice_trace_attachment_live(attachment)
            && attachment->runtime->collection_enabled
            && attachment->runtime->timing_enabled
        ? steady_clock_now_ns() : 0u;
}

export inline void record_slice_materialization_success(
    slice_trace_attachment* attachment,
    std::uint32_t producer_value_id,
    materialization_mechanism default_mechanism,
    materialization_reason default_reason,
    std::uint64_t input_symbols,
    std::uint64_t output_symbols,
    std::uint64_t bytes,
    bool reused,
    std::uint64_t peak_retained_bytes,
    std::uint64_t started_at_ns = 0u) noexcept
{
    auto const scope = active_slice_scope;
    if (attachment == nullptr) {
        attachment = scope.attachment;
    }
    if (!slice_trace_attachment_live(attachment)) {
        return;
    }
    auto& runtime = *attachment->runtime;
    if (!runtime.collection_enabled || runtime.boundaries == nullptr) {
        return;
    }

    slice_counter_sink* sink = std::addressof(runtime.cumulative_sink);
    slice_materialization_boundary const* boundary = nullptr;
    const bool scoped = scope.attachment == attachment
        && scope.boundary_id != 0u && scope.local_sink != nullptr;
    if (scoped) {
        boundary = find_boundary_by_id(runtime, scope.boundary_id);
        if (boundary == nullptr
            || boundary->producer_value_id != scope.producer_value_id
            || boundary->mechanism != scope.mechanism
            || boundary->reason != scope.reason
            || (producer_value_id != 0u
                && producer_value_id != scope.producer_value_id)) {
            return;
        }
        sink = scope.local_sink;
    } else {
        if (producer_value_id == 0u) {
            return;
        }
        const bool boundary_reason_scope = scope.attachment == nullptr
            && scope.boundary_id == 0u
            && scope.producer_value_id == 0u
            && scope.local_sink == nullptr
            && default_mechanism
                == materialization_mechanism::packed_full
            && default_reason == materialization_reason::executor_copy
            && (scope.mechanism
                    != materialization_mechanism::backing_copy
                || scope.reason
                    != materialization_reason::executor_copy);
        boundary = find_unique_boundary(
            runtime, producer_value_id,
            boundary_reason_scope ? scope.mechanism : default_mechanism,
            boundary_reason_scope ? scope.reason : default_reason);
        if (boundary == nullptr) {
            return;
        }
    }
    if (sink == nullptr || sink->entries == nullptr
        || boundary->counter_index >= sink->entry_count) {
        runtime.invalid = true;
        return;
    }
    std::uint64_t elapsed_ns = 0u;
    if (runtime.timing_enabled && started_at_ns != 0u) {
        const std::uint64_t finished = steady_clock_now_ns();
        elapsed_ns = finished >= started_at_ns
            ? finished - started_at_ns : 0u;
    }
    auto& counter = sink->entries[boundary->counter_index];
    update_slice_dsm_counter_noexcept(
        counter, input_symbols, output_symbols, bytes, reused,
        peak_retained_bytes, elapsed_ns);
    runtime.overflowed = runtime.overflowed || counter.overflowed != 0u;
}

/* Fixed scheduler-independent owner state.  Concrete executors install only
 * preconstructed, no-throw callbacks; no state transition allocates. */
export enum class slice_async_owner_phase : std::uint8_t {
    accepting = 0u,
    quiescing,
    quiesced,
    terminal,
};

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
    void* argument;
    void (*invoke)(void*) noexcept;
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
    slice_dsm_counter* counters;
    std::uint32_t counter_count;
    fixed_job_error_handoff error;
    preconstructed_cancel_handle cancel;
    preconstructed_join_handle join;
    retained_job_backings backings;
    slice_trace_attachment* attachment;
};

static_assert(std::is_trivially_copyable_v<slice_async_job_slot>);
static_assert(std::is_standard_layout_v<slice_async_job_slot>);

export struct slice_async_owner_guard {
    MemoryContextCallback callback;
    slice_materialization_runtime* runtime;
    slice_trace_attachment* attachment;
    slice_async_job_slot* jobs;
    std::uint32_t job_capacity;
    std::uint32_t enrolled_jobs;
    slice_async_owner_phase phase;
    bool local_state_published;
    bool terminal_requested;
    bool slot_generation_overflow;
    slice_noexcept_callback publish_complete;
    slice_noexcept_callback publish_incomplete;
};

static_assert(std::is_standard_layout_v<slice_async_owner_guard>);

namespace {

inline void invoke_slice_callback(slice_noexcept_callback callback) noexcept
{
    if (callback.invoke != nullptr) {
        callback.invoke(callback.argument);
    }
}

inline void reset_error_handoff(fixed_job_error_handoff& error) noexcept
{
    std::atomic_ref<std::uint8_t>{error.published}.store(
        0u, std::memory_order_relaxed);
    error.incomplete = 0u;
    error.reserved.fill(std::byte{0});
    error.sqlstate = 0u;
    error.message_length = 0u;
    error.message.fill('\0');
}

[[nodiscard]] inline bool handle_matches(
    slice_async_owner_guard const* guard,
    slice_async_completion_handle handle) noexcept
{
    return guard != nullptr && guard->jobs != nullptr
        && handle.slot_index < guard->job_capacity
        && guard->jobs[handle.slot_index].generation == handle.generation;
}

inline void merge_job_counters_noexcept(
    slice_async_owner_guard* guard,
    slice_async_job_slot& job) noexcept
{
    if (guard->runtime == nullptr
        || guard->runtime->cumulative_sink.entries == nullptr
        || job.counters == nullptr
        || job.counter_count
            != guard->runtime->cumulative_sink.entry_count) {
        if (guard->runtime != nullptr) {
            guard->runtime->invalid = true;
        }
        return;
    }
    for (std::uint32_t index = 0u; index < job.counter_count; ++index) {
        if (!slice_dsm_counter_reserved_valid(job.counters[index])) {
            guard->runtime->invalid = true;
            guard->runtime->overflowed = true;
            return;
        }
        merge_slice_dsm_counter_noexcept(
            guard->runtime->cumulative_sink.entries[index],
            job.counters[index]);
        guard->runtime->overflowed = guard->runtime->overflowed
            || guard->runtime->cumulative_sink.entries[index].overflowed != 0u;
    }
}

inline void rearm_job_noexcept(
    slice_async_owner_guard* guard,
    slice_async_job_slot& job) noexcept
{
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
    if (job.generation
        == (std::numeric_limits<std::uint64_t>::max)()) {
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

export [[nodiscard]] inline bool initialize_slice_async_owner_noexcept(
    slice_async_owner_guard* guard,
    slice_materialization_runtime* runtime,
    slice_trace_attachment* attachment,
    slice_async_job_slot* jobs,
    std::uint32_t job_capacity,
    slice_dsm_counter* job_counters,
    std::uint32_t counter_count) noexcept;

export inline void slice_async_owner_reset(void* argument) noexcept;

export [[nodiscard]] inline bool initialize_slice_async_owner_noexcept(
    slice_async_owner_guard* guard,
    slice_materialization_runtime* runtime,
    slice_trace_attachment* attachment,
    slice_async_job_slot* jobs,
    std::uint32_t job_capacity,
    slice_dsm_counter* job_counters,
    std::uint32_t counter_count) noexcept
{
    if (guard == nullptr || runtime == nullptr || attachment == nullptr
        || (job_capacity != 0u && jobs == nullptr)
        || (job_capacity != 0u && counter_count != 0u
            && job_counters == nullptr)) {
        return false;
    }
    std::memset(guard, 0, sizeof(*guard));
    guard->callback.func = slice_async_owner_reset;
    guard->callback.arg = guard;
    guard->runtime = runtime;
    guard->attachment = attachment;
    guard->jobs = jobs;
    guard->job_capacity = job_capacity;
    guard->phase = slice_async_owner_phase::accepting;
    for (std::uint32_t slot_index = 0u;
         slot_index < job_capacity; ++slot_index) {
        auto& job = jobs[slot_index];
        job.generation = 0u;
        job.state = slice_async_job_state::unused;
        job.counters = counter_count == 0u ? nullptr
            : job_counters
                + static_cast<std::size_t>(slot_index) * counter_count;
        job.counter_count = counter_count;
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

export [[nodiscard]] inline bool enroll_slice_async_job_noexcept(
    slice_async_owner_guard* guard,
    slice_async_job_resources resources,
    slice_async_completion_handle* handle) noexcept
{
    if (guard == nullptr || handle == nullptr
        || guard->phase != slice_async_owner_phase::accepting
        || guard->slot_generation_overflow) {
        return false;
    }
    for (std::uint32_t index = 0u; index < guard->job_capacity; ++index) {
        auto& job = guard->jobs[index];
        if (job.state != slice_async_job_state::unused) {
            continue;
        }
        for (std::uint32_t counter = 0u;
             counter < job.counter_count; ++counter) {
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
    slice_async_owner_guard* guard,
    slice_async_completion_handle handle) noexcept
{
    if (!handle_matches(guard, handle)) {
        return false;
    }
    auto& job = guard->jobs[handle.slot_index];
    if (job.state != slice_async_job_state::enrolled) {
        return false;
    }
    job.state = slice_async_job_state::scheduled;
    return true;
}

export inline void handoff_slice_async_job_error_noexcept(
    slice_async_owner_guard* guard,
    slice_async_completion_handle handle,
    std::uint32_t sqlstate,
    char const* message,
    bool incomplete) noexcept
{
    if (!handle_matches(guard, handle)) {
        return;
    }
    auto& error = guard->jobs[handle.slot_index].error;
    error.sqlstate = sqlstate;
    error.incomplete = incomplete ? 1u : 0u;
    const std::size_t length = message == nullptr ? 0u
        : std::min<std::size_t>(
            std::strlen(message), error.message.size() - 1u);
    if (length != 0u) {
        std::memcpy(error.message.data(), message, length);
    }
    error.message[length] = '\0';
    error.message_length = static_cast<std::uint16_t>(length);
    std::atomic_ref<std::uint8_t>{error.published}.store(
        1u, std::memory_order_release);
}

export [[nodiscard]] inline bool mark_slice_async_job_finished_noexcept(
    slice_async_owner_guard* guard,
    slice_async_completion_handle handle) noexcept
{
    /* Called by the executor only after scheduler completion publication;
     * workers never read or write job.state. */
    if (!handle_matches(guard, handle)) {
        return false;
    }
    auto& job = guard->jobs[handle.slot_index];
    if (job.state != slice_async_job_state::scheduled) {
        return false;
    }
    job.state = slice_async_job_state::finished;
    return true;
}

export [[nodiscard]] inline bool drain_slice_async_job_noexcept(
    slice_async_owner_guard* guard,
    slice_async_completion_handle handle,
    bool* incomplete_out = nullptr) noexcept
{
    if (!handle_matches(guard, handle)) {
        return false;
    }
    auto& job = guard->jobs[handle.slot_index];
    bool forced_incomplete = false;
    if (job.state == slice_async_job_state::enrolled) {
        /* A scheduler rejection is still a visible incomplete job use. */
        job.error.incomplete = 1u;
        std::atomic_ref<std::uint8_t>{job.error.published}.store(
            1u, std::memory_order_release);
        job.state = slice_async_job_state::scheduled;
    }
    if (job.state == slice_async_job_state::scheduled) {
        /* Reaching drain before executor-side completion publication means
         * shutdown/early-stop had to cancel this use.  It cannot be reported
         * as a complete cycle even when cancellation and join both succeed. */
        forced_incomplete = true;
        invoke_slice_callback(job.cancel);
        job.state = slice_async_job_state::finished;
    }
    if (job.state == slice_async_job_state::finished) {
        invoke_slice_callback(job.join);
        job.state = slice_async_job_state::joined;
    }
    const bool incomplete = forced_incomplete
        || std::atomic_ref<std::uint8_t>{job.error.published}.load(
            std::memory_order_acquire) != 0u
        || job.error.incomplete != 0u;
    if (job.state == slice_async_job_state::joined) {
        merge_job_counters_noexcept(guard, job);
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
            *incomplete_out = *incomplete_out || incomplete
                || guard->slot_generation_overflow;
        }
        return true;
    }
    return job.state == slice_async_job_state::unused;
}

export inline void quiesce_slice_async_owner_noexcept(
    slice_async_owner_guard* guard,
    slice_shutdown_disposition disposition) noexcept
{
    if (guard == nullptr
        || guard->phase == slice_async_owner_phase::quiesced
        || guard->phase == slice_async_owner_phase::terminal) {
        return;
    }
    guard->phase = slice_async_owner_phase::quiescing;
    bool incomplete = disposition == slice_shutdown_disposition::incomplete;
    for (std::uint32_t index = 0u; index < guard->job_capacity; ++index) {
        auto& job = guard->jobs[index];
        if (job.state == slice_async_job_state::unused) {
            continue;
        }
        const slice_async_completion_handle handle{index, job.generation};
        (void) drain_slice_async_job_noexcept(
            guard, handle, std::addressof(incomplete));
    }
    if (!guard->local_state_published) {
        invoke_slice_callback(incomplete
            ? guard->publish_incomplete : guard->publish_complete);
        guard->local_state_published = true;
    }
    guard->phase = slice_async_owner_phase::quiesced;
}

export inline void finish_slice_async_owner_noexcept(
    slice_async_owner_guard* guard,
    slice_shutdown_disposition disposition) noexcept
{
    if (guard == nullptr
        || guard->phase == slice_async_owner_phase::terminal) {
        return;
    }
    guard->terminal_requested = true;
    quiesce_slice_async_owner_noexcept(guard, disposition);
    auto* attachment = guard->attachment;
    guard->attachment = nullptr;
    auto* runtime = guard->runtime;
    guard->runtime = nullptr;
    invalidate_slice_trace_attachment_noexcept(attachment);
    release_slice_trace_attachment(attachment);
    destroy_slice_runtime_noexcept(runtime);
    guard->phase = slice_async_owner_phase::terminal;
}

export inline void slice_async_owner_reset(void* argument) noexcept
{
    finish_slice_async_owner_noexcept(
        static_cast<slice_async_owner_guard*>(argument),
        slice_shutdown_disposition::incomplete);
}

export [[nodiscard]] inline bool rearm_slice_async_owner_for_rescan_noexcept(
    slice_async_owner_guard* guard) noexcept
{
    if (guard == nullptr
        || guard->phase != slice_async_owner_phase::quiesced
        || guard->enrolled_jobs != 0u || guard->terminal_requested
        || guard->slot_generation_overflow) {
        return false;
    }
    guard->local_state_published = false;
    guard->phase = slice_async_owner_phase::accepting;
    return true;
}

/* Integration seam for the pure fixed-record/state-machine invariants.  It
 * uses only stack-owned POD and never installs query-visible trace state. */
export [[nodiscard]] inline bool slice_trace_self_test_noexcept() noexcept
{
    bool valid = sizeof(slice_dsm_counter) == 64u;

    slice_dsm_counter overflow{};
    reset_slice_dsm_counter_noexcept(overflow);
    overflow.calls = (std::numeric_limits<std::uint64_t>::max)();
    update_slice_dsm_counter_noexcept(
        overflow, 1u, 1u, 1u, false, 9u, 1u);
    valid = valid
        && overflow.calls
            == (std::numeric_limits<std::uint64_t>::max)()
        && overflow.overflowed != 0u;

    slice_dsm_counter malformed{};
    malformed.reserved[3] = std::byte{1};
    merge_slice_dsm_counter_noexcept(overflow, malformed);
    valid = valid && overflow.overflowed != 0u
        && slice_dsm_counter_reserved_valid(overflow);

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
        std::addressof(attachment), 7u,
        materialization_mechanism::backing_copy,
        materialization_reason::client_output,
        13u, 5u, 24u, true, 31u, 0u);
    valid = valid && cumulative.calls == 1u
        && cumulative.input_symbols == 13u
        && cumulative.output_symbols == 5u
        && cumulative.bytes == 24u
        && cumulative.reused_calls == 1u
        && cumulative.peak_retained_bytes == 31u;
    const auto saved_inner = exchange_slice_materialization_scope({});
    restore_slice_materialization_scope(saved_inner);
    const auto restored_outer = get_current_slice_materialization_scope();
    restore_slice_materialization_scope(saved_outer);
    const auto scope_equal = [](
        slice_materialization_scope const& left,
        slice_materialization_scope const& right) noexcept {
        return left.attachment == right.attachment
            && left.boundary_id == right.boundary_id
            && left.producer_value_id == right.producer_value_id
            && left.mechanism == right.mechanism
            && left.reason == right.reason
            && left.local_sink == right.local_sink;
    };
    valid = valid && scope_equal(observed_outer, outer)
        && scope_equal(restored_outer, outer)
        && scope_equal(
            get_current_slice_materialization_scope(), original_scope);
    reset_slice_dsm_counter_noexcept(cumulative);

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
    valid = valid && first_capacity_enrolled && second_capacity_enrolled
        && capacity_handles[0].slot_index != capacity_handles[1].slot_index;
    if (first_capacity_enrolled && second_capacity_enrolled) {
        for (auto const handle : capacity_handles) {
            valid = valid && mark_slice_async_job_scheduled_noexcept(
                std::addressof(guard), handle)
                && mark_slice_async_job_finished_noexcept(
                    std::addressof(guard), handle)
                && drain_slice_async_job_noexcept(
                    std::addressof(guard), handle);
        }
        valid = valid && guard.enrolled_jobs == 0u
            && attachment.refcount == 32u;
    }
    slice_async_completion_handle first_handle{};
    for (std::uint32_t use = 0u; use < 128u; ++use) {
        slice_async_completion_handle handle{};
        const bool enrolled = enroll_slice_async_job_noexcept(
            std::addressof(guard), {}, std::addressof(handle));
        if (use == 0u) {
            first_handle = handle;
        }
        valid = valid && enrolled
            && mark_slice_async_job_scheduled_noexcept(
                std::addressof(guard), handle)
            && mark_slice_async_job_finished_noexcept(
                std::addressof(guard), handle);
        if (!enrolled) {
            break;
        }
        update_slice_dsm_counter_noexcept(
            guard.jobs[handle.slot_index].counters[0],
            1u, 1u, 8u, false, 8u, 0u);
        valid = valid && drain_slice_async_job_noexcept(
            std::addressof(guard), handle)
            && guard.enrolled_jobs == 0u;
    }
    valid = valid && cumulative.calls == 128u
        && attachment.refcount == 32u
        && !mark_slice_async_job_scheduled_noexcept(
            std::addressof(guard), first_handle)
        && !guard.slot_generation_overflow;
    slice_async_completion_handle cancelled_handle{};
    bool cancelled_incomplete = false;
    valid = valid && enroll_slice_async_job_noexcept(
        std::addressof(guard), {}, std::addressof(cancelled_handle))
        && mark_slice_async_job_scheduled_noexcept(
            std::addressof(guard), cancelled_handle)
        && drain_slice_async_job_noexcept(
            std::addressof(guard), cancelled_handle,
            std::addressof(cancelled_incomplete))
        && cancelled_incomplete && guard.enrolled_jobs == 0u
        && attachment.refcount == 32u && cumulative.calls == 128u;
    restore_slice_materialization_scope(original_scope);
    return valid;
}

export inline Datum slice_trace_self_test_impl(FunctionCallInfo)
{
    PG_RETURN_BOOL(slice_trace_self_test_noexcept());
}

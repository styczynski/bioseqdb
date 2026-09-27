/**
 * @file
 * @brief Verifies pool scheduling, tracked tasks, and memory-admission lifetimes.
 */

#include <catch2/catch_test_macros.hpp>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__linux__)
#include <malloc.h>
#endif

import std;
import bioseq.alignment.contracts;
import bioseq.common.context;
import bioseq.common.heap_allocation;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.pipeline_ops;
import bioseq.parallelism.memory_admission;
import bioseq.pipeline.pipe;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.minimap_port.match_result_contract;
import bioseq.sequences.cigar;

namespace {

std::array<std::uint64_t, 4> materialization_observations{};
std::atomic<std::size_t> materialization_observation_index{0u};

inline constexpr std::uint64_t k_context_test_block_bytes = 4096u;
std::atomic<std::uint64_t> context_reflection_probe_calls{0u};
std::uint64_t context_reflected_block_bytes = 0u;
bioseq::parallelism::memory_admission_controller* replacement_peak_admission = nullptr;
std::atomic<std::uint64_t> replacement_peak_reserved_bytes{0u};

struct context_allocation_geometry {
    std::uint64_t admitted_bytes;
    std::uint64_t retained_bytes;
};

/// Independently read the native heap extent, not the allocator's logical request.
[[nodiscard]] std::size_t native_context_allocation_bytes(void* allocation) noexcept {
#if defined(__APPLE__)
    return ::malloc_size(allocation);
#elif defined(__GLIBC__)
    return ::malloc_usable_size(allocation);
#else
    return 0u;
#endif
}

/// Inspect one source-planned allocation without involving context admission.
[[nodiscard]] context_allocation_geometry context_test_allocation_geometry(
    std::size_t bytes,
    std::size_t alignment = alignof(std::max_align_t),
    std::size_t prefix_bytes = sizeof(void*)) {
    const auto plan =
        bioseq::common::heap_allocation_detail::plan_aligned_heap_allocation(
            bytes, alignment, prefix_bytes);
    void* allocation = std::malloc(plan.allocator_request_bytes);
    if (allocation == nullptr) {
        throw std::bad_alloc{};
    }
    const auto retained_bytes = native_context_allocation_bytes(allocation);
    std::free(allocation);
    REQUIRE(retained_bytes >= plan.allocator_request_bytes);
    REQUIRE(retained_bytes <= plan.allocation_requirement_bytes);
    return {plan.allocation_requirement_bytes, retained_bytes};
}

/// Test-only hook observer; ownership remains in the concrete context allocator.
struct context_storage_transaction_probe {
    std::uint64_t available_bytes = (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t reserved_bytes = 0u;
    context::backing_allocation_materialization allocation{};
    std::size_t materializations = 0u;
    std::size_t cleanups = 0u;
    bool reject_after_materialization = false;

    [[nodiscard]] static bool reconcile(void* state, std::uint64_t bytes) noexcept {
        auto& probe = *static_cast<context_storage_transaction_probe*>(state);
        if (bytes > probe.reserved_bytes) {
            return false;
        }
        probe.reserved_bytes = bytes;
        return true;
    }

    [[nodiscard]] static context::backing_allocation_authority materialize(
        void* state,
        std::uint64_t requested_bytes,
        context::backing_allocation_lifetime,
        void* operation,
        context::backing_allocation_materializer materializer,
        context::backing_allocation_cleanup cleanup) {
        auto& probe = *static_cast<context_storage_transaction_probe*>(state);
        if (requested_bytes > probe.available_bytes) {
            throw std::bad_alloc{};
        }
        probe.reserved_bytes = requested_bytes;
        try {
            ++probe.materializations;
            probe.allocation = materializer(operation, {state, &reconcile});
            if (probe.reject_after_materialization) {
                throw std::bad_alloc{};
            }
            return {};
        } catch (...) {
            cleanup(operation);
            ++probe.cleanups;
            probe.reserved_bytes = 0u;
            throw;
        }
    }

    [[nodiscard]] context::backing_allocation_hook hook() noexcept {
        return {this, &materialize};
    }
};

/// Return the next deterministic process-footprint observation.
[[nodiscard]] std::uint64_t deterministic_footprint_observation() noexcept {
    const std::size_t index =
        materialization_observation_index.fetch_add(1u, std::memory_order_relaxed);
    return materialization_observations[std::min(index, materialization_observations.size() - 1u)];
}

/// Reset the deterministic footprint sequence for one materialization test.
void set_materialization_observations(std::uint64_t before, std::uint64_t after) noexcept {
    materialization_observations = {before, after, after, after};
    materialization_observation_index.store(0u, std::memory_order_relaxed);
}

/// Force unreflected ownership for context-allocation lifetime tests.
[[nodiscard]] std::uint64_t zero_context_footprint_observation() noexcept {
    return 0u;
}

/// Retain the largest admitted byte count observed around one replacement.
[[nodiscard]] std::uint64_t recording_zero_footprint_observation() noexcept {
    if (replacement_peak_admission != nullptr) {
        const std::uint64_t reserved = replacement_peak_admission->reserved_bytes();
        std::uint64_t observed = replacement_peak_reserved_bytes.load(std::memory_order_relaxed);
        while (observed < reserved &&
               !replacement_peak_reserved_bytes.compare_exchange_weak(
                   observed, reserved, std::memory_order_relaxed)) {
        }
    }
    return 0u;
}

/// Reflect one fixed-size initial arena block between each probe pair.
[[nodiscard]] std::uint64_t reflecting_context_footprint_observation() noexcept {
    const std::uint64_t call =
        context_reflection_probe_calls.fetch_add(1u, std::memory_order_relaxed);
    return 1'000'000u + ((call + 1u) / 2u) * context_reflected_block_bytes;
}

/// Return valid compact geometry with five equal, predictable initial blocks.
[[nodiscard]] context::context::arena_configuration context_test_configuration() noexcept {
    return {
        .scratch_block = k_context_test_block_bytes,
        .align_block = k_context_test_block_bytes,
        .chain_block = k_context_test_block_bytes,
        .hash_node_max = 64u,
        .hash_block = k_context_test_block_bytes,
        .result_node_max = 64u,
        .result_block = k_context_test_block_bytes,
    };
}

/// Scheduler state modeled by the tracked-pool unit fixture.
enum class fake_tracked_state : std::uint8_t {
    unused = 0u,
    enrolled,
    scheduled,
    finished,
};

/// Generation-checked state for one fake scheduler slot.
struct fake_tracked_slot_state {
    std::uint64_t generation = 0u;
    fake_tracked_state state = fake_tracked_state::unused;
    bioseq::parallelism::tracked_job_resources resources{};
    bool failed = false;
};

/// Complete fake scheduler owner used by observer callbacks.
struct fake_tracked_owner {
    std::array<fake_tracked_slot_state, 2> slots{};
    std::uint32_t live = 0u;
    std::uint32_t peak_live = 0u;
    std::uint32_t enrollments = 0u;
    std::uint32_t drains = 0u;
    std::atomic<std::uint32_t> worker_entries{0u};
    std::atomic<std::uint32_t> active_workers{0u};
    std::atomic<std::uint32_t> peak_active_workers{0u};
};

/// Enroll a tracked task in the first unused fake scheduler slot.
[[nodiscard]] bool fake_tracked_enroll(void* argument,
                                       bioseq::parallelism::tracked_job_resources resources,
                                       bioseq::parallelism::tracked_job_handle* handle) noexcept {
    auto* owner = static_cast<fake_tracked_owner*>(argument);
    if (owner == nullptr || handle == nullptr) {
        return false;
    }
    for (std::size_t index = 0u; index < owner->slots.size(); ++index) {
        auto& slot = owner->slots[index];
        if (slot.state != fake_tracked_state::unused) {
            continue;
        }
        slot.state = fake_tracked_state::enrolled;
        slot.resources = resources;
        slot.failed = false;
        ++owner->live;
        ++owner->enrollments;
        owner->peak_live = std::max(owner->peak_live, owner->live);
        *handle = {
            .slot_index = static_cast<std::uint32_t>(index),
            .generation = slot.generation,
        };
        return true;
    }
    return false;
}

/// Resolve a fake scheduler handle after validating its generation.
[[nodiscard]] fake_tracked_slot_state*
fake_tracked_slot(fake_tracked_owner* owner,
                  bioseq::parallelism::tracked_job_handle handle) noexcept {
    if (owner == nullptr || handle.slot_index >= owner->slots.size()) {
        return nullptr;
    }
    auto& slot = owner->slots[handle.slot_index];
    return slot.generation == handle.generation ? std::addressof(slot) : nullptr;
}

/// Apply the enrolled-to-scheduled fixture transition.
[[nodiscard]] bool
fake_tracked_mark_scheduled(void* argument,
                            bioseq::parallelism::tracked_job_handle handle) noexcept {
    auto* slot = fake_tracked_slot(static_cast<fake_tracked_owner*>(argument), handle);
    if (slot == nullptr || slot->state != fake_tracked_state::enrolled) {
        return false;
    }
    slot->state = fake_tracked_state::scheduled;
    return true;
}

/// Apply the scheduled-to-finished fixture transition.
[[nodiscard]] bool
fake_tracked_mark_finished(void* argument,
                           bioseq::parallelism::tracked_job_handle handle) noexcept {
    auto* slot = fake_tracked_slot(static_cast<fake_tracked_owner*>(argument), handle);
    if (slot == nullptr || slot->state != fake_tracked_state::scheduled) {
        return false;
    }
    slot->state = fake_tracked_state::finished;
    return true;
}

/// Drain callbacks and recycle one fake scheduler slot.
[[nodiscard]] bool fake_tracked_drain(void* argument,
                                      bioseq::parallelism::tracked_job_handle handle,
                                      bioseq::parallelism::tracked_job_outcome* outcome) noexcept {
    auto* owner = static_cast<fake_tracked_owner*>(argument);
    auto* slot = fake_tracked_slot(owner, handle);
    if (slot == nullptr || slot->state == fake_tracked_state::unused) {
        return false;
    }
    if (slot->state != fake_tracked_state::finished) {
        if (slot->resources.cancel.invoke != nullptr) {
            slot->resources.cancel.invoke(slot->resources.cancel.argument);
        }
        if (outcome != nullptr) {
            outcome->is_incomplete = true;
        }
    }
    if (slot->resources.join.invoke != nullptr) {
        slot->resources.join.invoke(slot->resources.join.argument);
    }
    if (slot->resources.backings.invoke != nullptr) {
        slot->resources.backings.invoke(slot->resources.backings.argument);
    }
    if (outcome != nullptr) {
        outcome->has_failure = slot->failed;
    }
    slot->resources = {};
    slot->state = fake_tracked_state::unused;
    ++slot->generation;
    --owner->live;
    ++owner->drains;
    return true;
}

/// Mark the addressed fake slot as failed.
void fake_tracked_handoff_error(void* argument,
                                bioseq::parallelism::tracked_job_handle handle,
                                std::uint32_t,
                                char const*,
                                bool) noexcept {
    auto* slot = fake_tracked_slot(static_cast<fake_tracked_owner*>(argument), handle);
    if (slot != nullptr) {
        slot->failed = true;
    }
}

/// Record entry into a fake scheduler worker scope.
void fake_tracked_worker_enter(void* argument,
                               bioseq::parallelism::tracked_job_handle,
                               bioseq::parallelism::tracked_worker_scope_storage*) noexcept {
    auto* owner = static_cast<fake_tracked_owner*>(argument);
    owner->worker_entries.fetch_add(1u, std::memory_order_relaxed);
    const std::uint32_t active =
        owner->active_workers.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    std::uint32_t observed = owner->peak_active_workers.load(std::memory_order_relaxed);
    while (observed < active &&
           !owner->peak_active_workers.compare_exchange_weak(
               observed, active, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

/// Record exit from a fake scheduler worker scope.
void fake_tracked_worker_leave(void* argument,
                               bioseq::parallelism::tracked_job_handle,
                               bioseq::parallelism::tracked_worker_scope_storage*) noexcept {
    auto* owner = static_cast<fake_tracked_owner*>(argument);
    owner->active_workers.fetch_sub(1u, std::memory_order_acq_rel);
}

/// Build the observer callback table for one fake scheduler owner.
[[nodiscard]] bioseq::parallelism::tracked_job_observer
make_fake_tracked_observer(fake_tracked_owner* owner) noexcept {
    return {
        .context = owner,
        .enroll = &fake_tracked_enroll,
        .mark_scheduled = &fake_tracked_mark_scheduled,
        .mark_finished = &fake_tracked_mark_finished,
        .drain = &fake_tracked_drain,
        .handoff_error = &fake_tracked_handoff_error,
        .worker_enter = &fake_tracked_worker_enter,
        .worker_leave = &fake_tracked_worker_leave,
    };
}

/// Task fixture that waits for an explicit release signal.
struct delayed_tracked_task {
    std::atomic<bool>* release = nullptr;
    std::atomic<std::uint32_t>* executions = nullptr;
};

/// Execute one delayed task fixture through its opaque task reference.
void run_delayed_tracked_task(void* argument) {
    auto* task = static_cast<delayed_tracked_task*>(argument);
    bool released = task->release->load(std::memory_order_acquire);
    while (!released) {
        task->release->wait(false, std::memory_order_acquire);
        released = task->release->load(std::memory_order_acquire);
    }
    task->executions->fetch_add(1u, std::memory_order_relaxed);
}

} // namespace

TEST_CASE("pool lifecycle and range execution",
          "[component:bioseq][layer:unit][feature:scheduler]") {
    auto& pool = bioseq::parallelism::global_pool::instance();
    pool.shutdown();
    REQUIRE(!pool.is_active());

    pool.configure(bioseq::parallelism::pool_config{
        .maximum_concurrency = 1,
        .mode = bioseq::parallelism::pipeline_concurrency_mode::single_thread,
    });
    REQUIRE(pool.is_active());
    REQUIRE(pool.maximum_concurrency() == 1u);

    std::atomic<std::uint64_t> sum = 0;
    pool.parallel_for(
        0, 8, 1, bioseq::parallelism::range_body{[&sum](std::size_t lo, std::size_t hi) {
            for (std::size_t value = lo; value < hi; ++value) {
                sum.fetch_add(value, std::memory_order_relaxed);
            }
        }});
    REQUIRE(sum.load(std::memory_order_relaxed) == 28u);

    pool.restart(bioseq::parallelism::pool_config{
        .maximum_concurrency = 2,
        .mode = bioseq::parallelism::pipeline_concurrency_mode::task_parallel,
    });
    REQUIRE(pool.maximum_concurrency() == 2u);
    REQUIRE(pool.get_mode() == bioseq::parallelism::pipeline_concurrency_mode::task_parallel);
    pool.shutdown();
}

TEST_CASE("pool installs worker contexts", "[component:bioseq][layer:unit][feature:scheduler]") {
    auto& pool = bioseq::parallelism::global_pool::instance();
    pool.configure(bioseq::parallelism::pool_config{
        .maximum_concurrency = 2,
        .mode = bioseq::parallelism::pipeline_concurrency_mode::task_parallel,
    });

    std::atomic<std::uint64_t> seen = 0;
    std::atomic<bool> missing_context = false;
    pool.parallel_for_with_context(
        0,
        16,
        1,
        bioseq::parallelism::range_body{[&seen, &missing_context](std::size_t lo, std::size_t hi) {
            if (context::current_or_null() == nullptr) {
                missing_context.store(true, std::memory_order_relaxed);
            }
            for (std::size_t value = lo; value < hi; ++value) {
                seen.fetch_add(1, std::memory_order_relaxed);
            }
        }});
    REQUIRE_FALSE(missing_context.load(std::memory_order_relaxed));
    REQUIRE(seen.load(std::memory_order_relaxed) == 16u);

    context::context outer_context;
    context::installed_context_scope install(outer_context);
    context::context* installed = context::current_or_null();
    std::atomic<bool> inherited_context = false;
    pool.parallel_for_with_context(
        0,
        1,
        1,
        bioseq::parallelism::range_body{[installed, &inherited_context](std::size_t, std::size_t) {
            inherited_context.store(context::current_or_null() == installed,
                                    std::memory_order_relaxed);
        }});
    REQUIRE(inherited_context.load(std::memory_order_relaxed));

    pool.shutdown();
}

TEST_CASE("tracked pool submissions are enrolled before later drain and reuse slots",
          "[component:bioseq][layer:unit][feature:scheduler]") {
    using namespace bioseq::parallelism;
    auto& pool = global_pool::instance();
    pool.restart(pool_config{
        .maximum_concurrency = 2,
        .mode = pipeline_concurrency_mode::task_parallel,
    });

    fake_tracked_owner owner{};
    const auto observer = make_fake_tracked_observer(std::addressof(owner));
    std::atomic<bool> release{false};
    std::atomic<std::uint32_t> executions{0u};
    std::array<delayed_tracked_task, 2> task_contexts{{
        {std::addressof(release), std::addressof(executions)},
        {std::addressof(release), std::addressof(executions)},
    }};
    std::array<tracked_task_submission, 2> submissions{};

    const auto first_status = pool.submit_tracked_task_noexcept(
        std::addressof(submissions[0]),
        {std::addressof(task_contexts[0]), &run_delayed_tracked_task},
        observer);
    const auto second_status = pool.submit_tracked_task_noexcept(
        std::addressof(submissions[1]),
        {std::addressof(task_contexts[1]), &run_delayed_tracked_task},
        observer);
    REQUIRE(first_status == tracked_submit_status::submitted);
    REQUIRE(second_status == tracked_submit_status::submitted);
    CHECK(owner.live == 2u);
    CHECK(owner.peak_live == 2u);
    CHECK(owner.drains == 0u);

    release.store(true, std::memory_order_release);
    release.notify_all();
    for (auto& submission : submissions) {
        const auto outcome = pool.join_and_drain_tracked_task_noexcept(std::addressof(submission));
        CHECK(outcome.is_drained);
        CHECK_FALSE(outcome.is_incomplete);
        CHECK_FALSE(outcome.has_failure);
        CHECK(submission.backing_released == 1u);
    }
    CHECK(executions.load(std::memory_order_relaxed) == 2u);
    CHECK(owner.live == 0u);
    CHECK(owner.drains == 2u);

    delayed_tracked_task reusable{std::addressof(release), std::addressof(executions)};
    tracked_task_submission reusable_submission{};
    for (std::uint32_t use = 0u; use < 128u; ++use) {
        const auto status =
            pool.submit_tracked_task_noexcept(std::addressof(reusable_submission),
                                              {std::addressof(reusable), &run_delayed_tracked_task},
                                              observer);
        REQUIRE(status == tracked_submit_status::submitted);
        const auto outcome =
            pool.join_and_drain_tracked_task_noexcept(std::addressof(reusable_submission));
        REQUIRE(outcome.is_drained);
        REQUIRE_FALSE(outcome.is_incomplete);
        REQUIRE_FALSE(outcome.has_failure);
    }
    CHECK(owner.live == 0u);
    CHECK(owner.enrollments == 130u);
    CHECK(owner.drains == 130u);
    CHECK(executions.load(std::memory_order_relaxed) == 130u);
    CHECK(owner.slots[0].generation + owner.slots[1].generation == 130u);
    pool.shutdown();
}

TEST_CASE("task-parallel nested scope gates inner regions",
          "[component:bioseq][layer:unit][feature:scheduler]") {
    auto& pool = bioseq::parallelism::global_pool::instance();
    pool.configure(bioseq::parallelism::pool_config{
        .maximum_concurrency = 2,
        .mode = bioseq::parallelism::pipeline_concurrency_mode::task_parallel,
    });

    std::atomic<std::uint64_t> outer_calls = 0;
    std::atomic<std::uint64_t> inner_calls = 0;
    bioseq::parallelism::parallel_for_pipeline(
        0,
        8,
        1,
        1,
        bioseq::parallelism::task_parallel_region::query_batch,
        [&outer_calls, &inner_calls](std::size_t lo, std::size_t hi) {
            outer_calls.fetch_add(1, std::memory_order_relaxed);
            bioseq::parallelism::parallel_for_pipeline(
                lo,
                hi,
                1,
                1,
                bioseq::parallelism::task_parallel_region::align_regions,
                [&inner_calls](std::size_t, std::size_t) {
                    inner_calls.fetch_add(1, std::memory_order_relaxed);
                });
        });

    REQUIRE(outer_calls.load(std::memory_order_relaxed) >= 1u);
    REQUIRE(inner_calls.load(std::memory_order_relaxed) ==
            outer_calls.load(std::memory_order_relaxed));
    pool.shutdown();
}

TEST_CASE("context block retention absorbs permits without a dynamic ledger",
          "[component:bioseq][layer:unit][feature:scheduler]") {
    using bioseq::parallelism::memory_admission_controller;
    using bioseq::parallelism::memory_permit;
    using bioseq::parallelism::operation_memory_retention;
    static_assert(sizeof(operation_memory_retention) == sizeof(memory_permit));
    static_assert(!std::is_copy_constructible_v<operation_memory_retention>);
    static_assert(std::is_nothrow_move_constructible_v<operation_memory_retention>);
    memory_admission_controller admission;
    memory_admission_controller unrelated;
    admission.set_index_memory_cap_bytes(1024u);
    unrelated.set_index_memory_cap_bytes(1024u);
    {
        operation_memory_retention retention;
        memory_permit empty;
        REQUIRE(retention.retain_context_block(std::move(empty)));
        REQUIRE(retention.retained_bytes() == 0u);
        auto first = admission.try_reserve(256u);
        auto second = admission.try_reserve(768u);
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        REQUIRE(retention.retain_context_block(std::move(*first)));
        REQUIRE(retention.retain_context_block(std::move(*second)));
        REQUIRE_FALSE(*first);
        REQUIRE_FALSE(*second);
        REQUIRE(retention.retained_bytes() == 1024u);
        REQUIRE(admission.reserved_bytes() == 1024u);
        REQUIRE_FALSE(admission.try_reserve(1u).has_value());

        auto foreign = unrelated.try_reserve(128u);
        REQUIRE(foreign.has_value());
        REQUIRE_FALSE(retention.retain_context_block(std::move(*foreign)));
        REQUIRE(foreign->bytes() == 128u);
        REQUIRE(retention.retained_bytes() == 1024u);
        REQUIRE(unrelated.reserved_bytes() == 128u);

        operation_memory_retention moved = std::move(retention);
        REQUIRE(retention.retained_bytes() == 0u);
        REQUIRE(moved.retained_bytes() == 1024u);
        REQUIRE(admission.reserved_bytes() == 1024u);
    }
    REQUIRE(admission.reserved_bytes() == 0u);
    REQUIRE(unrelated.reserved_bytes() == 0u);
}

TEST_CASE("context backing allocator reports native bytes and preserves aligned payload",
          "[component:bioseq][layer:unit][feature:scheduler]") {
    constexpr std::size_t bytes = 4097u;
    constexpr std::size_t alignment = 256u;
    const auto plan = bioseq::common::heap_allocation_detail::plan_aligned_heap_allocation(
        bytes, alignment, sizeof(void*));
    context_storage_transaction_probe probe;
    context::detail::context_block_allocator allocator{probe.hook()};
    probe.available_bytes = plan.allocation_requirement_bytes - 1u;
    REQUIRE_THROWS_AS(allocator.allocate_node(bytes, alignment), std::bad_alloc);
    REQUIRE(probe.materializations == 0u);
    probe.available_bytes = plan.allocation_requirement_bytes;
    auto release = [&allocator](std::byte* allocation) noexcept {
        allocator.deallocate_node(allocation, bytes, alignment);
    };
    std::unique_ptr<std::byte, decltype(release)> payload{
        static_cast<std::byte*>(allocator.allocate_node(bytes, alignment)), release};
    REQUIRE(payload != nullptr);
    REQUIRE(reinterpret_cast<std::uintptr_t>(payload.get()) % alignment == 0u);
    void* base = nullptr;
    std::memcpy(std::addressof(base), payload.get() - sizeof(void*), sizeof(void*));
    const auto physical_bytes = native_context_allocation_bytes(base);
    REQUIRE(physical_bytes > bytes);
    REQUIRE(probe.allocation.actual_allocation_bytes == physical_bytes);
    REQUIRE(probe.allocation.fully_touched_bytes == physical_bytes);
    REQUIRE(probe.reserved_bytes == physical_bytes);
    REQUIRE(std::all_of(payload.get(), payload.get() + bytes,
                        [](std::byte value) { return value == std::byte{}; }));
    payload.get()[0] = std::byte{1};
    payload.get()[bytes - 1u] = std::byte{2};
    payload.reset();

    probe.reject_after_materialization = true;
    REQUIRE_THROWS_AS(allocator.allocate_node(bytes, alignment), std::bad_alloc);
    REQUIRE(probe.cleanups == 1u);
    REQUIRE(probe.reserved_bytes == 0u);
    const auto attempts_before_invalid_request = probe.materializations;
    REQUIRE_THROWS_AS(allocator.allocate_node(bytes, 3u), std::invalid_argument);
    REQUIRE_THROWS_AS(
        allocator.allocate_node((std::numeric_limits<std::size_t>::max)(), alignment),
        std::overflow_error);
    REQUIRE(probe.materializations == attempts_before_invalid_request);
}

TEST_CASE("context oversize publication waits until the hook transaction accepts",
          "[component:bioseq][layer:unit][feature:scheduler]") {
    context_storage_transaction_probe probe;
    context::context owner{context_test_configuration(), probe.hook()};
    constexpr std::size_t bytes = (std::size_t{1} << 20u) + 4096u;
    {
        auto scratch_scope = owner.push<context::arena::scratch>();
        auto first = owner.alloc_scratch_array<std::byte, 256u>(bytes);
        REQUIRE(reinterpret_cast<std::uintptr_t>(first.data()) % 256u == 0u);
        first.front() = std::byte{42};
        const auto first_retained = owner.scratch_heap_fallback_bytes();
        const auto first_snapshot = probe.allocation;
        REQUIRE(first_retained >= bytes);
        REQUIRE(first_snapshot.primary_owner_bytes == first_retained);
        REQUIRE(first_snapshot.replacement_owner_bytes != 0u);
        probe.reject_after_materialization = true;
        REQUIRE_THROWS_AS((owner.alloc_scratch_array<std::byte, 256u>(bytes)), std::bad_alloc);
        REQUIRE(probe.cleanups == 1u);
        REQUIRE(owner.scratch_heap_fallback_bytes() == first_retained);
        REQUIRE(first.front() == std::byte{42});
        probe.reject_after_materialization = false;
        auto second = owner.alloc_scratch_array<std::byte, 256u>(bytes);
        REQUIRE(second.size() == bytes);
        REQUIRE(first.front() == std::byte{42});
        REQUIRE(owner.scratch_heap_fallback_bytes()
                == first_retained + probe.allocation.primary_owner_bytes);
        REQUIRE(probe.allocation.replacement_owner_bytes
                == 2u * first_snapshot.replacement_owner_bytes);
    }
    REQUIRE(owner.scratch_heap_fallback_bytes() == 0u);
}

TEST_CASE("route attempt installs backing admission before all initial context blocks",
          "[component:bioseq][layer:unit][feature:scheduler]") {
    using bioseq::parallelism::memory_admission_controller;
    using bioseq::simd::pipe::route_attempt_memory;
    static_assert(!std::is_copy_constructible_v<route_attempt_memory>);
    static_assert(!std::is_move_constructible_v<route_attempt_memory>);

    const auto block_geometry = context_test_allocation_geometry(k_context_test_block_bytes);
    const std::uint64_t initial_context_bytes = 5u * block_geometry.retained_bytes;
    const std::uint64_t initial_context_peak =
        4u * block_geometry.retained_bytes + block_geometry.admitted_bytes;
    const auto configuration = context_test_configuration();
    memory_admission_controller admission;

    admission.set_index_memory_cap_bytes(initial_context_peak - 1u);
    REQUIRE_THROWS_AS(
        route_attempt_memory(
            admission, configuration, zero_context_footprint_observation),
        std::bad_alloc);
    REQUIRE(admission.reserved_bytes() == 0u);

    admission.set_index_memory_cap_bytes(initial_context_peak);
    {
        route_attempt_memory attempt{
            admission, configuration, zero_context_footprint_observation};
        REQUIRE(attempt.retention.retained_bytes() == initial_context_bytes);
        REQUIRE(admission.reserved_bytes() == initial_context_bytes);
    }
    REQUIRE(admission.reserved_bytes() == 0u);
}

TEST_CASE("route worker factory joins before independent context permits unwind",
          "[component:bioseq][layer:unit][feature:scheduler]") {
    using bioseq::parallelism::memory_admission_controller;
    using bioseq::simd::pipe::route_worker_context_factory;
    auto& pool = bioseq::parallelism::global_pool::instance();
    pool.configure(bioseq::parallelism::pool_config{
        .maximum_concurrency = 2,
        .mode = bioseq::parallelism::pipeline_concurrency_mode::task_parallel,
    });
    memory_admission_controller admission;
    admission.set_index_memory_cap_bytes(1u << 20u);
    bioseq::simd::pipe::route_attempt_memory parent_attempt{
        admission, context_test_configuration(), zero_context_footprint_observation};
    auto& parent = parent_attempt.fresh_context;
    const auto parent_reserved_bytes = admission.reserved_bytes();
    context::installed_context_scope parent_scope{parent};
    std::array<std::atomic<unsigned>, 32> seen{};
    std::atomic<bool> foreign_context{false};
    {
        route_worker_context_factory factory{2u, parent_attempt, context_test_configuration()};
        REQUIRE(admission.reserved_bytes() > parent_reserved_bytes);
        pool.parallel_for_with_context(0u, seen.size(), 1u,
            [&seen, &foreign_context, &parent](std::size_t lo, std::size_t hi) {
                auto* current = context::current_or_null();
                if (current == nullptr || current == std::addressof(parent)) {
                    foreign_context.store(true, std::memory_order_relaxed);
                    return;
                }
                auto scratch_scope = current->push<context::arena::scratch>();
                auto storage = current->alloc_scratch_array<std::uint64_t>(32u);
                for (std::size_t index = lo; index < hi; ++index) {
                    storage[0] = index;
                    seen[index].fetch_add(1u, std::memory_order_relaxed);
                }
            }, factory.borrowed_factory());
        REQUIRE_FALSE(foreign_context.load(std::memory_order_relaxed));
        REQUIRE(context::current_or_null() == std::addressof(parent));
        for (auto& count : seen) {
            REQUIRE(count.load(std::memory_order_relaxed) == 1u);
        }
        REQUIRE(admission.reserved_bytes() >= 5u * k_context_test_block_bytes);
    }
    REQUIRE(admission.reserved_bytes() == parent_reserved_bytes);
    pool.shutdown();
}

TEST_CASE("route worker factory destroys partial slots after acquisition or body failure",
          "[component:bioseq][layer:unit][feature:scheduler]") {
    using bioseq::parallelism::memory_admission_controller;
    using bioseq::simd::pipe::route_worker_context_factory;
    auto& pool = bioseq::parallelism::global_pool::instance();
    pool.configure(bioseq::parallelism::pool_config{
        .maximum_concurrency = 1,
        .mode = bioseq::parallelism::pipeline_concurrency_mode::single_thread,
    });
    memory_admission_controller admission;
    admission.set_index_memory_cap_bytes(1u << 20u);
    {
        route_worker_context_factory factory{1u, admission,
            context_test_configuration(), zero_context_footprint_observation};
        const auto slots_bytes = admission.reserved_bytes();
        admission.set_index_memory_cap_bytes(slots_bytes + 5u * k_context_test_block_bytes - 1u);
        bool entered = false;
        REQUIRE_THROWS_AS(pool.parallel_for_with_context(0u, 1u, 1u,
            [&entered](std::size_t, std::size_t) { entered = true; },
            factory.borrowed_factory()), std::bad_alloc);
        REQUIRE_FALSE(entered);
        REQUIRE(admission.reserved_bytes() == slots_bytes);
    }
    REQUIRE(admission.reserved_bytes() == 0u);
    admission.set_index_memory_cap_bytes(1u << 20u);
    {
        route_worker_context_factory factory{1u, admission,
            context_test_configuration(), zero_context_footprint_observation};
        REQUIRE_THROWS_AS(pool.parallel_for_with_context(0u, 1u, 1u,
            [](std::size_t, std::size_t) { throw std::runtime_error{"worker body"}; },
            factory.borrowed_factory()), std::runtime_error);
        REQUIRE(admission.reserved_bytes() >= 5u * k_context_test_block_bytes);
    }
    REQUIRE(admission.reserved_bytes() == 0u);
    pool.shutdown();
}

TEST_CASE("route worker factory preserves coarse parallel and nested context ownership",
          "[component:bioseq][layer:unit][feature:scheduler]") {
    using namespace bioseq::parallelism;
    using bioseq::simd::pipe::route_attempt_memory;
    using bioseq::simd::pipe::route_worker_context_factory;
    auto& pool = global_pool::instance();
    pool.configure(pool_config{.maximum_concurrency = 2,
                               .mode = pipeline_concurrency_mode::task_parallel});
    memory_admission_controller admission;
    admission.set_index_memory_cap_bytes(1u << 20u);
    {
        route_attempt_memory parent{admission, context_test_configuration(),
                                    zero_context_footprint_observation};
        context::installed_context_scope installed{parent.fresh_context};
        for (const bool nested : {false, true}) {
            std::array<std::atomic<unsigned>, 32> seen{};
            std::atomic<bool> wrong_context{false};
            {
                route_worker_context_factory factory{2u, admission,
                    context_test_configuration(), zero_context_footprint_observation};
                std::optional<task_parallel_scope> outer;
                if (nested) {
                    outer.emplace(task_parallel_region::query_batch);
                    REQUIRE(outer->is_acquired());
                }
                parallel_for_pipeline(0u, seen.size(), 1u, 4u,
                    task_parallel_region::align_regions,
                    [&parent, &seen, &wrong_context, nested](std::size_t lo, std::size_t hi) {
                        auto* current = context::current_or_null();
                        if (current == nullptr || !current->has_backing_allocation_hook() ||
                            (current == &parent.fresh_context) != nested) {
                            wrong_context.store(true, std::memory_order_relaxed);
                            return;
                        }
                        auto scope = current->push<context::arena::scratch>();
                        auto scratch = current->alloc_scratch_array<std::uint64_t>(32u);
                        for (auto index = lo; index < hi; ++index) {
                            scratch[0] = index;
                            seen[index].fetch_add(1u, std::memory_order_relaxed);
                        }
                    }, factory.borrowed_factory());
                REQUIRE_FALSE(wrong_context.load(std::memory_order_relaxed));
                for (const auto& count : seen) {
                    REQUIRE(count.load(std::memory_order_relaxed) == 1u);
                }
                REQUIRE(context::current_or_null() == &parent.fresh_context);
            }
            REQUIRE(admission.reserved_bytes() == parent.retention.retained_bytes());
        }
    }
    REQUIRE(admission.reserved_bytes() == 0u);
    REQUIRE_FALSE(task_parallel_scope::is_active());
    pool.shutdown();
}

TEST_CASE("route worker factory tracks and drains admitted shards on success or failure",
          "[component:bioseq][layer:unit][feature:scheduler]") {
    using namespace bioseq::parallelism;
    using bioseq::simd::pipe::route_worker_context_factory;
    auto& pool = global_pool::instance();
    pool.configure(pool_config{
        .maximum_concurrency = 2,
        .mode = pipeline_concurrency_mode::task_parallel,
    });
    memory_admission_controller admission;
    admission.set_index_memory_cap_bytes(1u << 20u);
    context::context parent{context_test_configuration()};
    context::installed_context_scope parent_scope{parent};

    for (const bool fail_body : {false, true}) {
        fake_tracked_owner scheduler{};
        const auto observer = make_fake_tracked_observer(std::addressof(scheduler));
        std::array<std::atomic<unsigned>, 16> seen{};
        std::atomic<bool> foreign_context{false};
        {
            route_worker_context_factory factory{2u, admission,
                context_test_configuration(), zero_context_footprint_observation};
            const auto body = [&seen, &foreign_context, &parent, fail_body](
                std::size_t lo, std::size_t hi) {
                auto* current = context::current_or_null();
                if (current == nullptr || current == std::addressof(parent)) {
                    foreign_context.store(true, std::memory_order_relaxed);
                    return;
                }
                auto scope = current->push<context::arena::scratch>();
                auto scratch = current->alloc_scratch_array<std::uint64_t>(16u);
                for (std::size_t index = lo; index < hi; ++index) {
                    scratch[0] = index;
                    seen[index].fetch_add(1u, std::memory_order_relaxed);
                }
                if (fail_body && lo == 0u) {
                    throw std::runtime_error{"tracked admitted body failure"};
                }
            };
            if (fail_body) {
                REQUIRE_THROWS_AS(pool.parallel_for_with_context_tracked(
                    0u, seen.size(), 1u, body, observer, factory.borrowed_factory()),
                    std::runtime_error);
            } else {
                REQUIRE_NOTHROW(pool.parallel_for_with_context_tracked(
                    0u, seen.size(), 1u, body, observer, factory.borrowed_factory()));
            }
            REQUIRE(scheduler.enrollments == 2u);
            REQUIRE(scheduler.drains == 2u);
            REQUIRE(scheduler.live == 0u);
            REQUIRE(scheduler.worker_entries.load(std::memory_order_relaxed) == 2u);
            REQUIRE(scheduler.active_workers.load(std::memory_order_relaxed) == 0u);
            REQUIRE_FALSE(foreign_context.load(std::memory_order_relaxed));
            REQUIRE(context::current_or_null() == std::addressof(parent));
            for (auto& count : seen) {
                REQUIRE(count.load(std::memory_order_relaxed) == 1u);
            }
            REQUIRE(admission.reserved_bytes() >= 10u * k_context_test_block_bytes);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }
    {
        fake_tracked_owner scheduler{};
        const auto observer = make_fake_tracked_observer(std::addressof(scheduler));
        route_worker_context_factory factory{2u, admission,
            context_test_configuration(), zero_context_footprint_observation};
        const auto slots_bytes = admission.reserved_bytes();
        // Neither worker can obtain even its first backing block. Enrollment
        // still precedes execution and both failed submissions must be drained.
        admission.set_index_memory_cap_bytes(slots_bytes + k_context_test_block_bytes - 1u);
        std::atomic<unsigned> body_entries{0u};
        REQUIRE_THROWS_AS(pool.parallel_for_with_context_tracked(0u, 16u, 1u,
            [&body_entries](std::size_t, std::size_t) {
                body_entries.fetch_add(1u, std::memory_order_relaxed);
            }, observer, factory.borrowed_factory()), std::runtime_error);
        REQUIRE(body_entries.load(std::memory_order_relaxed) == 0u);
        REQUIRE(scheduler.enrollments == 2u);
        REQUIRE(scheduler.drains == 2u);
        REQUIRE(scheduler.live == 0u);
        REQUIRE(scheduler.active_workers.load(std::memory_order_relaxed) == 0u);
        REQUIRE(admission.reserved_bytes() == slots_bytes);
    }
    REQUIRE(admission.reserved_bytes() == 0u);
    pool.shutdown();
}

TEST_CASE("context hook admits later backing blocks but not covered suballocations",
          "[component:bioseq][layer:unit][feature:scheduler]") {
    using bioseq::parallelism::memory_admission_controller;
    using bioseq::simd::pipe::route_attempt_memory;

    const std::uint64_t initial_context_bytes =
        5u * context_test_allocation_geometry(k_context_test_block_bytes).retained_bytes;
    constexpr std::size_t extended_scratch_request = 6000u;
    const std::uint64_t next_scratch_block_bytes =
        context_test_allocation_geometry(2u * k_context_test_block_bytes).retained_bytes;
    memory_admission_controller admission;
    admission.set_index_memory_cap_bytes(1u << 20u);

    {
        route_attempt_memory attempt{
            admission,
            context_test_configuration(),
            zero_context_footprint_observation};
        REQUIRE(attempt.retention.retained_bytes() == initial_context_bytes);

        {
            auto scratch_scope = attempt.fresh_context.push<context::arena::scratch>();
            auto covered = attempt.fresh_context.make_scratch_vector<std::uint64_t>(8u);
            REQUIRE(covered.capacity() >= 8u);
            REQUIRE(attempt.retention.retained_bytes() == initial_context_bytes);

            auto extended = attempt.fresh_context.alloc_scratch_array<std::byte>(
                extended_scratch_request);
            REQUIRE(extended.size() == extended_scratch_request);
            REQUIRE(attempt.retention.retained_bytes()
                    == initial_context_bytes + next_scratch_block_bytes);
        }

        // Scope rewind caches the backing block; it does not release admission.
        REQUIRE(attempt.retention.retained_bytes()
                == initial_context_bytes + next_scratch_block_bytes);
        REQUIRE(admission.reserved_bytes()
                == initial_context_bytes + next_scratch_block_bytes);
    }
    REQUIRE(admission.reserved_bytes() == 0u);
}

TEST_CASE("context hook reconciles reflected initial backing allocations",
          "[component:bioseq][layer:unit][feature:scheduler]") {
    using bioseq::parallelism::memory_admission_controller;
    using bioseq::simd::pipe::route_attempt_memory;

    context_reflection_probe_calls.store(0u, std::memory_order_relaxed);
    const auto block_geometry = context_test_allocation_geometry(k_context_test_block_bytes);
    context_reflected_block_bytes = block_geometry.retained_bytes;
    memory_admission_controller admission;
    admission.set_index_memory_cap_bytes(block_geometry.admitted_bytes);
    {
        route_attempt_memory attempt{
            admission,
            context_test_configuration(),
            reflecting_context_footprint_observation};
        REQUIRE(attempt.retention.retained_bytes() == 0u);
        REQUIRE(admission.reserved_bytes() == 0u);
        REQUIRE(context_reflection_probe_calls.load(std::memory_order_relaxed) == 10u);
    }
    REQUIRE(admission.reserved_bytes() == 0u);
}

#if defined(BIOSEQ_TEST_INDEXES)
TEST_CASE("result node allocator admits the actual rebound allocation and alignment",
          "[component:bioseq][layer:unit][feature:storage]") {
    struct alignas(256) aligned_value {
        std::array<std::byte, 193u> data{};
    };
    using allocator_type = bioseq::simd::admitted_alignment_node_allocator<aligned_value>;
    bioseq::parallelism::memory_admission_controller admission;
    allocator_type allocator{admission, zero_context_footprint_observation};
    const auto plan = allocator_type::allocation_plan(1u);
    admission.set_index_memory_cap_bytes(plan.allocation_requirement_bytes - 1u);
    REQUIRE_THROWS_AS(allocator.allocate(1u), std::bad_alloc);
    REQUIRE(admission.reserved_bytes() == 0u);
    admission.set_index_memory_cap_bytes(plan.allocation_requirement_bytes);
    auto release = [&allocator](aligned_value* value) noexcept {
        allocator.deallocate(value, 1u);
    };
    std::unique_ptr<aligned_value, decltype(release)> allocation{allocator.allocate(1u), release};
    REQUIRE(allocation != nullptr);
    REQUIRE(reinterpret_cast<std::uintptr_t>(allocation.get()) % alignof(aligned_value) == 0u);
    void* base = nullptr;
    std::memcpy(std::addressof(base),
                reinterpret_cast<std::byte*>(allocation.get()) - sizeof(void*), sizeof(void*));
    const auto physical_bytes = native_context_allocation_bytes(base);
    REQUIRE(physical_bytes > sizeof(aligned_value));
    REQUIRE(admission.reserved_bytes() == physical_bytes);
    auto* value = std::construct_at(allocation.get());
    value->data.back() = std::byte{91};
    REQUIRE(value->data.back() == std::byte{91});
    std::destroy_at(value);
    allocation.reset();
    REQUIRE(admission.reserved_bytes() == 0u);
    REQUIRE(allocator.allocate(0u) == nullptr);
    REQUIRE_THROWS_AS(allocator.allocate((std::numeric_limits<std::size_t>::max)()),
                      std::bad_array_new_length);
    REQUIRE(admission.reserved_bytes() == 0u);
}

TEST_CASE("result map construction failure releases its real node permit",
          "[component:bioseq][layer:unit][feature:storage]") {
    struct rejecting_value {
        explicit rejecting_value(bool reject) {
            if (reject) {
                throw std::runtime_error{"intentional result-node construction rejection"};
            }
        }
    };
    using value_type = std::pair<const int, rejecting_value>;
    using allocator_type = bioseq::simd::admitted_alignment_node_allocator<value_type>;
    using map_type = std::map<int, rejecting_value, std::less<int>, allocator_type>;
    bioseq::parallelism::memory_admission_controller admission;
    admission.set_index_memory_cap_bytes(1u << 20u);
    {
        map_type values{std::less<int>{}, allocator_type{admission, zero_context_footprint_observation}};
        REQUIRE_THROWS_AS(values.try_emplace(1, true), std::runtime_error);
        REQUIRE(values.empty());
        REQUIRE(admission.reserved_bytes() == 0u);
        values.try_emplace(1, false);
        const auto retained = admission.reserved_bytes();
        REQUIRE(retained > sizeof(value_type));
        auto moved = std::move(values);
        REQUIRE(moved.contains(1));
        REQUIRE(admission.reserved_bytes() == retained);
        moved.erase(1);
        REQUIRE(admission.reserved_bytes() == 0u);
    }
    REQUIRE(admission.reserved_bytes() == 0u);
}

TEST_CASE("result map preserves over-aligned rebound nodes across unequal-controller moves",
          "[component:bioseq][layer:unit][feature:storage]") {
    struct alignas(256) aligned_value {
        std::uint64_t marker = 0u;
        explicit aligned_value(std::uint64_t value) noexcept : marker(value) {}
    };
    using value_type = std::pair<const int, aligned_value>;
    using allocator_type = bioseq::simd::admitted_alignment_node_allocator<value_type>;
    using map_type = std::map<int, aligned_value, std::less<int>, allocator_type>;
    bioseq::parallelism::memory_admission_controller first_admission;
    bioseq::parallelism::memory_admission_controller second_admission;
    first_admission.set_index_memory_cap_bytes(1u << 20u);
    second_admission.set_index_memory_cap_bytes(1u << 20u);
    const allocator_type first_allocator{first_admission, zero_context_footprint_observation};
    const allocator_type second_allocator{second_admission, zero_context_footprint_observation};
    {
        map_type first{std::less<int>{}, first_allocator};
        map_type second{std::less<int>{}, second_allocator};
        first.try_emplace(1, 11u);
        second.try_emplace(2, 22u);
        const auto first_bytes = first_admission.reserved_bytes();
        const auto second_bytes = second_admission.reserved_bytes();
        REQUIRE(first_bytes > sizeof(value_type));
        REQUIRE(second_bytes > sizeof(value_type));
        REQUIRE(reinterpret_cast<std::uintptr_t>(std::addressof(first.at(1))) % 256u == 0u);
        REQUIRE(reinterpret_cast<std::uintptr_t>(std::addressof(second.at(2))) % 256u == 0u);
        first.swap(second);
        REQUIRE(first.get_allocator() == second_allocator);
        REQUIRE(second.get_allocator() == first_allocator);
        REQUIRE(first.at(2).marker == 22u);
        REQUIRE(second.at(1).marker == 11u);
        REQUIRE(first_admission.reserved_bytes() == first_bytes);
        REQUIRE(second_admission.reserved_bytes() == second_bytes);
        first = std::move(second);
        REQUIRE(first.get_allocator() == first_allocator);
        REQUIRE(first.at(1).marker == 11u);
        REQUIRE(second_admission.reserved_bytes() == 0u);
        REQUIRE(first_admission.reserved_bytes() == first_bytes);
        first.clear();
        REQUIRE(first_admission.reserved_bytes() == 0u);
    }
    REQUIRE(first_admission.reserved_bytes() == 0u);
    REQUIRE(second_admission.reserved_bytes() == 0u);
}

TEST_CASE("admitted alignment vector replaces capacity at the exact adjacent peak",
          "[component:bioseq][layer:unit][feature:storage]") {
    using bioseq::parallelism::k_vector_preallocation_headroom_factor;
    using bioseq::parallelism::memory_admission_controller;
    using bioseq::simd::pipe::route_attempt_memory;
    using storage_type = bioseq::simd::test_alignment_result_vector<std::uint64_t>;

    memory_admission_controller admission;
    admission.set_index_memory_cap_bytes(1u << 20u);
    replacement_peak_admission = std::addressof(admission);
    struct replacement_probe_reset_guard {
        ~replacement_probe_reset_guard() { replacement_peak_admission = nullptr; }
    } reset_replacement_probe;
    replacement_peak_reserved_bytes.store(0u, std::memory_order_relaxed);
    {
        route_attempt_memory attempt{
            admission,
            context_test_configuration(),
            recording_zero_footprint_observation};
        context::installed_context_scope installed{attempt.fresh_context};
        const std::uint64_t context_baseline = admission.reserved_bytes();

        storage_type values;
        values.set_logical_bounds(8u, 8u);
        const std::uint64_t initial_peak_bytes =
            4u * sizeof(std::uint64_t) * k_vector_preallocation_headroom_factor;
        admission.set_index_memory_cap_bytes(context_baseline + initial_peak_bytes);
        REQUIRE_NOTHROW(values.reserve(4u));
        REQUIRE(values.capacity() >= 4u);
        values.push_back(17u);
        values.push_back(29u);
        const std::uint64_t old_capacity_bytes =
            static_cast<std::uint64_t>(values.capacity()) * sizeof(std::uint64_t);
        REQUIRE(admission.reserved_bytes() == context_baseline + old_capacity_bytes);

        const std::uint64_t before_replacement = admission.reserved_bytes();
        const std::uint64_t replacement_bound_bytes =
            8u * sizeof(std::uint64_t) * k_vector_preallocation_headroom_factor;
        replacement_peak_reserved_bytes.store(before_replacement, std::memory_order_relaxed);
        admission.set_index_memory_cap_bytes(before_replacement + replacement_bound_bytes);
        REQUIRE_NOTHROW(values.reserve(8u));
        REQUIRE(values.size() == 2u);
        CHECK(values[0] == 17u);
        CHECK(values[1] == 29u);
        CHECK(replacement_peak_reserved_bytes.load(std::memory_order_relaxed) ==
              before_replacement + replacement_bound_bytes);

        const std::uint64_t stable_reserved = admission.reserved_bytes();
        const std::size_t stable_capacity = values.capacity();
        admission.set_index_memory_cap_bytes(
            stable_reserved + replacement_bound_bytes - 1u);
        REQUIRE_THROWS_AS(values.reserve(8u + 1u), std::length_error);
        CHECK(values.capacity() == stable_capacity);
        CHECK(values.size() == 2u);
        CHECK(values[0] == 17u);
        CHECK(values[1] == 29u);
        CHECK(admission.reserved_bytes() == stable_reserved);

        // A prior legitimate reserve may leave physical capacity above a later
        // logical plan. The bound still rejects a request before the capacity
        // early-return path can hide it.
        values.set_logical_bounds(values.size(), values.size());
        REQUIRE(values.capacity() > values.size());
        REQUIRE_THROWS_AS(values.reserve(values.size() + 1u), std::length_error);
        CHECK(values.capacity() == stable_capacity);
        CHECK(values.size() == 2u);
    }
    REQUIRE(admission.reserved_bytes() == 0u);
}

TEST_CASE("admitted alignment vector preserves old ownership on one-short denial",
          "[component:bioseq][layer:unit][feature:storage]") {
    using bioseq::parallelism::k_vector_preallocation_headroom_factor;
    using bioseq::parallelism::memory_admission_controller;
    using bioseq::simd::pipe::route_attempt_memory;
    using storage_type = bioseq::simd::test_alignment_result_vector<std::uint32_t>;

    memory_admission_controller admission;
    admission.set_index_memory_cap_bytes(1u << 20u);
    {
        route_attempt_memory attempt{
            admission,
            context_test_configuration(),
            zero_context_footprint_observation};
        context::installed_context_scope installed{attempt.fresh_context};
        const std::uint64_t context_baseline = admission.reserved_bytes();

        storage_type values;
        values.set_logical_bounds(8u, 8u);
        values.reserve(4u);
        values.push_back(3u);
        values.push_back(5u);
        const std::uint64_t stable_reserved = admission.reserved_bytes();
        const std::size_t stable_capacity = values.capacity();
        const std::uint64_t denied_bound =
            8u * sizeof(std::uint32_t) * k_vector_preallocation_headroom_factor;
        admission.set_index_memory_cap_bytes(stable_reserved + denied_bound - 1u);
        REQUIRE_THROWS_AS(values.reserve(8u), std::bad_alloc);
        CHECK(values.capacity() == stable_capacity);
        CHECK(values.size() == 2u);
        CHECK(values[0] == 3u);
        CHECK(values[1] == 5u);
        CHECK(admission.reserved_bytes() == stable_reserved);

        values.clear();
        CHECK(admission.reserved_bytes() == stable_reserved);
        values.set_logical_bounds(0u, 0u);
        CHECK(values.capacity() == stable_capacity);
        CHECK(admission.reserved_bytes() > context_baseline);
    }
    REQUIRE(admission.reserved_bytes() == 0u);
}

TEST_CASE("fully touched trivial alignment capacity releases a reflected permit",
          "[component:bioseq][layer:unit][feature:storage]") {
    using bioseq::parallelism::memory_admission_controller;
    using bioseq::simd::pipe::route_attempt_memory;
    using storage_type = bioseq::simd::test_alignment_result_vector<std::uint64_t>;

    memory_admission_controller admission;
    admission.set_index_memory_cap_bytes(1u << 20u);
    set_materialization_observations(0u, 0u);
    {
        route_attempt_memory attempt{
            admission,
            context_test_configuration(),
            deterministic_footprint_observation};
        context::installed_context_scope installed{attempt.fresh_context};
        const std::uint64_t context_baseline = admission.reserved_bytes();
        set_materialization_observations(50'000u, 50'000u + sizeof(std::uint64_t));

        storage_type values;
        values.set_logical_bounds(1u, 1u);
        values.reserve(1u);
        REQUIRE(values.capacity() == 1u);
        CHECK(admission.reserved_bytes() == context_baseline);
        values.push_back(41u);
        CHECK(values[0] == 41u);
    }
    REQUIRE(admission.reserved_bytes() == 0u);
}

TEST_CASE("outer match results retain exact replacement ownership across map transfer",
          "[component:bioseq][layer:unit][feature:storage]") {
    using bioseq::parallelism::k_vector_preallocation_headroom_factor;
    using bioseq::parallelism::memory_admission_controller;
    using bioseq::simd::match_result;
    using storage_type = bioseq::simd::test_match_result_vector;
    static_assert(!std::is_copy_constructible_v<storage_type>);
    static_assert(std::is_nothrow_move_constructible_v<storage_type>);

    memory_admission_controller admission;
    admission.set_index_memory_cap_bytes(1u << 20u);
    replacement_peak_admission = std::addressof(admission);
    struct replacement_probe_reset_guard {
        ~replacement_probe_reset_guard() { replacement_peak_admission = nullptr; }
    } reset_replacement_probe;
    {
        const std::uint64_t context_baseline = admission.reserved_bytes();

        storage_type results{admission, recording_zero_footprint_observation};
        results.set_logical_bounds(2u, 2u);
        const std::uint64_t first_bound =
            sizeof(match_result) * k_vector_preallocation_headroom_factor;
        admission.set_index_memory_cap_bytes(context_baseline + first_bound);
        results.reserve(1u);
        match_result first{};
        first.dp_score = 71;
        first.cigar.push_back({9u, bioseq::simd::cigar_match_opcode});
        results.push_back(std::move(first));

        const std::uint64_t before_replacement = admission.reserved_bytes();
        const std::uint64_t replacement_bound =
            2u * sizeof(match_result) * k_vector_preallocation_headroom_factor;
        replacement_peak_reserved_bytes.store(before_replacement, std::memory_order_relaxed);
        admission.set_index_memory_cap_bytes(before_replacement + replacement_bound);
        results.reserve(2u);
        REQUIRE(results.size() == 1u);
        CHECK(results[0].dp_score == 71);
        REQUIRE(results[0].cigar.size() == 1u);
        CHECK(results[0].cigar[0].length == 9u);
        CHECK(replacement_peak_reserved_bytes.load(std::memory_order_relaxed) ==
              before_replacement + replacement_bound);

        const std::uint64_t admitted_outer_bytes = admission.reserved_bytes();
        {
            using map_type = bioseq::simd::test_match_result_map<std::int32_t>;
            admission.set_index_memory_cap_bytes(1u << 20u);
            map_type mapped_results{
                std::less<std::int32_t>{},
                typename map_type::allocator_type{admission, zero_context_footprint_observation}};
            mapped_results.try_emplace(4);
            const auto node_bytes = admission.reserved_bytes() - admitted_outer_bytes;
            REQUIRE(node_bytes > sizeof(typename map_type::value_type));
            mapped_results.at(4) = std::move(results);
            REQUIRE(mapped_results.at(4).size() == 1u);
            CHECK(mapped_results.at(4)[0].dp_score == 71);
            // Moving the vector transfers its existing lease; only the
            // independently allocated real tree node adds physical bytes.
            CHECK(admission.reserved_bytes() == admitted_outer_bytes + node_bytes);
        }
        CHECK(admission.reserved_bytes() == context_baseline);
    }
    REQUIRE(admission.reserved_bytes() == 0u);
}

TEST_CASE("outer match result denial preserves source and reflected probes keep its lease",
          "[component:bioseq][layer:unit][feature:storage]") {
    using bioseq::parallelism::k_vector_preallocation_headroom_factor;
    using bioseq::parallelism::memory_admission_controller;
    using bioseq::simd::match_result;
    using storage_type = bioseq::simd::test_match_result_vector;

    memory_admission_controller denial_admission;
    denial_admission.set_index_memory_cap_bytes(1u << 20u);
    {
        storage_type results{denial_admission, zero_context_footprint_observation};
        results.set_logical_bounds(2u, 2u);
        results.reserve(1u);
        match_result retained{};
        retained.dp_score = 83;
        retained.query_start = 11u;
        retained.cigar.push_back({5u, bioseq::simd::cigar_equal_opcode});
        results.push_back(std::move(retained));

        const std::uint64_t stable_reserved = denial_admission.reserved_bytes();
        const std::size_t stable_capacity = results.capacity();
        const std::uint64_t replacement_bound =
            2u * sizeof(match_result) * k_vector_preallocation_headroom_factor;
        denial_admission.set_index_memory_cap_bytes(
            stable_reserved + replacement_bound - 1u);
        REQUIRE_THROWS_AS(results.reserve(2u), std::bad_alloc);
        CHECK(denial_admission.reserved_bytes() == stable_reserved);
        CHECK(results.capacity() == stable_capacity);
        REQUIRE(results.size() == 1u);
        CHECK(results[0].dp_score == 83);
        CHECK(results[0].query_start == 11u);
        REQUIRE(results[0].cigar.size() == 1u);
        CHECK(results[0].cigar[0].length == 5u);
    }
    REQUIRE(denial_admission.reserved_bytes() == 0u);

    memory_admission_controller reflected_admission;
    reflected_admission.set_index_memory_cap_bytes(1u << 20u);
    set_materialization_observations(0u, 0u);
    {
        const std::uint64_t context_baseline = reflected_admission.reserved_bytes();
        set_materialization_observations(
            70'000u, 70'000u + static_cast<std::uint64_t>(sizeof(match_result)));

        storage_type results{reflected_admission, deterministic_footprint_observation};
        results.set_logical_bounds(1u, 1u);
        results.reserve(1u);
        REQUIRE(results.capacity() == 1u);
        // A move-only result replacement cannot touch unconstructed spare
        // elements, so even an equal footprint delta cannot release its lease.
        CHECK(reflected_admission.reserved_bytes() ==
              context_baseline + sizeof(match_result));
    }
    REQUIRE(reflected_admission.reserved_bytes() == 0u);
}

TEST_CASE("batched result collections replace and unwind their outer storage exactly",
          "[component:bioseq][layer:unit][feature:storage]") {
    using bioseq::parallelism::k_vector_preallocation_headroom_factor;
    using bioseq::parallelism::memory_admission_controller;
    using bioseq::simd::match_result;
    using bioseq::simd::match_result_collection;
    using storage_type = bioseq::simd::test_match_result_collection_vector;
    static_assert(!std::is_copy_constructible_v<storage_type>);
    static_assert(std::is_nothrow_move_constructible_v<storage_type>);

    const std::uint64_t two_collection_replacement_bound =
        2u * sizeof(match_result_collection) * k_vector_preallocation_headroom_factor;
    std::uint64_t one_collection_bytes = 0u;
    memory_admission_controller success_admission;
    success_admission.set_index_memory_cap_bytes(1u << 20u);
    replacement_peak_admission = std::addressof(success_admission);
    struct replacement_probe_reset_guard {
        ~replacement_probe_reset_guard() { replacement_peak_admission = nullptr; }
    } reset_replacement_probe;
    {
        storage_type collections{success_admission, recording_zero_footprint_observation};
        collections.set_logical_bounds(2u, 2u);
        collections.reserve(1u);
        collections.resize(1u);
        match_result retained{};
        retained.dp_score = 97;
        collections[0].results.push_back(std::move(retained));

        const std::uint64_t before_replacement = success_admission.reserved_bytes();
        one_collection_bytes = before_replacement;
        REQUIRE(one_collection_bytes > 0u);
        replacement_peak_reserved_bytes.store(before_replacement, std::memory_order_relaxed);
        collections.reserve(2u);
        REQUIRE(success_admission.reserved_bytes() > 0u);
        CHECK(replacement_peak_reserved_bytes.load(std::memory_order_relaxed) ==
              before_replacement + two_collection_replacement_bound);
        REQUIRE(collections.size() == 1u);
        REQUIRE(collections[0].results.size() == 1u);
        CHECK(collections[0].results[0].dp_score == 97);

        // A narrower later logical plan rejects a request even when the prior
        // legitimate physical capacity is already equal to that request.
        collections.set_logical_bounds(1u, 1u);
        REQUIRE(collections.capacity() >= 2u);
        REQUIRE_THROWS_AS(collections.reserve(2u), std::length_error);

        const std::uint64_t before_move = success_admission.reserved_bytes();
        storage_type moved = std::move(collections);
        CHECK(success_admission.reserved_bytes() == before_move);
        REQUIRE(moved[0].results.size() == 1u);
        CHECK(moved[0].results[0].dp_score == 97);
    }
    REQUIRE(success_admission.reserved_bytes() == 0u);

    memory_admission_controller denial_admission;
    denial_admission.set_index_memory_cap_bytes(1u << 20u);
    {
        storage_type collections{denial_admission, zero_context_footprint_observation};
        collections.set_logical_bounds(2u, 2u);
        collections.reserve(1u);
        collections.resize(1u);
        collections[0].alignment_result_host_wall_ns = 123u;
        const std::uint64_t stable_reserved = denial_admission.reserved_bytes();
        denial_admission.set_index_memory_cap_bytes(
            stable_reserved + two_collection_replacement_bound - 1u);
        REQUIRE_THROWS_AS(collections.reserve(2u), std::bad_alloc);
        CHECK(collections.size() == 1u);
        CHECK(collections[0].alignment_result_host_wall_ns == 123u);
        CHECK(denial_admission.reserved_bytes() == stable_reserved);
    }
    REQUIRE(denial_admission.reserved_bytes() == 0u);

    memory_admission_controller reflected_admission;
    reflected_admission.set_index_memory_cap_bytes(1u << 20u);
    set_materialization_observations(110'000u, 110'000u + one_collection_bytes);
    {
        storage_type collections{reflected_admission, deterministic_footprint_observation};
        collections.set_logical_bounds(1u, 1u);
        collections.reserve(1u);
        // The move-only collection cannot touch unconstructed spare storage,
        // so an equal process-footprint delta does not discharge its lease.
        CHECK(reflected_admission.reserved_bytes() == one_collection_bytes);
    }
    REQUIRE(reflected_admission.reserved_bytes() == 0u);
}

TEST_CASE("nontrivial outer elements are destroyed before their capacity lease",
          "[component:bioseq][layer:unit][feature:storage]") {
    using bioseq::parallelism::memory_admission_controller;

    struct observed_result {
        memory_admission_controller* admission = nullptr;
        const std::uint64_t* expected_reserved = nullptr;
        bool* destroyed = nullptr;
        bool* lease_was_live = nullptr;
        bool armed = true;

        observed_result(memory_admission_controller* owner,
                        const std::uint64_t* expected,
                        bool* destroyed_flag,
                        bool* live_flag) noexcept
            : admission(owner), expected_reserved(expected), destroyed(destroyed_flag),
              lease_was_live(live_flag) {}
        observed_result(const observed_result&) = delete;
        observed_result& operator=(const observed_result&) = delete;
        observed_result(observed_result&& other) noexcept
            : admission(other.admission), expected_reserved(other.expected_reserved),
              destroyed(other.destroyed), lease_was_live(other.lease_was_live),
              armed(std::exchange(other.armed, false)) {}
        observed_result& operator=(observed_result&&) = delete;
        ~observed_result() {
            if (armed) {
                *destroyed = true;
                *lease_was_live = admission->reserved_bytes() == *expected_reserved;
            }
        }
    };
    static_assert(std::is_nothrow_move_constructible_v<observed_result>);

    memory_admission_controller admission;
    admission.set_index_memory_cap_bytes(1u << 20u);
    bool destroyed = false;
    bool lease_was_live = false;
    {
        const std::uint64_t context_baseline = admission.reserved_bytes();
        std::uint64_t expected_reserved = 0u;
        {
            bioseq::simd::test_outer_alignment_result_vector<observed_result> results{
                admission, zero_context_footprint_observation};
            results.set_logical_bounds(1u, 1u);
            results.reserve(1u);
            expected_reserved = admission.reserved_bytes();
            results.emplace_back(
                std::addressof(admission),
                std::addressof(expected_reserved),
                std::addressof(destroyed),
                std::addressof(lease_was_live));
        }
        CHECK(destroyed);
        CHECK(lease_was_live);
        CHECK(admission.reserved_bytes() == context_baseline);
    }
    REQUIRE(admission.reserved_bytes() == 0u);
}
#endif

TEST_CASE("oversize context storage and tracker replacement keep adjacent permits",
          "[component:bioseq][layer:unit][feature:scheduler]") {
    using bioseq::parallelism::memory_admission_controller;
    using bioseq::simd::pipe::route_attempt_memory;

    const std::uint64_t initial_context_bytes =
        5u * context_test_allocation_geometry(k_context_test_block_bytes).retained_bytes;
    constexpr std::size_t oversize_bytes = (std::size_t{1} << 20u) + 4096u;
    const auto oversize_geometry = context_test_allocation_geometry(
        oversize_bytes, sizeof(void*), 0u);
    memory_admission_controller admission;
    admission.set_index_memory_cap_bytes(16u << 20u);

    {
        route_attempt_memory attempt{
            admission,
            context_test_configuration(),
            zero_context_footprint_observation};
        REQUIRE(attempt.retention.retained_bytes() == initial_context_bytes);

        std::uint64_t reserved_with_blocks = 0u;
        {
            auto scratch_scope = attempt.fresh_context.push<context::arena::scratch>();
            auto first = attempt.fresh_context.alloc_scratch_array<std::byte>(oversize_bytes);
            REQUIRE(first.size() == oversize_bytes);
            REQUIRE(attempt.fresh_context.scratch_heap_fallback_bytes()
                    == oversize_geometry.retained_bytes);
            const std::uint64_t reserved_with_first_block = admission.reserved_bytes();
            REQUIRE(reserved_with_first_block > initial_context_bytes + oversize_bytes);

            const std::uint64_t first_tracker_bytes =
                reserved_with_first_block - initial_context_bytes - oversize_geometry.retained_bytes;
            REQUIRE(first_tracker_bytes != 0u);
            admission.set_index_memory_cap_bytes(
                reserved_with_first_block + oversize_geometry.admitted_bytes
                    + 2u * first_tracker_bytes - 1u);
            REQUIRE_THROWS_AS(
                attempt.fresh_context.alloc_scratch_array<std::byte>(oversize_bytes),
                std::bad_alloc);
            REQUIRE(admission.reserved_bytes() == reserved_with_first_block);
            REQUIRE(attempt.fresh_context.scratch_heap_fallback_bytes()
                    == oversize_geometry.retained_bytes);

            admission.set_index_memory_cap_bytes(16u << 20u);
            auto second =
                attempt.fresh_context.alloc_scratch_array<std::byte>(oversize_bytes);
            REQUIRE(second.size() == oversize_bytes);
            REQUIRE(attempt.fresh_context.scratch_heap_fallback_bytes()
                    == 2u * oversize_geometry.retained_bytes);
            reserved_with_blocks = admission.reserved_bytes();
            REQUIRE(reserved_with_blocks
                    > reserved_with_first_block + oversize_bytes);
        }

        REQUIRE(attempt.fresh_context.scratch_heap_fallback_bytes() == 0u);
        REQUIRE(admission.reserved_bytes()
                == reserved_with_blocks - 2u * oversize_geometry.retained_bytes);
        REQUIRE(admission.reserved_bytes() > initial_context_bytes);
    }
    REQUIRE(admission.reserved_bytes() == 0u);
}

TEST_CASE("memory admission reserves and releases",
          "[component:bioseq][layer:unit][feature:scheduler]") {
    bioseq::parallelism::memory_admission_controller admission;
    admission.set_index_memory_cap_bytes(1024);
    REQUIRE(admission.reserved_bytes() == 0u);

    {
        auto permit = admission.try_reserve(512);
        REQUIRE(permit.has_value());
        REQUIRE(admission.reserved_bytes() == 512u);
        permit->shrink_to(128);
        REQUIRE(permit->bytes() == 128u);
        REQUIRE(admission.reserved_bytes() == 128u);
        permit->shrink_to(256);
        REQUIRE(permit->bytes() == 128u);
        REQUIRE(admission.reserved_bytes() == 128u);

        bioseq::parallelism::memory_admission_failure failure{};
        auto denied = admission.try_reserve(1024, &failure);
        REQUIRE_FALSE(denied.has_value());
        REQUIRE(failure.reason ==
                bioseq::parallelism::memory_admission_failure_reason::budget_exhausted);
        REQUIRE(failure.requested_bytes == 1024u);
        REQUIRE(failure.reserved_bytes == 128u);
        REQUIRE(failure.allocatable_after_reservations_bytes <= 896u);
        REQUIRE(failure.budget_ceiling_bytes >= failure.reserved_bytes);
        REQUIRE(failure.allocatable_after_reservations_bytes ==
                failure.budget_ceiling_bytes - failure.reserved_bytes);
        REQUIRE((failure.system_available_bytes <= failure.system_total_bytes ||
                 failure.system_total_bytes == 0u));
        REQUIRE((failure.process_soft_cap_bytes <= failure.system_total_bytes ||
                 failure.system_total_bytes == 0u));
    }
    REQUIRE(admission.reserved_bytes() == 0u);

    auto wave = admission.try_reserve_wave(4, 400);
    REQUIRE(wave.is_admitted);
    REQUIRE(wave.fanout <= 2u);
    wave.permit.release();
    REQUIRE(admission.reserved_bytes() == 0u);
    admission.set_index_memory_cap_bytes(0);
}

TEST_CASE("admitted vector capacity is destroyed before its reservation is released",
          "[component:bioseq][layer:unit][feature:scheduler]") {
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    /// Restores the process-wide admission cap after the fixture unwinds.
    struct restore_cap_guard {
        bioseq::parallelism::memory_admission_controller& admission;
        std::uint64_t previous = 0;
        ~restore_cap_guard() {
            admission.set_index_memory_cap_bytes(previous);
        }
    } restore_cap{admission, previous_cap};
    const std::uint64_t baseline = admission.reserved_bytes();
    admission.set_index_memory_cap_bytes(std::max<std::uint64_t>(previous_cap, baseline + 4096u));

    /// Records whether vector elements are destroyed while their permit is live.
    struct destruction_observer {
        bioseq::parallelism::memory_admission_controller* admission = nullptr;
        std::uint64_t expected_reserved = 0;
        bool* destroyed = nullptr;
        bool* reservation_was_live = nullptr;

        destruction_observer(bioseq::parallelism::memory_admission_controller* owner,
                             std::uint64_t expected,
                             bool* destroyed_flag,
                             bool* live_flag) noexcept
            : admission(owner), expected_reserved(expected), destroyed(destroyed_flag),
              reservation_was_live(live_flag) {}

        ~destruction_observer() {
            *destroyed = true;
            *reservation_was_live = admission->reserved_bytes() == expected_reserved;
        }
    };

    bool explicit_destroyed = false;
    bool explicit_reservation_live = false;
    {
        auto permit = admission.try_reserve(512u);
        REQUIRE(permit.has_value());
        std::vector<destruction_observer> values;
        values.reserve(4u);
        values.emplace_back(
            &admission, baseline + 512u, &explicit_destroyed, &explicit_reservation_live);
        bioseq::parallelism::release_admitted_vector_capacity_before_permit(values, *permit);
        REQUIRE(values.capacity() == 0u);
        REQUIRE(admission.reserved_bytes() == baseline);
    }
    REQUIRE(explicit_destroyed);
    REQUIRE(explicit_reservation_live);

    // Mirrors MemoryRun's production member order: reverse destruction must
    // destroy the vector before the permit's destructor releases its bytes.
    bool member_destroyed = false;
    bool member_reservation_live = false;
    {
        auto permit = admission.try_reserve(512u);
        REQUIRE(permit.has_value());
        /// Mirrors the permit-before-vector declaration order of admitted owners.
        struct ordered_admitted_vector {
            bioseq::parallelism::memory_permit permit;
            std::vector<destruction_observer> values;
        } admitted{
            .permit = std::move(*permit),
            .values = {},
        };
        admitted.values.emplace_back(
            &admission, baseline + 512u, &member_destroyed, &member_reservation_live);
    }
    REQUIRE(member_destroyed);
    REQUIRE(member_reservation_live);
    REQUIRE(admission.reserved_bytes() == baseline);

    bool first_buffer_destroyed = false;
    bool first_buffer_reservation_live = false;
    bool second_buffer_destroyed = false;
    bool second_buffer_reservation_live = false;
    {
        constexpr std::size_t units_per_buffer = 4u;
        const std::uint64_t logical_sum = 2u * units_per_buffer * sizeof(destruction_observer);
        const std::uint64_t preallocation_peak =
            logical_sum * bioseq::parallelism::k_vector_preallocation_headroom_factor;
        auto permit = admission.try_reserve(preallocation_peak);
        REQUIRE(permit.has_value());
        REQUIRE(admission.reserved_bytes() == baseline + preallocation_peak);

        std::vector<destruction_observer> first;
        std::vector<destruction_observer> second;
        first.reserve(units_per_buffer);
        second.reserve(units_per_buffer);
        const std::uint64_t allocated_capacity_bytes =
            static_cast<std::uint64_t>(first.capacity() + second.capacity()) *
            sizeof(destruction_observer);
        REQUIRE(allocated_capacity_bytes <= permit->bytes());
        permit->shrink_to(allocated_capacity_bytes);
        REQUIRE(admission.reserved_bytes() == baseline + allocated_capacity_bytes);
        first.emplace_back(&admission,
                           baseline + allocated_capacity_bytes,
                           &first_buffer_destroyed,
                           &first_buffer_reservation_live);
        second.emplace_back(&admission,
                            baseline + allocated_capacity_bytes,
                            &second_buffer_destroyed,
                            &second_buffer_reservation_live);

        bioseq::parallelism::release_admitted_vector_capacities_before_permit(
            first, second, *permit);
        REQUIRE(first.capacity() == 0u);
        REQUIRE(second.capacity() == 0u);
        REQUIRE(admission.reserved_bytes() == baseline);
    }
    REQUIRE(first_buffer_destroyed);
    REQUIRE(first_buffer_reservation_live);
    REQUIRE(second_buffer_destroyed);
    REQUIRE(second_buffer_reservation_live);
    REQUIRE(admission.reserved_bytes() == baseline);

    // Model the compaction hand-off used by the sorted-run store: old and next
    // capacities are simultaneously admitted, old capacity is destroyed, then
    // next transfers both allocation and permit without a reservation gap.
    /// Couples vector capacity with the admission permit that accounts for it.
    struct owned_u64_vector {
        bioseq::parallelism::memory_permit permit;
        std::vector<std::uint64_t> values;
    } old_runs, next_runs;
    const auto admit = [&admission](owned_u64_vector& owned, std::size_t units) {
        const std::uint64_t logical = units * sizeof(std::uint64_t);
        const std::uint64_t peak =
            logical * bioseq::parallelism::k_vector_preallocation_headroom_factor;
        auto permit = admission.try_reserve(peak);
        REQUIRE(permit.has_value());
        owned.permit = std::move(*permit);
        owned.values.reserve(units);
        const std::uint64_t allocated_bytes = owned.values.capacity() * sizeof(std::uint64_t);
        REQUIRE(allocated_bytes <= owned.permit.bytes());
        owned.permit.shrink_to(allocated_bytes);
        return allocated_bytes;
    };
    const std::uint64_t old_capacity_bytes = admit(old_runs, 8u);
    const std::uint64_t next_capacity_bytes = admit(next_runs, 4u);
    REQUIRE(admission.reserved_bytes() == baseline + old_capacity_bytes + next_capacity_bytes);
    bioseq::parallelism::release_admitted_vector_capacity_before_permit(old_runs.values,
                                                                        old_runs.permit);
    REQUIRE(admission.reserved_bytes() == baseline + next_capacity_bytes);
    // Production compaction/store transfer swaps the allocation before moving
    // its permit, so the source is capacity-free at hand-off.
    old_runs.values.swap(next_runs.values);
    old_runs.permit = std::move(next_runs.permit);
    REQUIRE(next_runs.values.capacity() == 0u);
    REQUIRE_FALSE(static_cast<bool>(next_runs.permit));
    REQUIRE(admission.reserved_bytes() == baseline + next_capacity_bytes);
    bioseq::parallelism::release_admitted_vector_capacity_before_permit(old_runs.values,
                                                                        old_runs.permit);
    REQUIRE(admission.reserved_bytes() == baseline);
}

TEST_CASE("memory materialization releases reservation after reflected ownership validation",
          "[component:bioseq][layer:unit][feature:scheduler]") {
    using bioseq::parallelism::memory_materialization_snapshot;
    bioseq::parallelism::memory_admission_controller admission;
    admission.set_index_memory_cap_bytes(4096u);
    REQUIRE(admission.reserved_bytes() == 0u);

    const auto run = [&admission](std::uint64_t before,
                                  std::uint64_t after,
                                  memory_materialization_snapshot snapshot) {
        set_materialization_observations(before, after);
        auto permit = admission.try_reserve(512u);
        REQUIRE(permit.has_value());
        const auto commit = admission.materialize(
            *permit,
            [snapshot]() noexcept { return snapshot; },
            deterministic_footprint_observation);
        return std::pair{std::move(*permit), commit};
    };

    const memory_materialization_snapshot reflected{
        .actual_allocation_bytes = 512u,
        .fully_touched_bytes = 512u,
        .is_host_visible = true,
        .is_host_coherent = true,
        .is_process_footprint_reflected = true,
    };
    {
        auto [permit, commit] = run(1000u, 1512u, reflected);
        REQUIRE(commit.is_permit_released);
        REQUIRE(commit.reflected_delta_bytes == 512u);
        REQUIRE(permit.bytes() == 0u);
        REQUIRE(admission.reserved_bytes() == 0u);
    }
    {
        auto [permit, commit] = run(2000u, 2511u, reflected);
        REQUIRE_FALSE(commit.is_permit_released);
        REQUIRE(permit.bytes() == 512u);
    }
    REQUIRE(admission.reserved_bytes() == 0u);
    {
        auto snapshot = reflected;
        snapshot.is_process_footprint_reflected = false;
        auto [permit, commit] = run(3000u, 3512u, snapshot);
        REQUIRE_FALSE(commit.is_permit_released);
    }
    {
        auto snapshot = reflected;
        snapshot.is_host_coherent = false;
        auto [permit, commit] = run(4000u, 4512u, snapshot);
        REQUIRE_FALSE(commit.is_permit_released);
    }
    {
        auto snapshot = reflected;
        snapshot.actual_allocation_bytes = 513u;
        snapshot.fully_touched_bytes = 513u;
        auto [permit, commit] = run(5000u, 5513u, snapshot);
        REQUIRE_FALSE(commit.is_permit_released);
    }
    {
        auto snapshot = reflected;
        snapshot.fully_touched_bytes = 511u;
        auto [permit, commit] = run(6000u, 6512u, snapshot);
        REQUIRE_FALSE(commit.is_permit_released);
    }
    {
        auto [permit, commit] = run(7000u, 6999u, reflected);
        REQUIRE_FALSE(commit.is_permit_released);
        REQUIRE(commit.reflected_delta_bytes == 0u);
    }
    REQUIRE(admission.reserved_bytes() == 0u);
    admission.set_index_memory_cap_bytes(0u);
}

TEST_CASE("memory materialization rejects a foreign controller before sampling or allocation",
          "[component:bioseq][layer:unit][feature:scheduler]") {
    bioseq::parallelism::memory_admission_controller owner;
    bioseq::parallelism::memory_admission_controller foreign;
    owner.set_index_memory_cap_bytes(512u);
    auto permit = owner.try_reserve(512u);
    REQUIRE(permit.has_value());
    REQUIRE(permit->controller() == std::addressof(owner));
    bool allocated = false;
    const auto materializer = [&allocated] {
        allocated = true;
        return bioseq::parallelism::memory_materialization_snapshot{};
    };
    REQUIRE_THROWS_AS(foreign.materialize(*permit, materializer, nullptr),
                      std::invalid_argument);
    REQUIRE_FALSE(allocated);
    REQUIRE(owner.reserved_bytes() == 512u);
    REQUIRE(foreign.reserved_bytes() == 0u);
    permit->release();
    REQUIRE(permit->controller() == nullptr);
    REQUIRE_THROWS_AS(owner.materialize(*permit, materializer, nullptr),
                      std::invalid_argument);
    REQUIRE_FALSE(allocated);
    REQUIRE(owner.reserved_bytes() == 0u);
}

TEST_CASE("memory materialization grow transfer, throw safety, and serialization",
          "[component:bioseq][layer:unit][feature:scheduler]") {
    using bioseq::parallelism::memory_materialization_snapshot;
    bioseq::parallelism::memory_admission_controller admission;
    admission.set_index_memory_cap_bytes(8192u);
    REQUIRE(admission.reserved_bytes() == 0u);

    set_materialization_observations(10'000u, 10'768u);
    auto grown = admission.try_reserve(512u);
    REQUIRE(grown.has_value());
    const auto grown_commit = admission.materialize(
        *grown,
        [&grown, &admission]() {
            REQUIRE(grown->grow_to(768u));
            REQUIRE(admission.reserved_bytes() == 768u);
            return memory_materialization_snapshot{
                .actual_allocation_bytes = 768u,
                .fully_touched_bytes = 768u,
                .is_host_visible = true,
                .is_host_coherent = true,
                .is_process_footprint_reflected = true,
            };
        },
        deterministic_footprint_observation);
    REQUIRE(grown_commit.is_permit_released);
    REQUIRE(grown->bytes() == 0u);
    REQUIRE(admission.reserved_bytes() == 0u);

    auto throwing = admission.try_reserve(256u);
    REQUIRE(throwing.has_value());
    REQUIRE_THROWS_AS(admission.materialize(
                          *throwing,
                          []() -> memory_materialization_snapshot {
                              throw std::runtime_error("forced materialization failure");
                          },
                          deterministic_footprint_observation),
                      std::runtime_error);
    REQUIRE(throwing->bytes() == 256u);
    REQUIRE(admission.reserved_bytes() == 256u);
    throwing->release();
    REQUIRE(admission.reserved_bytes() == 0u);

    std::atomic<unsigned> ready{0u};
    std::atomic<bool> go{false};
    std::atomic<unsigned> active{0u};
    std::atomic<unsigned> max_active{0u};
    std::atomic<bool> worker_failed{false};
    auto worker = [&admission, &ready, &go, &active, &max_active, &worker_failed]() {
        auto permit = admission.try_reserve(128u);
        if (!permit) {
            worker_failed.store(true, std::memory_order_release);
            ready.fetch_add(1u, std::memory_order_release);
            return;
        }
        ready.fetch_add(1u, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        (void)admission.materialize(
            *permit,
            [&active, &max_active]() noexcept {
                const unsigned now = active.fetch_add(1u, std::memory_order_acq_rel) + 1u;
                unsigned observed = max_active.load(std::memory_order_relaxed);
                while (observed < now && !max_active.compare_exchange_weak(
                                             observed, now, std::memory_order_relaxed)) {
                }
                for (unsigned spin = 0; spin < 100'000u; ++spin) {
                    std::atomic_signal_fence(std::memory_order_seq_cst);
                }
                active.fetch_sub(1u, std::memory_order_acq_rel);
                return memory_materialization_snapshot{
                    .actual_allocation_bytes = 128u,
                    .fully_touched_bytes = 128u,
                    .is_host_visible = true,
                    .is_host_coherent = true,
                    .is_process_footprint_reflected = false,
                };
            },
            deterministic_footprint_observation);
    };
    std::thread first{worker};
    std::thread second{worker};
    while (ready.load(std::memory_order_acquire) != 2u) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    first.join();
    second.join();
    REQUIRE_FALSE(worker_failed.load(std::memory_order_acquire));
    REQUIRE(max_active.load(std::memory_order_relaxed) == 1u);
    REQUIRE(admission.reserved_bytes() == 0u);
    admission.set_index_memory_cap_bytes(0u);
}

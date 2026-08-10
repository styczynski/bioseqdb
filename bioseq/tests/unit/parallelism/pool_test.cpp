#include <tests/support/common.hpp>

#include <atomic>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

import bioseq.common.context;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.pipeline_ops;
import bioseq.parallelism.memory_admission;

namespace {

std::array<std::uint64_t, 4> materialization_observations{};
std::atomic<std::size_t> materialization_observation_index{0u};

[[nodiscard]] std::uint64_t deterministic_footprint_observation() noexcept {
    const std::size_t index = materialization_observation_index.fetch_add(
        1u, std::memory_order_relaxed);
    return materialization_observations[
        std::min(index, materialization_observations.size() - 1u)];
}

void set_materialization_observations(std::uint64_t before,
                                      std::uint64_t after) noexcept {
  materialization_observations = {before, after, after, after};
  materialization_observation_index.store(0u, std::memory_order_relaxed);
}

enum class fake_tracked_state : std::uint8_t {
  unused = 0u,
  enrolled,
  scheduled,
  finished,
};

struct fake_tracked_slot_state {
  std::uint64_t generation = 0u;
  fake_tracked_state state = fake_tracked_state::unused;
  bioseq::parallelism::tracked_job_resources resources{};
  bool failed = false;
};

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

[[nodiscard]] bool
fake_tracked_enroll(void *argument,
                    bioseq::parallelism::tracked_job_resources resources,
                    bioseq::parallelism::tracked_job_handle *handle) noexcept {
  auto *owner = static_cast<fake_tracked_owner *>(argument);
  if (owner == nullptr || handle == nullptr) {
    return false;
  }
  for (std::size_t index = 0u; index < owner->slots.size(); ++index) {
    auto &slot = owner->slots[index];
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

[[nodiscard]] fake_tracked_slot_state *
fake_tracked_slot(fake_tracked_owner *owner,
                  bioseq::parallelism::tracked_job_handle handle) noexcept {
  if (owner == nullptr || handle.slot_index >= owner->slots.size()) {
    return nullptr;
  }
  auto &slot = owner->slots[handle.slot_index];
  return slot.generation == handle.generation ? std::addressof(slot) : nullptr;
}

[[nodiscard]] bool fake_tracked_mark_scheduled(
    void *argument, bioseq::parallelism::tracked_job_handle handle) noexcept {
  auto *slot =
      fake_tracked_slot(static_cast<fake_tracked_owner *>(argument), handle);
  if (slot == nullptr || slot->state != fake_tracked_state::enrolled) {
    return false;
  }
  slot->state = fake_tracked_state::scheduled;
  return true;
}

[[nodiscard]] bool fake_tracked_mark_finished(
    void *argument, bioseq::parallelism::tracked_job_handle handle) noexcept {
  auto *slot =
      fake_tracked_slot(static_cast<fake_tracked_owner *>(argument), handle);
  if (slot == nullptr || slot->state != fake_tracked_state::scheduled) {
    return false;
  }
  slot->state = fake_tracked_state::finished;
  return true;
}

[[nodiscard]] bool
fake_tracked_drain(void *argument,
                   bioseq::parallelism::tracked_job_handle handle,
                   bioseq::parallelism::tracked_job_outcome *outcome) noexcept {
  auto *owner = static_cast<fake_tracked_owner *>(argument);
  auto *slot = fake_tracked_slot(owner, handle);
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

void fake_tracked_handoff_error(void *argument,
                                bioseq::parallelism::tracked_job_handle handle,
                                std::uint32_t, char const *, bool) noexcept {
  auto *slot =
      fake_tracked_slot(static_cast<fake_tracked_owner *>(argument), handle);
  if (slot != nullptr) {
    slot->failed = true;
  }
}

void fake_tracked_worker_enter(
    void *argument, bioseq::parallelism::tracked_job_handle,
    bioseq::parallelism::tracked_worker_scope_storage *) noexcept {
  auto *owner = static_cast<fake_tracked_owner *>(argument);
  owner->worker_entries.fetch_add(1u, std::memory_order_relaxed);
  const std::uint32_t active =
      owner->active_workers.fetch_add(1u, std::memory_order_acq_rel) + 1u;
  std::uint32_t observed =
      owner->peak_active_workers.load(std::memory_order_relaxed);
  while (observed < active && !owner->peak_active_workers.compare_exchange_weak(
                                  observed, active, std::memory_order_relaxed,
                                  std::memory_order_relaxed)) {
  }
}

void fake_tracked_worker_leave(
    void *argument, bioseq::parallelism::tracked_job_handle,
    bioseq::parallelism::tracked_worker_scope_storage *) noexcept {
  auto *owner = static_cast<fake_tracked_owner *>(argument);
  owner->active_workers.fetch_sub(1u, std::memory_order_acq_rel);
}

[[nodiscard]] bioseq::parallelism::tracked_job_observer
make_fake_tracked_observer(fake_tracked_owner *owner) noexcept {
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

struct delayed_tracked_task {
  std::atomic<bool> *release = nullptr;
  std::atomic<std::uint32_t> *executions = nullptr;
};

void run_delayed_tracked_task(void *argument) {
  auto *task = static_cast<delayed_tracked_task *>(argument);
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
    pool.parallel_for(0, 8, 1, bioseq::parallelism::range_body{
        [&sum](std::size_t lo, std::size_t hi) {
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
    REQUIRE(pool.get_mode()
            == bioseq::parallelism::pipeline_concurrency_mode::task_parallel);
    pool.shutdown();
}

TEST_CASE("pool installs worker contexts",
          "[component:bioseq][layer:unit][feature:scheduler]") {
    auto& pool = bioseq::parallelism::global_pool::instance();
    pool.configure(bioseq::parallelism::pool_config{
        .maximum_concurrency = 2,
        .mode = bioseq::parallelism::pipeline_concurrency_mode::task_parallel,
    });

    std::atomic<std::uint64_t> seen = 0;
    std::atomic<bool> missing_context = false;
    pool.parallel_for_with_context(0, 16, 1, bioseq::parallelism::range_body{
        [&seen, &missing_context](std::size_t lo, std::size_t hi) {
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
        0, 1, 1,
        bioseq::parallelism::range_body{
            [installed, &inherited_context](std::size_t, std::size_t) {
              inherited_context.store(context::current_or_null() == installed,
                                      std::memory_order_relaxed);
            }});
    REQUIRE(inherited_context.load(std::memory_order_relaxed));

    pool.shutdown();
}

TEST_CASE(
    "tracked pool submissions are enrolled before later drain and reuse slots",
    "[component:bioseq][layer:unit][feature:scheduler]") {
  using namespace bioseq::parallelism;
  auto &pool = global_pool::instance();
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
      {std::addressof(task_contexts[0]), &run_delayed_tracked_task}, observer);
  const auto second_status = pool.submit_tracked_task_noexcept(
      std::addressof(submissions[1]),
      {std::addressof(task_contexts[1]), &run_delayed_tracked_task}, observer);
  REQUIRE(first_status == tracked_submit_status::submitted);
  REQUIRE(second_status == tracked_submit_status::submitted);
  CHECK(owner.live == 2u);
  CHECK(owner.peak_live == 2u);
  CHECK(owner.drains == 0u);

  release.store(true, std::memory_order_release);
  release.notify_all();
  for (auto &submission : submissions) {
    const auto outcome =
        pool.join_and_drain_tracked_task_noexcept(std::addressof(submission));
    CHECK(outcome.is_drained);
    CHECK_FALSE(outcome.is_incomplete);
    CHECK_FALSE(outcome.has_failure);
    CHECK(submission.backing_released == 1u);
  }
  CHECK(executions.load(std::memory_order_relaxed) == 2u);
  CHECK(owner.live == 0u);
  CHECK(owner.drains == 2u);

  delayed_tracked_task reusable{std::addressof(release),
                              std::addressof(executions)};
  tracked_task_submission reusable_submission{};
  for (std::uint32_t use = 0u; use < 128u; ++use) {
    const auto status = pool.submit_tracked_task_noexcept(
        std::addressof(reusable_submission),
        {std::addressof(reusable), &run_delayed_tracked_task}, observer);
    REQUIRE(status == tracked_submit_status::submitted);
    const auto outcome = pool.join_and_drain_tracked_task_noexcept(
        std::addressof(reusable_submission));
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
  auto &pool = bioseq::parallelism::global_pool::instance();
  pool.configure(bioseq::parallelism::pool_config{
      .maximum_concurrency = 2,
      .mode = bioseq::parallelism::pipeline_concurrency_mode::task_parallel,
  });

  std::atomic<std::uint64_t> outer_calls = 0;
  std::atomic<std::uint64_t> inner_calls = 0;
  bioseq::parallelism::parallel_for_pipeline(
      0, 8, 1, 1, bioseq::parallelism::task_parallel_region::query_batch,
      [&outer_calls, &inner_calls](std::size_t lo, std::size_t hi) {
        outer_calls.fetch_add(1, std::memory_order_relaxed);
        bioseq::parallelism::parallel_for_pipeline(
            lo, hi, 1, 1,
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
        REQUIRE(
            failure.reason
            == bioseq::parallelism::memory_admission_failure_reason::budget_exhausted);
        REQUIRE(failure.requested_bytes == 1024u);
        REQUIRE(failure.reserved_bytes == 128u);
        REQUIRE(failure.allocatable_after_reservations_bytes <= 896u);
        REQUIRE(failure.budget_ceiling_bytes >= failure.reserved_bytes);
        REQUIRE(failure.allocatable_after_reservations_bytes
                == failure.budget_ceiling_bytes - failure.reserved_bytes);
        REQUIRE((failure.system_available_bytes <= failure.system_total_bytes
                 || failure.system_total_bytes == 0u));
        REQUIRE((failure.process_soft_cap_bytes <= failure.system_total_bytes
                 || failure.system_total_bytes == 0u));
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
    struct restore_cap_guard {
        bioseq::parallelism::memory_admission_controller& admission;
        std::uint64_t previous = 0;
        ~restore_cap_guard() { admission.set_index_memory_cap_bytes(previous); }
    } restore_cap{admission, previous_cap};
    const std::uint64_t baseline = admission.reserved_bytes();
    admission.set_index_memory_cap_bytes(std::max<std::uint64_t>(
        previous_cap, baseline + 4096u));

    struct destruction_observer {
        bioseq::parallelism::memory_admission_controller* admission = nullptr;
        std::uint64_t expected_reserved = 0;
        bool* destroyed = nullptr;
        bool* reservation_was_live = nullptr;

        destruction_observer(
                bioseq::parallelism::memory_admission_controller* owner,
                std::uint64_t expected,
                bool* destroyed_flag,
                bool* live_flag) noexcept
            : admission(owner),
              expected_reserved(expected),
              destroyed(destroyed_flag),
              reservation_was_live(live_flag)
        {}

        ~destruction_observer() {
            *destroyed = true;
            *reservation_was_live =
                admission->reserved_bytes() == expected_reserved;
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
            &admission,
            baseline + 512u,
            &explicit_destroyed,
            &explicit_reservation_live);
        bioseq::parallelism::release_admitted_vector_capacity_before_permit(
            values, *permit);
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
        struct ordered_admitted_vector {
            bioseq::parallelism::memory_permit permit;
            std::vector<destruction_observer> values;
        } admitted{
            .permit = std::move(*permit),
            .values = {},
        };
        admitted.values.emplace_back(
            &admission,
            baseline + 512u,
            &member_destroyed,
            &member_reservation_live);
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
        const std::uint64_t logical_sum =
            2u * units_per_buffer * sizeof(destruction_observer);
        const std::uint64_t preallocation_peak =
            logical_sum
            * bioseq::parallelism::k_vector_preallocation_headroom_factor;
        auto permit = admission.try_reserve(preallocation_peak);
        REQUIRE(permit.has_value());
        REQUIRE(admission.reserved_bytes() == baseline + preallocation_peak);

        std::vector<destruction_observer> first;
        std::vector<destruction_observer> second;
        first.reserve(units_per_buffer);
        second.reserve(units_per_buffer);
        const std::uint64_t allocated_capacity_bytes =
            static_cast<std::uint64_t>(
                first.capacity() + second.capacity())
            * sizeof(destruction_observer);
        REQUIRE(allocated_capacity_bytes <= permit->bytes());
        permit->shrink_to(allocated_capacity_bytes);
        REQUIRE(admission.reserved_bytes()
                == baseline + allocated_capacity_bytes);
        first.emplace_back(
            &admission,
            baseline + allocated_capacity_bytes,
            &first_buffer_destroyed,
            &first_buffer_reservation_live);
        second.emplace_back(
            &admission,
            baseline + allocated_capacity_bytes,
            &second_buffer_destroyed,
            &second_buffer_reservation_live);

        bioseq::parallelism::
            release_admitted_vector_capacities_before_permit(
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
    struct owned_u64_vector {
        bioseq::parallelism::memory_permit permit;
        std::vector<std::uint64_t> values;
    } old_runs, next_runs;
    const auto admit = [&](owned_u64_vector& owned, std::size_t units) {
        const std::uint64_t logical = units * sizeof(std::uint64_t);
        const std::uint64_t peak = logical
            * bioseq::parallelism::k_vector_preallocation_headroom_factor;
        auto permit = admission.try_reserve(peak);
        REQUIRE(permit.has_value());
        owned.permit = std::move(*permit);
        owned.values.reserve(units);
        const std::uint64_t allocated_bytes =
            owned.values.capacity() * sizeof(std::uint64_t);
        REQUIRE(allocated_bytes <= owned.permit.bytes());
        owned.permit.shrink_to(allocated_bytes);
        return allocated_bytes;
    };
    const std::uint64_t old_capacity_bytes = admit(old_runs, 8u);
    const std::uint64_t next_capacity_bytes = admit(next_runs, 4u);
    REQUIRE(admission.reserved_bytes()
            == baseline + old_capacity_bytes + next_capacity_bytes);
    bioseq::parallelism::release_admitted_vector_capacity_before_permit(
        old_runs.values, old_runs.permit);
    REQUIRE(admission.reserved_bytes() == baseline + next_capacity_bytes);
    // Production compaction/store transfer swaps the allocation before moving
    // its permit, so the source is capacity-free at hand-off.
    old_runs.values.swap(next_runs.values);
    old_runs.permit = std::move(next_runs.permit);
    REQUIRE(next_runs.values.capacity() == 0u);
    REQUIRE_FALSE(static_cast<bool>(next_runs.permit));
    REQUIRE(admission.reserved_bytes() == baseline + next_capacity_bytes);
    bioseq::parallelism::release_admitted_vector_capacity_before_permit(
        old_runs.values, old_runs.permit);
    REQUIRE(admission.reserved_bytes() == baseline);
}

TEST_CASE("memory materialization releases reservation after reflected ownership validation",
          "[component:bioseq][layer:unit][feature:scheduler]") {
    using bioseq::parallelism::memory_materialization_snapshot;
    bioseq::parallelism::memory_admission_controller admission;
    admission.set_index_memory_cap_bytes(4096u);
    REQUIRE(admission.reserved_bytes() == 0u);

    const auto run = [&](std::uint64_t before,
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
        [&]() {
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
    REQUIRE_THROWS_AS(
        admission.materialize(
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
    auto worker = [&]() {
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
            [&]() noexcept {
                const unsigned now = active.fetch_add(
                    1u, std::memory_order_acq_rel) + 1u;
                unsigned observed = max_active.load(std::memory_order_relaxed);
                while (observed < now
                    && !max_active.compare_exchange_weak(
                        observed, now, std::memory_order_relaxed)) {}
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

#include "common_tests.h"
#include "common.h"

#include <atomic>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

import bioseq.common.context;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.pipeline_ops;
import bioseq.parallelism.memory_admission;

namespace {

std::array<std::uint64_t, 4> materialization_probe_values{};
std::atomic<std::size_t> materialization_probe_index{0u};

[[nodiscard]] std::uint64_t deterministic_footprint_probe() noexcept {
    const std::size_t index = materialization_probe_index.fetch_add(
        1u, std::memory_order_relaxed);
    return materialization_probe_values[
        std::min(index, materialization_probe_values.size() - 1u)];
}

void set_materialization_probe(
        std::uint64_t before,
        std::uint64_t after) noexcept {
    materialization_probe_values = {before, after, after, after};
    materialization_probe_index.store(0u, std::memory_order_relaxed);
}

} // namespace

TEST_CASE("DOC0294 pool lifecycle and range execution", "[parallelism][pool]") {
    auto& pool = bioseq::parallelism::GlobalPool::instance();
    pool.shutdown();
    REQUIRE(!pool.active());

    pool.configure(bioseq::parallelism::PoolConfig{
        .max_concurrency = 1,
        .mode = bioseq::parallelism::PipelineConcurrencyMode::SINGLE,
    });
    REQUIRE(pool.active());
    REQUIRE(pool.max_concurrency() == 1u);

    std::atomic<std::uint64_t> sum = 0;
    pool.parallel_for(0, 8, 1, bioseq::parallelism::range_body{
        [&sum](std::size_t lo, std::size_t hi) {
            for (std::size_t value = lo; value < hi; ++value) {
                sum.fetch_add(value, std::memory_order_relaxed);
            }
        }});
    REQUIRE(sum.load(std::memory_order_relaxed) == 28u);

    pool.restart(bioseq::parallelism::PoolConfig{
        .max_concurrency = 2,
        .mode = bioseq::parallelism::PipelineConcurrencyMode::TASK_PARALLEL,
    });
    REQUIRE(pool.max_concurrency() == 2u);
    REQUIRE(pool.mode() == bioseq::parallelism::PipelineConcurrencyMode::TASK_PARALLEL);
    pool.shutdown();
}

TEST_CASE("DOC0294 pool installs worker contexts", "[parallelism][context]") {
    auto& pool = bioseq::parallelism::GlobalPool::instance();
    pool.configure(bioseq::parallelism::PoolConfig{
        .max_concurrency = 2,
        .mode = bioseq::parallelism::PipelineConcurrencyMode::TASK_PARALLEL,
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
    pool.parallel_for_with_context(0, 1, 1, bioseq::parallelism::range_body{
        [installed, &inherited_context](std::size_t, std::size_t) {
            inherited_context.store(context::current_or_null() == installed, std::memory_order_relaxed);
        }});
    REQUIRE(inherited_context.load(std::memory_order_relaxed));

    pool.shutdown();
}

TEST_CASE("DOC0294 task-parallel nested scope gates inner regions", "[parallelism][task-scope]") {
    auto& pool = bioseq::parallelism::GlobalPool::instance();
    pool.configure(bioseq::parallelism::PoolConfig{
        .max_concurrency = 2,
        .mode = bioseq::parallelism::PipelineConcurrencyMode::TASK_PARALLEL,
    });

    std::atomic<std::uint64_t> outer_calls = 0;
    std::atomic<std::uint64_t> inner_calls = 0;
    bioseq::parallelism::parallel_for_pipeline(
        0, 8, 1, 1,
        bioseq::parallelism::TaskParallelRegion::query_batch,
        [&outer_calls, &inner_calls](std::size_t lo, std::size_t hi) {
            outer_calls.fetch_add(1, std::memory_order_relaxed);
            bioseq::parallelism::parallel_for_pipeline(
                lo, hi, 1, 1,
                bioseq::parallelism::TaskParallelRegion::align_regions,
                [&inner_calls](std::size_t, std::size_t) {
                    inner_calls.fetch_add(1, std::memory_order_relaxed);
                });
        });

    REQUIRE(outer_calls.load(std::memory_order_relaxed) >= 1u);
    REQUIRE(inner_calls.load(std::memory_order_relaxed) == outer_calls.load(std::memory_order_relaxed));
    pool.shutdown();
}

TEST_CASE("DOC0294 memory admission reserves and releases", "[parallelism][memory]") {
    auto& admission = bioseq::parallelism::MemoryAdmissionController::instance();
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

        bioseq::parallelism::MemoryAdmissionFailure failure{};
        auto denied = admission.try_reserve(1024, &failure);
        REQUIRE_FALSE(denied.has_value());
        REQUIRE(
            failure.reason
            == bioseq::parallelism::MemoryAdmissionFailureReason::budget_exhausted);
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
    REQUIRE(wave.admitted);
    REQUIRE(wave.fanout <= 2u);
    wave.permit.release();
    REQUIRE(admission.reserved_bytes() == 0u);
    admission.set_index_memory_cap_bytes(0);
}

TEST_CASE("admitted vector capacity is destroyed before its reservation is released",
          "[parallelism][memory][lifetime-order]") {
    auto& admission = bioseq::parallelism::MemoryAdmissionController::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    struct RestoreCap {
        bioseq::parallelism::MemoryAdmissionController& admission;
        std::uint64_t previous = 0;
        ~RestoreCap() { admission.set_index_memory_cap_bytes(previous); }
    } restore_cap{admission, previous_cap};
    const std::uint64_t baseline = admission.reserved_bytes();
    admission.set_index_memory_cap_bytes(std::max<std::uint64_t>(
        previous_cap, baseline + 4096u));

    struct DestructionProbe {
        bioseq::parallelism::MemoryAdmissionController* admission = nullptr;
        std::uint64_t expected_reserved = 0;
        bool* destroyed = nullptr;
        bool* reservation_was_live = nullptr;

        DestructionProbe(
                bioseq::parallelism::MemoryAdmissionController* owner,
                std::uint64_t expected,
                bool* destroyed_flag,
                bool* live_flag) noexcept
            : admission(owner),
              expected_reserved(expected),
              destroyed(destroyed_flag),
              reservation_was_live(live_flag)
        {}

        ~DestructionProbe() {
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
        std::vector<DestructionProbe> values;
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
        struct OrderedAdmittedVector {
            bioseq::parallelism::MemoryPermit permit;
            std::vector<DestructionProbe> values;
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
            2u * units_per_buffer * sizeof(DestructionProbe);
        const std::uint64_t preallocation_peak =
            logical_sum
            * bioseq::parallelism::kVectorPreallocationHeadroomFactor;
        auto permit = admission.try_reserve(preallocation_peak);
        REQUIRE(permit.has_value());
        REQUIRE(admission.reserved_bytes() == baseline + preallocation_peak);

        std::vector<DestructionProbe> first;
        std::vector<DestructionProbe> second;
        first.reserve(units_per_buffer);
        second.reserve(units_per_buffer);
        const std::uint64_t exact_capacity_bytes =
            static_cast<std::uint64_t>(
                first.capacity() + second.capacity())
            * sizeof(DestructionProbe);
        REQUIRE(exact_capacity_bytes <= permit->bytes());
        permit->shrink_to(exact_capacity_bytes);
        REQUIRE(admission.reserved_bytes()
                == baseline + exact_capacity_bytes);
        first.emplace_back(
            &admission,
            baseline + exact_capacity_bytes,
            &first_buffer_destroyed,
            &first_buffer_reservation_live);
        second.emplace_back(
            &admission,
            baseline + exact_capacity_bytes,
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
    struct OwnedU64Vector {
        bioseq::parallelism::MemoryPermit permit;
        std::vector<std::uint64_t> values;
    } old_runs, next_runs;
    const auto admit = [&](OwnedU64Vector& owned, std::size_t units) {
        const std::uint64_t logical = units * sizeof(std::uint64_t);
        const std::uint64_t peak = logical
            * bioseq::parallelism::kVectorPreallocationHeadroomFactor;
        auto permit = admission.try_reserve(peak);
        REQUIRE(permit.has_value());
        owned.permit = std::move(*permit);
        owned.values.reserve(units);
        const std::uint64_t exact =
            owned.values.capacity() * sizeof(std::uint64_t);
        REQUIRE(exact <= owned.permit.bytes());
        owned.permit.shrink_to(exact);
        return exact;
    };
    const std::uint64_t old_exact = admit(old_runs, 8u);
    const std::uint64_t next_exact = admit(next_runs, 4u);
    REQUIRE(admission.reserved_bytes()
            == baseline + old_exact + next_exact);
    bioseq::parallelism::release_admitted_vector_capacity_before_permit(
        old_runs.values, old_runs.permit);
    REQUIRE(admission.reserved_bytes() == baseline + next_exact);
    // Production compaction/store transfer swaps the allocation before moving
    // its permit, so the source is provably capacity-free at hand-off.
    old_runs.values.swap(next_runs.values);
    old_runs.permit = std::move(next_runs.permit);
    REQUIRE(next_runs.values.capacity() == 0u);
    REQUIRE_FALSE(static_cast<bool>(next_runs.permit));
    REQUIRE(admission.reserved_bytes() == baseline + next_exact);
    bioseq::parallelism::release_admitted_vector_capacity_before_permit(
        old_runs.values, old_runs.permit);
    REQUIRE(admission.reserved_bytes() == baseline);
}

TEST_CASE("memory materialization releases only on exact reflected ownership proof",
          "[parallelism][memory][materialization]") {
    using bioseq::parallelism::MemoryMaterializationEvidence;
    auto& admission = bioseq::parallelism::MemoryAdmissionController::instance();
    admission.set_index_memory_cap_bytes(4096u);
    REQUIRE(admission.reserved_bytes() == 0u);

    const auto run = [&](std::uint64_t before,
                         std::uint64_t after,
                         MemoryMaterializationEvidence evidence) {
        set_materialization_probe(before, after);
        auto permit = admission.try_reserve(512u);
        REQUIRE(permit.has_value());
        const auto commit = admission.materialize(
            *permit,
            [evidence]() noexcept { return evidence; },
            deterministic_footprint_probe);
        return std::pair{std::move(*permit), commit};
    };

    const MemoryMaterializationEvidence exact{
        .actual_allocation_bytes = 512u,
        .fully_touched_bytes = 512u,
        .host_visible = true,
        .host_coherent = true,
        .process_footprint_reflected = true,
    };
    {
        auto [permit, commit] = run(1000u, 1512u, exact);
        REQUIRE(commit.permit_released);
        REQUIRE(commit.reflected_delta_bytes == 512u);
        REQUIRE(permit.bytes() == 0u);
        REQUIRE(admission.reserved_bytes() == 0u);
    }
    {
        auto [permit, commit] = run(2000u, 2511u, exact);
        REQUIRE_FALSE(commit.permit_released);
        REQUIRE(permit.bytes() == 512u);
    }
    REQUIRE(admission.reserved_bytes() == 0u);
    {
        auto evidence = exact;
        evidence.process_footprint_reflected = false;
        auto [permit, commit] = run(3000u, 3512u, evidence);
        REQUIRE_FALSE(commit.permit_released);
    }
    {
        auto evidence = exact;
        evidence.host_coherent = false;
        auto [permit, commit] = run(4000u, 4512u, evidence);
        REQUIRE_FALSE(commit.permit_released);
    }
    {
        auto evidence = exact;
        evidence.actual_allocation_bytes = 513u;
        evidence.fully_touched_bytes = 513u;
        auto [permit, commit] = run(5000u, 5513u, evidence);
        REQUIRE_FALSE(commit.permit_released);
    }
    {
        auto evidence = exact;
        evidence.fully_touched_bytes = 511u;
        auto [permit, commit] = run(6000u, 6512u, evidence);
        REQUIRE_FALSE(commit.permit_released);
    }
    {
        auto [permit, commit] = run(7000u, 6999u, exact);
        REQUIRE_FALSE(commit.permit_released);
        REQUIRE(commit.reflected_delta_bytes == 0u);
    }
    REQUIRE(admission.reserved_bytes() == 0u);
    admission.set_index_memory_cap_bytes(0u);
}

TEST_CASE("memory materialization grow transfer, throw safety, and serialization",
          "[parallelism][memory][materialization]") {
    using bioseq::parallelism::MemoryMaterializationEvidence;
    auto& admission = bioseq::parallelism::MemoryAdmissionController::instance();
    admission.set_index_memory_cap_bytes(8192u);
    REQUIRE(admission.reserved_bytes() == 0u);

    set_materialization_probe(10'000u, 10'768u);
    auto grown = admission.try_reserve(512u);
    REQUIRE(grown.has_value());
    const auto grown_commit = admission.materialize(
        *grown,
        [&]() {
            REQUIRE(grown->grow_to(768u));
            REQUIRE(admission.reserved_bytes() == 768u);
            return MemoryMaterializationEvidence{
                .actual_allocation_bytes = 768u,
                .fully_touched_bytes = 768u,
                .host_visible = true,
                .host_coherent = true,
                .process_footprint_reflected = true,
            };
        },
        deterministic_footprint_probe);
    REQUIRE(grown_commit.permit_released);
    REQUIRE(grown->bytes() == 0u);
    REQUIRE(admission.reserved_bytes() == 0u);

    auto throwing = admission.try_reserve(256u);
    REQUIRE(throwing.has_value());
    REQUIRE_THROWS_AS(
        admission.materialize(
            *throwing,
            []() -> MemoryMaterializationEvidence {
                throw std::runtime_error("forced materialization failure");
            },
            deterministic_footprint_probe),
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
                return MemoryMaterializationEvidence{
                    .actual_allocation_bytes = 128u,
                    .fully_touched_bytes = 128u,
                    .host_visible = true,
                    .host_coherent = true,
                    .process_footprint_reflected = false,
                };
            },
            deterministic_footprint_probe);
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

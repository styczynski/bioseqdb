#include "common_tests.h"
#include "common.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

import bioseq.common.context;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.pipeline_ops;
import bioseq.parallelism.memory_admission;

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

#include "common_tests.h"
#include "common.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

import testing;
import context;
import bioseq_parallelism.mode;
import bioseq_parallelism.concepts;
import bioseq_parallelism.pool;
import bioseq_parallelism.pipeline_ops;
import bioseq_parallelism.memory_admission;

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
    pool.parallel_for_with_context(0, 16, 1, bioseq::parallelism::range_body{
        [&seen](std::size_t lo, std::size_t hi) {
            REQUIRE(context::current_or_null() != nullptr);
            for (std::size_t value = lo; value < hi; ++value) {
                seen.fetch_add(1, std::memory_order_relaxed);
            }
        }});
    REQUIRE(seen.load(std::memory_order_relaxed) == 16u);

    context::context outer_context;
    context::installed_context_scope install(outer_context);
    context::context* installed = context::current_or_null();
    pool.parallel_for_with_context(0, 1, 1, bioseq::parallelism::range_body{
        [installed](std::size_t, std::size_t) {
            REQUIRE(context::current_or_null() == installed);
        }});

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
    }
    REQUIRE(admission.reserved_bytes() == 0u);

    auto wave = admission.try_reserve_wave(4, 400);
    REQUIRE(wave.admitted);
    REQUIRE(wave.fanout <= 2u);
    wave.permit.release();
    REQUIRE(admission.reserved_bytes() == 0u);
    admission.set_index_memory_cap_bytes(0);
}

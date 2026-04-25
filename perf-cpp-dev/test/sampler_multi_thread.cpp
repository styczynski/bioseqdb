#include "access_benchmark.h"
#include <catch2/catch_test_macros.hpp>

#if defined(__x86_64__) || defined(__i386__)
#include <algorithm>
#include <cstdint>
#include <optional>
#include <perfcpp/sampler.hpp>
#include <thread>
#include <unistd.h>

namespace {

/// Spawns a thread that runs fn and stores any thrown exception into capture.
/// The caller must join the returned thread before reading capture.
template<typename F>
[[nodiscard]] std::thread
spawn_catching(std::exception_ptr& capture, F&& fn)
{
  return std::thread{ [&capture, fn = std::forward<F>(fn)]() {
    try {
      fn();
    } catch (...) {
      capture = std::current_exception();
    }
  } };
}
}

TEST_CASE("multi_thread_sampler_lifecycle", "[MultiThreadSampler]")
{
  /// Shared read-only benchmark: all threads access the same data without data races.
  auto benchmark = perf::test::AccessBenchmark{ /* is_random= */ true, 512U };

  SECTION("per-thread open, start, and stop execute without throwing")
  {
    auto sampler = perf::MultiThreadSampler{ 2U };
    REQUIRE_NOTHROW(sampler.trigger(std::string{ "cycles" }));
    sampler.values().logical_instruction_pointer(true);

    /// spawn_catching stores any thread exception for main-thread assertion after join.
    auto exceptions = std::array<std::exception_ptr, 2>{};
    auto t0 = spawn_catching(exceptions[0], [&]() {
      sampler.open(0U);
      sampler.start(0U);
      benchmark.run();
      sampler.stop(0U);
    });
    auto t1 = spawn_catching(exceptions[1], [&]() {
      sampler.open(1U);
      sampler.start(1U);
      benchmark.run();
      sampler.stop(1U);
    });
    t0.join();
    t1.join();

    for (const auto& ex : exceptions) {
      REQUIRE(ex == nullptr);
    }
    REQUIRE_NOTHROW(sampler.close());
  }

  SECTION("stop() without thread_id stops all threads from the main thread")
  {
    auto sampler = perf::MultiThreadSampler{ 2U };
    REQUIRE_NOTHROW(sampler.trigger(std::string{ "cycles" }));
    sampler.values().logical_instruction_pointer(true);

    /// Threads open and start, but deliberately do not stop themselves.
    auto exceptions = std::array<std::exception_ptr, 2>{};
    auto t0 = spawn_catching(exceptions[0], [&]() {
      sampler.open(0U);
      sampler.start(0U);
      benchmark.run();
    });
    auto t1 = spawn_catching(exceptions[1], [&]() {
      sampler.open(1U);
      sampler.start(1U);
      benchmark.run();
    });
    t0.join();
    t1.join();

    for (const auto& ex : exceptions) {
      REQUIRE(ex == nullptr);
    }

    /// stop() with no argument stops all per-thread samplers from the main thread.
    REQUIRE_NOTHROW(sampler.stop());

    const auto samples = sampler.result();
    REQUIRE_FALSE(samples.empty());

    REQUIRE_NOTHROW(sampler.close());
  }
}

TEST_CASE("multi_thread_sampling", "[MultiThreadSampler]")
{
  auto benchmark = perf::test::AccessBenchmark{ /* is_random= */ true, 256U };

  SECTION("IP sampling across 4 threads produces samples with instruction pointers")
  {
    auto sampler = perf::MultiThreadSampler{ 4U };
    REQUIRE_NOTHROW(sampler.trigger(std::string{ "cycles" }));
    sampler.values().logical_instruction_pointer(true);

    auto exceptions = std::vector<std::exception_ptr>(4U);
    auto threads = std::vector<std::thread>{};
    for (auto i = std::uint16_t{ 0U }; i < 4U; ++i) {
      threads.push_back(spawn_catching(exceptions[i], [&, i]() {
        sampler.open(i);
        sampler.start(i);
        benchmark.run();
        sampler.stop(i);
      }));
    }
    for (auto& t : threads) {
      t.join();
    }
    for (const auto& ex : exceptions) {
      REQUIRE(ex == nullptr);
    }

    const auto samples = sampler.result();
    REQUIRE_FALSE(samples.empty());
    for (const auto& sample : samples) {
      REQUIRE(sample.instruction_execution().logical_instruction_pointer().has_value());
    }

    REQUIRE_NOTHROW(sampler.close());
  }

  SECTION("result sorted by timestamp is monotonically non-decreasing")
  {
    auto sampler = perf::MultiThreadSampler{ 4U };
    REQUIRE_NOTHROW(sampler.trigger(std::string{ "cycles" }));
    sampler.values().logical_instruction_pointer(true).timestamp(true);

    auto exceptions = std::vector<std::exception_ptr>(4U);
    auto threads = std::vector<std::thread>{};
    for (auto i = std::uint16_t{ 0U }; i < 4U; ++i) {
      threads.push_back(spawn_catching(exceptions[i], [&, i]() {
        sampler.open(i);
        sampler.start(i);
        benchmark.run();
        sampler.stop(i);
      }));
    }
    for (auto& t : threads) {
      t.join();
    }
    for (const auto& ex : exceptions) {
      REQUIRE(ex == nullptr);
    }

    const auto samples = sampler.result(/* sort_by_time= */ true);
    REQUIRE_FALSE(samples.empty());

    auto last_timestamp = std::optional<std::uint64_t>{ std::nullopt };
    for (const auto& sample : samples) {
      REQUIRE(sample.metadata().timestamp().has_value());
      if (last_timestamp.has_value()) {
        CHECK(sample.metadata().timestamp().value() >= last_timestamp.value());
      }
      last_timestamp = sample.metadata().timestamp();
    }

    REQUIRE_NOTHROW(sampler.close());
  }

  SECTION("second measurement pass produces samples with later timestamps than the first")
  {
    auto sampler = perf::MultiThreadSampler{ 2U };
    REQUIRE_NOTHROW(sampler.trigger(std::string{ "cycles" }));
    sampler.values().logical_instruction_pointer(true).timestamp(true);

    /// First pass.
    auto ex0 = std::exception_ptr{};
    auto ex1 = std::exception_ptr{};
    auto thread0 = spawn_catching(ex0, [&]() {
      sampler.open(0U);
      sampler.start(0U);
      benchmark.run();
      sampler.stop(0U);
    });

    auto thread1 = spawn_catching(ex1, [&]() {
      sampler.open(1U);
      sampler.start(1U);
      benchmark.run();
      sampler.stop(1U);
    });

    thread0.join();
    thread1.join();
    REQUIRE(ex0 == nullptr);
    REQUIRE(ex1 == nullptr);

    const auto samples1 = sampler.result(/* sort_by_time= */ true);
    REQUIRE_FALSE(samples1.empty());
    REQUIRE(samples1.back().metadata().timestamp().has_value());
    const auto last_ts_pass1 = samples1.back().metadata().timestamp().value();

    /// Sampler::open() is a no-op while _is_opened is true; close() resets it.
    /// Without closing, new threads would open into an already-open fd still bound
    /// to the previous thread's TID, collecting no samples.
    REQUIRE_NOTHROW(sampler.close());

    /// Second pass: new threads open fresh fds bound to their own TIDs.
    auto ex2 = std::exception_ptr{};
    auto ex3 = std::exception_ptr{};
    auto thread2 = spawn_catching(ex2, [&]() {
      sampler.open(0U);
      sampler.start(0U);
      benchmark.run();
      sampler.stop(0U);
    });

    auto thread3 = spawn_catching(ex3, [&]() {
      sampler.open(1U);
      sampler.start(1U);
      benchmark.run();
      sampler.stop(1U);
    });

    thread2.join();
    thread3.join();
    REQUIRE(ex2 == nullptr);
    REQUIRE(ex3 == nullptr);

    const auto samples2 = sampler.result(/* sort_by_time= */ true);
    REQUIRE_FALSE(samples2.empty());
    REQUIRE(samples2.front().metadata().timestamp().has_value());
    /// close() above tears down the perf fds and resets the ring buffer, so the new
    /// threads open fresh fds. The time consumed by close() + thread spawning ensures
    /// even the earliest sample of pass 2 falls after the latest sample of pass 1.
    REQUIRE(samples2.front().metadata().timestamp().value() > last_ts_pass1);

    REQUIRE_NOTHROW(sampler.close());
  }

  SECTION("every thread's OS TID appears at least once in sampled metadata")
  {
    constexpr auto num_threads = std::uint16_t{ 4U };
    auto sampler = perf::MultiThreadSampler{ num_threads };
    REQUIRE_NOTHROW(sampler.trigger(std::string{ "cycles" }));
    sampler.values().logical_instruction_pointer(true).thread_id(true);

    /// Each thread stores its OS TID for post-join verification.
    auto tids = std::array<pid_t, num_threads>{};

    auto exceptions = std::vector<std::exception_ptr>(num_threads);
    auto threads = std::vector<std::thread>{};
    for (auto i = std::uint16_t{ 0U }; i < num_threads; ++i) {
      threads.push_back(spawn_catching(exceptions[i], [&, i]() {
        tids[i] = ::gettid();
        sampler.open(i);
        sampler.start(i);
        benchmark.run();
        sampler.stop(i);
      }));
    }
    for (auto& t : threads) {
      t.join();
    }
    for (const auto& ex : exceptions) {
      REQUIRE(ex == nullptr);
    }

    const auto samples = sampler.result();
    REQUIRE_FALSE(samples.empty());

    /// Verify each thread's TID appears in at least one sample's metadata.
    for (auto i = std::uint16_t{ 0U }; i < num_threads; ++i) {
      const auto tid = static_cast<std::uint32_t>(tids[i]);
      const auto has_sample = std::any_of(samples.begin(), samples.end(), [tid](const auto& sample) {
        return sample.metadata().thread_id().has_value() && sample.metadata().thread_id().value() == tid;
      });
      CHECK(has_sample);
    }

    REQUIRE_NOTHROW(sampler.close());
  }

  SECTION("active thread generates significantly more samples than idle thread")
  {
    auto sampler = perf::MultiThreadSampler{ 2U };
    REQUIRE_NOTHROW(sampler.trigger(std::string{ "cycles" }));
    sampler.values().logical_instruction_pointer(true).thread_id(true);

    auto tid_active = pid_t{ 0 };
    auto tid_idle = pid_t{ 0 };

    auto ex_active = std::exception_ptr{};
    auto ex_idle = std::exception_ptr{};

    /// Thread 0 runs the memory-access benchmark to produce heavy CPU activity.
    auto thread_active = spawn_catching(ex_active, [&]() {
      tid_active = ::gettid();
      sampler.open(0U);
      sampler.start(0U);
      benchmark.run();
      sampler.stop(0U);
    });

    /// Thread 1 starts and stops immediately without doing any CPU-intensive work.
    auto thread_idle = spawn_catching(ex_idle, [&]() {
      tid_idle = ::gettid();
      sampler.open(1U);
      sampler.start(1U);
      sampler.stop(1U);
    });

    thread_active.join();
    thread_idle.join();
    REQUIRE(ex_active == nullptr);
    REQUIRE(ex_idle == nullptr);

    const auto samples = sampler.result(/* sort_by_time= */ false);
    REQUIRE_FALSE(samples.empty());

    auto count_active = std::size_t{ 0U };
    auto count_idle = std::size_t{ 0U };
    const auto id_active = static_cast<std::uint32_t>(tid_active);
    const auto id_idle = static_cast<std::uint32_t>(tid_idle);

    for (const auto& sample : samples) {
      if (!sample.metadata().thread_id().has_value()) {
        continue;
      }
      const auto tid = sample.metadata().thread_id().value();
      if (tid == id_active) {
        ++count_active;
      } else if (tid == id_idle) {
        ++count_idle;
      }
    }

    REQUIRE(count_active >= 10U);
    REQUIRE(count_active > count_idle);
    REQUIRE_NOTHROW(sampler.close());
  }
}
#endif

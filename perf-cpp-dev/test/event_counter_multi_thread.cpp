#include "access_benchmark.h"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <perfcpp/event_counter.hpp>
#include <thread>
#include <vector>

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

} // namespace

TEST_CASE("multi_thread_event_counter_lifecycle", "[MultiThreadEventCounter]")
{
  SECTION("non-existing counter throws")
  {
    auto counter = perf::MultiThreadEventCounter{ 2U };
    REQUIRE_THROWS(counter.add("non-existing"));
  }

  SECTION("per-thread start and stop execute without throwing")
  {
    auto benchmark = perf::test::AccessBenchmark{ /* is_random= */ true, 256U };
    auto counter = perf::MultiThreadEventCounter{ 4U };
    REQUIRE_NOTHROW(counter.add(std::vector<std::string>{ "instructions", "cycles" }));

    auto exceptions = std::vector<std::exception_ptr>(4U);
    auto threads = std::vector<std::thread>{};
    for (auto i = std::uint16_t{ 0U }; i < 4U; ++i) {
      threads.push_back(spawn_catching(exceptions[i], [&, i]() {
        counter.start(i);
        benchmark.run();
        counter.stop(i);
      }));
    }
    for (auto& t : threads) {
      t.join();
    }
    for (const auto& ex : exceptions) {
      REQUIRE(ex == nullptr);
    }
    REQUIRE_NOTHROW(counter.close());
  }

  SECTION("stop without thread_id stops all threads")
  {
    auto benchmark = perf::test::AccessBenchmark{ /* is_random= */ true, 256U };
    auto counter = perf::MultiThreadEventCounter{ 2U };
    REQUIRE_NOTHROW(counter.add("instructions"));

    auto ex0 = std::exception_ptr{};
    auto ex1 = std::exception_ptr{};

    /// Threads start but deliberately do NOT stop themselves.
    auto t0 = spawn_catching(ex0, [&]() {
      counter.start(0U);
      benchmark.run();
    });
    auto t1 = spawn_catching(ex1, [&]() {
      counter.start(1U);
      benchmark.run();
    });
    t0.join();
    t1.join();
    REQUIRE(ex0 == nullptr);
    REQUIRE(ex1 == nullptr);

    /// stop() with no argument must stop all per-thread counters from the main thread.
    REQUIRE_NOTHROW(counter.stop());
    REQUIRE(counter.result().get("instructions").has_value());
    REQUIRE_NOTHROW(counter.close());
  }

  SECTION("close is idempotent")
  {
    auto benchmark = perf::test::AccessBenchmark{ /* is_random= */ true, 256U };
    auto counter = perf::MultiThreadEventCounter{ 2U };
    REQUIRE_NOTHROW(counter.add("instructions"));

    auto ex0 = std::exception_ptr{};
    auto ex1 = std::exception_ptr{};
    auto t0 = spawn_catching(ex0, [&]() {
      counter.start(0U);
      benchmark.run();
      counter.stop(0U);
    });
    auto t1 = spawn_catching(ex1, [&]() {
      counter.start(1U);
      benchmark.run();
      counter.stop(1U);
    });
    t0.join();
    t1.join();
    REQUIRE(ex0 == nullptr);
    REQUIRE(ex1 == nullptr);

    REQUIRE_NOTHROW(counter.close());
    REQUIRE_NOTHROW(counter.close()); /// Second close must not crash or throw.
  }
}

TEST_CASE("multi_thread_event_counter_counting", "[MultiThreadEventCounter]")
{
  /// Shared read-only benchmark: all threads access the same data without data races.
  auto benchmark = perf::test::AccessBenchmark{ /* is_random= */ true, 256U };

  SECTION("aggregated result contains expected counters")
  {
    auto counter = perf::MultiThreadEventCounter{ 4U };
    counter.add(std::vector<std::string>{ "instructions", "cycles" });

    auto exceptions = std::vector<std::exception_ptr>(4U);
    auto threads = std::vector<std::thread>{};
    for (auto i = std::uint16_t{ 0U }; i < 4U; ++i) {
      threads.push_back(spawn_catching(exceptions[i], [&, i]() {
        counter.start(i);
        benchmark.run();
        counter.stop(i);
      }));
    }
    for (auto& t : threads) {
      t.join();
    }
    for (const auto& ex : exceptions) {
      REQUIRE(ex == nullptr);
    }

    const auto result = counter.result();
    REQUIRE(result.get("instructions").has_value());
    REQUIRE(result.get("instructions").value() > 0.);
    REQUIRE(result.get("cycles").has_value());
    REQUIRE(result.get("cycles").value() > 0.);
  }

  SECTION("per-thread result contains expected counters")
  {
    auto counter = perf::MultiThreadEventCounter{ 4U };
    counter.add("instructions");

    auto exceptions = std::vector<std::exception_ptr>(4U);
    auto threads = std::vector<std::thread>{};
    for (auto i = std::uint16_t{ 0U }; i < 4U; ++i) {
      threads.push_back(spawn_catching(exceptions[i], [&, i]() {
        counter.start(i);
        benchmark.run();
        counter.stop(i);
      }));
    }
    for (auto& t : threads) {
      t.join();
    }
    for (const auto& ex : exceptions) {
      REQUIRE(ex == nullptr);
    }

    for (auto i = std::uint16_t{ 0U }; i < 4U; ++i) {
      const auto thread_result = counter.result_of_thread(i);
      REQUIRE(thread_result.get("instructions").has_value());
      REQUIRE(thread_result.get("instructions").value() > 0.);
    }
  }

  SECTION("aggregated result equals sum of per-thread results")
  {
    auto counter = perf::MultiThreadEventCounter{ 4U };
    counter.add("instructions");

    auto exceptions = std::vector<std::exception_ptr>(4U);
    auto threads = std::vector<std::thread>{};
    for (auto i = std::uint16_t{ 0U }; i < 4U; ++i) {
      threads.push_back(spawn_catching(exceptions[i], [&, i]() {
        counter.start(i);
        benchmark.run();
        counter.stop(i);
      }));
    }
    for (auto& t : threads) {
      t.join();
    }
    for (const auto& ex : exceptions) {
      REQUIRE(ex == nullptr);
    }

    const auto aggregated = counter.result().get("instructions");
    REQUIRE(aggregated.has_value());

    auto sum = double{ 0. };
    for (auto i = std::uint16_t{ 0U }; i < 4U; ++i) {
      const auto thread_val = counter.result_of_thread(i).get("instructions");
      REQUIRE(thread_val.has_value());
      sum += thread_val.value();
    }

    /// The aggregate must equal the sum of per-thread results (within 1% for scaling effects).
    REQUIRE(std::abs(aggregated.value() - sum) < sum * 0.01);
  }

  SECTION("active thread accumulates more instructions than idle thread")
  {
    auto counter = perf::MultiThreadEventCounter{ 2U };
    counter.add("instructions");

    auto ex_active = std::exception_ptr{};
    auto ex_idle = std::exception_ptr{};

    /// Thread 0 runs the full memory benchmark; thread 1 does no work.
    auto t_active = spawn_catching(ex_active, [&]() {
      counter.start(0U);
      benchmark.run();
      counter.stop(0U);
    });
    auto t_idle = spawn_catching(ex_idle, [&]() {
      counter.start(1U);
      counter.stop(1U);
    });

    t_active.join();
    t_idle.join();
    REQUIRE(ex_active == nullptr);
    REQUIRE(ex_idle == nullptr);

    const auto active_instructions = counter.result_of_thread(0U).get("instructions");
    const auto idle_instructions = counter.result_of_thread(1U).get("instructions");
    REQUIRE(active_instructions.has_value());
    REQUIRE(idle_instructions.has_value());

    /// Active thread must have orders of magnitude more instructions than the idle thread.
    REQUIRE(active_instructions.value() > idle_instructions.value() * 100.);
  }

  SECTION("restart after close produces consistent results")
  {
    auto counter = perf::MultiThreadEventCounter{ 2U };
    counter.add("instructions");

    /// First run.
    auto ex0 = std::exception_ptr{};
    auto ex1 = std::exception_ptr{};
    auto t0 = spawn_catching(ex0, [&]() {
      counter.start(0U);
      benchmark.run();
      counter.stop(0U);
    });
    auto t1 = spawn_catching(ex1, [&]() {
      counter.start(1U);
      benchmark.run();
      counter.stop(1U);
    });
    t0.join();
    t1.join();
    REQUIRE(ex0 == nullptr);
    REQUIRE(ex1 == nullptr);

    const auto result1 = counter.result().get("instructions");
    REQUIRE(result1.has_value());
    REQUIRE_NOTHROW(counter.close());

    /// Second run on the same object after close.
    auto ex2 = std::exception_ptr{};
    auto ex3 = std::exception_ptr{};
    auto t2 = spawn_catching(ex2, [&]() {
      counter.start(0U);
      benchmark.run();
      counter.stop(0U);
    });
    auto t3 = spawn_catching(ex3, [&]() {
      counter.start(1U);
      benchmark.run();
      counter.stop(1U);
    });
    t2.join();
    t3.join();
    REQUIRE(ex2 == nullptr);
    REQUIRE(ex3 == nullptr);

    const auto result2 = counter.result().get("instructions");
    REQUIRE(result2.has_value());

    /// Both runs on the same benchmark must produce instruction counts within 20% of each other.
    const auto max = std::max(result1.value(), result2.value());
    const auto min = std::min(result1.value(), result2.value());
    REQUIRE(min / max > 0.8);
  }
}

TEST_CASE("inherited_thread_event_counter", "[EventCounter]")
{
  auto benchmark = perf::test::AccessBenchmark{ /* is_random= */ true, 256U };

  SECTION("inherited counter captures child thread work")
  {
    auto config = perf::Config{};
    config.include_child_threads(true);
    auto counter = perf::EventCounter{ config };
    counter.add("instructions");

    /// Counter must be started before spawning so child threads are automatically inherited.
    counter.start();

    auto exceptions = std::vector<std::exception_ptr>(4U);
    auto threads = std::vector<std::thread>{};
    for (auto i = 0U; i < 4U; ++i) {
      threads.push_back(spawn_catching(exceptions[i], [&]() { benchmark.run(); }));
    }
    for (auto& t : threads) {
      t.join();
    }
    for (const auto& ex : exceptions) {
      REQUIRE(ex == nullptr);
    }

    counter.stop();

    const auto result = counter.result();
    REQUIRE(result.get("instructions").has_value());
    REQUIRE(result.get("instructions").value() > 1000000.);
  }

  SECTION("inherited counter reports more instructions than non-inherited counter")
  {
    /// Non-inherited run: only the main thread's instructions are counted.
    auto non_inherited_counter = perf::EventCounter{};
    non_inherited_counter.add("instructions");

    non_inherited_counter.start();
    {
      auto exceptions = std::vector<std::exception_ptr>(4U);
      auto threads = std::vector<std::thread>{};
      for (auto i = 0U; i < 4U; ++i) {
        threads.push_back(spawn_catching(exceptions[i], [&]() { benchmark.run(); }));
      }
      for (auto& t : threads) {
        t.join();
      }
      for (const auto& ex : exceptions) {
        REQUIRE(ex == nullptr);
      }
    }
    non_inherited_counter.stop();

    /// Inherited run: main thread + all four children are counted.
    auto config = perf::Config{};
    config.include_child_threads(true);
    auto inherited_counter = perf::EventCounter{ config };
    inherited_counter.add("instructions");

    inherited_counter.start();
    {
      auto exceptions = std::vector<std::exception_ptr>(4U);
      auto threads = std::vector<std::thread>{};
      for (auto i = 0U; i < 4U; ++i) {
        threads.push_back(spawn_catching(exceptions[i], [&]() { benchmark.run(); }));
      }
      for (auto& t : threads) {
        t.join();
      }
      for (const auto& ex : exceptions) {
        REQUIRE(ex == nullptr);
      }
    }
    inherited_counter.stop();

    const auto inherited = inherited_counter.result().get("instructions");
    const auto non_inherited = non_inherited_counter.result().get("instructions");
    REQUIRE(inherited.has_value());
    REQUIRE(non_inherited.has_value());

    /// The inherited counter must capture substantially more instructions (at least 2x)
    /// since it accounts for all four child threads while the non-inherited counter does not.
    REQUIRE(inherited.value() > non_inherited.value() * 2.);
  }
}

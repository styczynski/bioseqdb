#include "access_benchmark.h"
#include <catch2/catch_test_macros.hpp>
#include <perfcpp/event_counter.hpp>

TEST_CASE("configuration", "[EventCounter]")
{
  auto readonly_benchmark = perf::test::AccessBenchmark{ /* is random */ true, 1024U /* MB */ };

  SECTION("non-existing counter")
  {
    auto event_counter = perf::EventCounter{};

    REQUIRE_THROWS(event_counter.add("non-existing"));
  }

  SECTION("limited counters")
  {
    auto config = perf::Config{};
    config.num_events_per_physical_counter(1U);
    config.num_physical_counters(2U);
    auto event_counter = perf::EventCounter{ config };

    event_counter.add(std::vector<std::string>{ "instructions", "cycles" });

    event_counter.start();
    readonly_benchmark.run();
    event_counter.stop();

    REQUIRE(event_counter.result().get("cycles").has_value());
    REQUIRE(event_counter.result().get("instructions").has_value());
  }

  SECTION("too many counters")
  {
    auto config = perf::Config{};
    config.num_events_per_physical_counter(1U);
    config.num_physical_counters(2U);
    auto event_counter = perf::EventCounter{ config };

    REQUIRE_THROWS(event_counter.add({ "branches", "branch-misses", "cache-misses" }));
  }

  SECTION("empty counter")
  {
    auto event_counter = perf::EventCounter{};
    event_counter.start();
    readonly_benchmark.run();
    event_counter.stop();

    REQUIRE_FALSE(event_counter.result().get("instructions").has_value());
  }
}

TEST_CASE("counter scheduling", "[EventCounter]")
{
  auto readonly_benchmark = perf::test::AccessBenchmark{ /* is random */ true, 1024U /* MB */ };

  SECTION("same hardware counter")
  {
    auto event_counter = perf::EventCounter{};

    event_counter.add(std::vector<std::string>{ "instructions", "cycles" }, perf::EventCounter::Schedule::Group);

    event_counter.start();
    readonly_benchmark.run();
    event_counter.stop();

    REQUIRE(event_counter.result().get("cycles").has_value());
    REQUIRE(event_counter.result().get("instructions").has_value());
    REQUIRE(event_counter.result().get("instructions").value() > 100000000.);
    REQUIRE(event_counter.result().get("instructions").value() < 140000000.);
  }

  SECTION("separate hardware counter")
  {
    auto event_counter = perf::EventCounter{};

    event_counter.add(std::vector<std::string>{ "instructions", "cycles" }, perf::EventCounter::Schedule::Separate);

    event_counter.start();
    readonly_benchmark.run();
    event_counter.stop();

    REQUIRE(event_counter.result().get("cycles").has_value());
    REQUIRE(event_counter.result().get("instructions").has_value());
    REQUIRE(event_counter.result().get("instructions").value() > 100000000.);
    REQUIRE(event_counter.result().get("instructions").value() < 140000000.);
  }

  SECTION("counter with pmu")
  {
    auto event_counter = perf::EventCounter{};

    event_counter.add(std::vector<std::string>{ "cpu/cycles" }, perf::EventCounter::Schedule::Separate);

    event_counter.start();
    readonly_benchmark.run();
    event_counter.stop();

    REQUIRE(event_counter.result().get("cycles").has_value());
  }

  SECTION("counter with specific pmu")
  {
    // Add some non-existing "cycles" counter for a non-existing PMU; we will only add cpu/cycles.
    auto counter_definition = perf::CounterDefinition{};
    counter_definition.add("non-existing", "cycles", 10000U, 10000U);

    auto event_counter = perf::EventCounter{ counter_definition };

    event_counter.add(std::vector<std::string>{ "cpu/cycles" }, perf::EventCounter::Schedule::Separate);

    event_counter.start();
    readonly_benchmark.run();
    event_counter.stop();

    REQUIRE(event_counter.result().get("cycles").has_value());
  }
}

TEST_CASE("counting", "[EventCounter]")
{
  auto readonly_benchmark = perf::test::AccessBenchmark{ /* is random */ true, 1024U /* MB */ };

  SECTION("instructions only")
  {
    auto event_counter = perf::EventCounter{};
    event_counter.add("instructions");

    event_counter.start();
    readonly_benchmark.run();
    event_counter.stop();

    REQUIRE_FALSE(event_counter.result().get("cycles").has_value());
    REQUIRE(event_counter.result().get("instructions").has_value());
    REQUIRE(event_counter.result().get("instructions").value() > 100000000.);
    REQUIRE(event_counter.result().get("instructions").value() < 140000000.);
  }

  SECTION("re-open")
  {
    auto event_counter = perf::EventCounter{};
    event_counter.add("instructions");

    event_counter.start();
    readonly_benchmark.run();
    event_counter.stop();
    const auto result1 = event_counter.result();

    REQUIRE(result1.get("instructions").has_value());

    /// Restart the same counter without adding new events.
    event_counter.start();
    readonly_benchmark.run();
    event_counter.stop();
    const auto result2 = event_counter.result();

    REQUIRE(result2.get("instructions").has_value());

    const auto max_instructions = std::max(result1.get("instructions").value(), result2.get("instructions").value());
    const auto min_instructions = std::min(result1.get("instructions").value(), result2.get("instructions").value());
    REQUIRE((1. / max_instructions * min_instructions) < 1.1);
    REQUIRE((1. / max_instructions * min_instructions) > .9);
  }

  SECTION("re-open with close")
  {
    auto event_counter = perf::EventCounter{};
    event_counter.add("instructions");

    event_counter.start();
    readonly_benchmark.run();
    event_counter.stop();
    const auto result1 = event_counter.result();

    REQUIRE_FALSE(result1.get("cycles").has_value());
    REQUIRE(result1.get("instructions").has_value());

    /// Close before adding new events, then restart.
    event_counter.close();
    event_counter.add("cycles");
    event_counter.start();
    readonly_benchmark.run();
    event_counter.stop();
    const auto result2 = event_counter.result();

    REQUIRE(result2.get("cycles").has_value());
    REQUIRE(result2.get("instructions").has_value());

    const auto max_instructions = std::max(result1.get("instructions").value(), result2.get("instructions").value());
    const auto min_instructions = std::min(result1.get("instructions").value(), result2.get("instructions").value());
    REQUIRE((1. / max_instructions * min_instructions) < 1.1);
    REQUIRE((1. / max_instructions * min_instructions) > .9);
  }

  SECTION("add when opened")
  {
    /// Adding after start() (implicit open).
    {
      auto event_counter = perf::EventCounter{};
      event_counter.add("instructions");
      event_counter.start();
      REQUIRE_THROWS(event_counter.add("cycles"));
      event_counter.stop();
    }

    /// Adding after explicit open().
    {
      auto event_counter = perf::EventCounter{};
      event_counter.add("instructions");
      event_counter.open();
      REQUIRE_THROWS(event_counter.add("cycles"));
      event_counter.close();
    }
  }

  SECTION("instructions only")
  {
    auto event_counter = perf::EventCounter{};
    event_counter.add("instructions");

    event_counter.start();
    readonly_benchmark.run();
    event_counter.stop();

    REQUIRE_FALSE(event_counter.result().get("cycles").has_value());
    REQUIRE(event_counter.result().get("instructions").has_value());
    REQUIRE(event_counter.result().get("instructions").value() > 100000000.);
    REQUIRE(event_counter.result().get("instructions").value() < 140000000.);
  }

  SECTION("cache pattern increasing workload")
  {
    /// Create both benchmarks upfront so their pages are faulted in before any counter runs.
    auto readonly_sequential_benchmark = perf::test::AccessBenchmark{ /* is random */ false, 1024U /* MB */ };

    auto event_counter = perf::EventCounter{};
    event_counter.add({ "seconds", "instructions", "cycles", "cache-misses" });

    event_counter.start();
    readonly_benchmark.run();
    event_counter.stop();
    const auto random_result = event_counter.result();

    REQUIRE(random_result.get("seconds").has_value());
    REQUIRE(random_result.get("instructions").has_value());
    REQUIRE(random_result.get("cycles").has_value());
    REQUIRE(random_result.get("cache-misses").has_value());

    event_counter.start();
    readonly_sequential_benchmark.run();
    event_counter.stop();
    const auto sequential_result = event_counter.result();

    REQUIRE(sequential_result.get("seconds").has_value());
    REQUIRE(sequential_result.get("instructions").has_value());
    REQUIRE(sequential_result.get("cycles").has_value());
    REQUIRE(sequential_result.get("cache-misses").has_value());

    REQUIRE(random_result.get("cache-misses").value() > (sequential_result.get("cache-misses").value() * 2U));
    REQUIRE(random_result.get("cycles").value() > (sequential_result.get("cycles").value() * 2U));
    REQUIRE(random_result.get("seconds").value() > (sequential_result.get("seconds").value() * 2U));
  }

  SECTION("random access per cache line")
  {
    auto event_counter = perf::EventCounter{};
    event_counter.add({ "instructions", "cycles", "cache-misses", "branches", "dTLB-miss-ratio" });

    event_counter.start();
    readonly_benchmark.run();
    event_counter.stop();
    const auto random_result = event_counter.result(readonly_benchmark.size());

    REQUIRE(random_result.get("instructions").has_value());
    REQUIRE(random_result.get("instructions").value() > 6U);
    REQUIRE(random_result.get("instructions").value() < 9U);

    REQUIRE(random_result.get("cycles").has_value());
    REQUIRE(random_result.get("cycles").value() > 25U);
    REQUIRE(random_result.get("cycles").value() < 100U);

    REQUIRE(random_result.get("cache-misses").has_value());
    REQUIRE(random_result.get("cache-misses").value() > 1);
    REQUIRE(random_result.get("cache-misses").value() < 2);

    REQUIRE(random_result.get("branches").has_value());
    REQUIRE(random_result.get("branches").value() > .9);
    REQUIRE(random_result.get("branches").value() < 1.1);

    REQUIRE(random_result.get("dTLB-miss-ratio").has_value());
    REQUIRE(random_result.get("dTLB-miss-ratio").value() > .4);
    REQUIRE(random_result.get("dTLB-miss-ratio").value() < 1.1);
  }

  SECTION("time")
  {
    auto event_counter = perf::EventCounter{};
    event_counter.add(std::vector<std::string>{ "seconds", "milliseconds" });

    event_counter.start();
    readonly_benchmark.run();
    event_counter.stop();

    const auto result = event_counter.result();
    REQUIRE(result.get("seconds").has_value());
    REQUIRE(result.get("milliseconds").has_value());
    REQUIRE_FALSE(result.get("nanoseconds").has_value());

    REQUIRE((result.get("seconds").value() * 1100.) > (result.get("milliseconds").value()));
  }
}
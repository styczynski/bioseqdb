#include "../access_benchmark.h"
#include <iostream>
#include <perfcpp/event_counter.hpp>

int
main()
{
  std::cout
    << "libperf-cpp example: Record a events and live events where the latter can be read without stopping the "
       "hardware performance counters. As a benchmark, we use random access to an in-memory array multiple times."
    << std::endl;

  auto event_counter = perf::EventCounter{};

  try {
    /// Add counters that are recorded over the entire period (from start to end).
    // event_counter.add({ "cycles", "instructions", "cache-references", "cache-misses", "branches" });

    /// Add live counters that can be read without stopping the EventCounter.
    event_counter.add_live(std::vector<std::string>{ "cache-references", "cache-misses", "branches" });
  } catch (std::runtime_error& e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }

  /// Create random access benchmark.
  auto benchmark = perf::example::AccessBenchmark{ /*randomize the accesses*/ true,
                                                   /* create benchmark of 512 MB */ 512U };

  /// Access to live events. Needs to be initiated after adding all live events.
  auto live_events = perf::LiveEventCounter{ event_counter };

  /// Start recording.
  try {
    event_counter.start();
  } catch (std::runtime_error& exception) {
    std::cerr << exception.what() << std::endl;
    return 1;
  }

  /// Execute the benchmark (accessing cache lines in a random order).
  constexpr auto iterations = 20U;
  for (auto i = 0U; i < iterations; ++i) {
    /// Read current values of live events and mark them as "start" values.
    live_events.start();

    /// Perform benchmark.
    auto value = 0ULL;
    for (auto index = 0U; index < benchmark.size(); ++index) {
      value += benchmark[index].value;
    }

    /// We do not want the compiler to optimize away this (otherwise) unused value (and consequently the loop above).
    benchmark.pretend_to_use(value);

    /// Read the current counter value after the benchmark.
    live_events.stop();

    /// Print the live values.
    std::cout << "Live results: " << live_events.get("cache-references", benchmark.size()) << " cache-references, "
              << live_events.get("cache-misses", benchmark.size()) << " cache-misses, "
              << live_events.get("branches", benchmark.size()) << " branches" << std::endl;
  }

  /// Stop recording counters.
  event_counter.stop();

  return 0;
}
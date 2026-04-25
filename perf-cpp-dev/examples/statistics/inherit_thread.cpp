#include "../access_benchmark.h"
#include <iostream>
#include <numeric>
#include <perfcpp/event_counter.hpp>
#include <thread>

int
main()
{
  std::cout << "libperf-cpp example: Record performance counter for "
               "multi-threaded random access to an in-memory array."
            << std::endl;
  std::cout << "We will record the counters for all threads spawned by the main-thread." << std::endl;

  /// In this example, we will perform the benchmark multi-threaded and record
  /// all child-threads. If `include_child_threads` is not set to true, we would
  /// only record the main-thread.
  auto config = perf::Config{};
  config.include_child_threads(true);
  auto event_counter = perf::EventCounter{ config };

  /// Add all the performance counters we want to record.
  try {
    event_counter.add({ "instructions",
                        "cycles",
                        "branches",
                        "cache-misses",
                        "dTLB-miss-ratio",
                        "L1-data-miss-ratio",
                        "cycles-per-instruction" });
  } catch (std::runtime_error& e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }

  /// Create random access benchmark.
  auto benchmark = perf::example::AccessBenchmark{ /*randomize the accesses*/ true,
                                                   /* create benchmark of 1024 MB */ 1024U };

  /// One event_counter instance for every thread.
  const auto count_threads = 2U;
  const auto items_per_thread = benchmark.size() / count_threads;
  auto threads = std::vector<std::thread>{};
  auto thread_local_results = std::vector<std::uint64_t>(2U, 0U); /// Array to store the thread-local results.

  /// Start the performance counters. Note that the counters will also record
  /// the thread-creation.
  try {
    event_counter.start();
  } catch (std::runtime_error& exception) {
    std::cerr << exception.what() << std::endl;
    return 1;
  }

  for (auto thread_index = 0U; thread_index < count_threads; ++thread_index) {
    threads.emplace_back([thread_index, items_per_thread, &thread_local_results, &benchmark]() {
      auto local_value = 0ULL;

      /// Process the data.
      for (auto index = 0U; index < items_per_thread; ++index) {
        local_value += benchmark[(thread_index * items_per_thread) + index].value;
      }

      thread_local_results[thread_index] = local_value;
    });
  }

  /// Wait for all threads to finish.
  for (auto& thread : threads) {
    thread.join();
  }

  /// Stop recording counters.
  event_counter.stop();

  /// Add up the results so that the compiler does not get the idea of
  /// optimizing away the accesses.
  auto value = std::accumulate(thread_local_results.begin(), thread_local_results.end(), 0UL);

  /// We do not want the compiler to optimize away this (otherwise) unused value (and consequently the loop above).
  benchmark.pretend_to_use(value);

  /// Get the result (normalized per cache line).
  const auto result = event_counter.result(benchmark.size());

  /// Print the performance counters.
  std::cout << "\nResults:\n";
  for (const auto& [counter_name, counter_value] : result) {
    std::cout << counter_value << " " << counter_name << " / cache line" << std::endl;
  }

  return 0;
}
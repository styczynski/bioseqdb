#include "../access_benchmark.h"
#include <atomic>
#include <iostream>
#include <numeric>
#include <perfcpp/event_counter.hpp>
#include <thread>

int
main()
{
  std::cout << "libperf-cpp example: Record performance counter for "
               "random access to an in-memory array on all CPU cores."
            << std::endl;
  std::cout << "We will record the counters per (logical) CPU core and merge the results "
               "afterwards."
            << std::endl;

  /// Create a list of cpus to record performance counters on (all available, in this example).
  auto cpus_to_watch = std::vector<std::uint16_t>(std::min(std::thread::hardware_concurrency(), 16U));
  std::iota(cpus_to_watch.begin(), cpus_to_watch.end(), 0U);
  std::cout << "Creating counters for CPUs: ";
  for (auto cpu : cpus_to_watch) {
    std::cout << std::int32_t(cpu) << " ";
  }
  std::cout << std::endl;

  /// Initialize performance counters.
  auto multi_cpu_event_counter = perf::MultiCoreEventCounter{ std::move(cpus_to_watch) };

  /// Add all the performance counters we want to record.
  try {
    multi_cpu_event_counter.add({ "instructions",
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
  constexpr auto count_threads = 2U;
  const auto items_per_thread = benchmark.size() / count_threads;
  auto threads = std::vector<std::thread>{};
  auto thread_local_results = std::vector<std::uint64_t>(2U, 0U); /// Array to store the thread-local results.

  /// Barrier for the threads to wait.
  auto thread_barrier = std::atomic<bool>{ false };

  for (auto thread_index = 0U; thread_index < count_threads; ++thread_index) {
    threads.emplace_back([thread_index, items_per_thread, &thread_local_results, &benchmark, &thread_barrier]() {
      auto local_value = 0ULL;

      /// Wait for the barrier to become "true", i.e., all threads are spawned.
      while (!thread_barrier)
        ;

      /// Process the data.
      for (auto index = 0U; index < items_per_thread; ++index) {
        local_value += benchmark[(thread_index * items_per_thread) + index].value;
      }

      thread_local_results[thread_index] = local_value;
    });
  }

  /// Start recording performance counter.
  /// In contrast to the inherit-thread example (see inherit_thread.cpp), we
  /// will record the performance counters on each logical CPU core.
  try {
    multi_cpu_event_counter.start();
  } catch (std::runtime_error& exception) {
    std::cerr << exception.what() << std::endl;
    return 1;
  }

  /// Let threads start.
  thread_barrier = true;

  /// Wait for all threads to finish.
  for (auto& thread : threads) {
    thread.join();
  }

  /// Stop performance counter recording.
  multi_cpu_event_counter.stop();

  /// Add up the results so that the compiler does not get the idea of
  /// optimizing away the accesses.
  auto value = std::accumulate(thread_local_results.begin(), thread_local_results.end(), 0UL);

  /// We do not want the compiler to optimize away this (otherwise) unused value.
  benchmark.pretend_to_use(value);

  /// Get the result (normalized per cache line) from the
  /// multithread_event_counter.
  auto result = multi_cpu_event_counter.result(benchmark.size());

  /// Print the performance counters.
  std::cout << "\nResults:\n";
  for (const auto& [counter_name, counter_value] : result) {
    std::cout << counter_value << " " << counter_name << " / cache line" << std::endl;
  }

  return 0;
}

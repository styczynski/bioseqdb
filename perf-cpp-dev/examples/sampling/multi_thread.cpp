#include "../access_benchmark.h"
#include <iostream>
#include <numeric>
#include <perfcpp/sampler.hpp>
#include <thread>

int
main()
{
  std::cout << "libperf-cpp example: Record perf samples including time, "
               "instruction pointer, and cpu id for single-threaded random "
               "access to an in-memory array on multiple threads."
            << std::endl;

  constexpr auto count_threads = 4U;

  auto sampler = perf::MultiThreadSampler{ count_threads };

  /// Setup event that triggers writing samples.
  sampler.trigger(perf::Cycles{}, perf::Period{ 50000 });

  /// Setup what data the samples should include (timestamp, instruction pointer, CPU id, thread id).
  sampler.values().timestamp(true).logical_instruction_pointer(true).cpu_id(true).thread_id(true);

  /// Create random access benchmark.
  auto benchmark = perf::example::AccessBenchmark{ /*randomize the accesses*/ true,
                                                   /* create benchmark of 512 MB */ 1024U };

  /// Allocate space for threads and their results.
  const auto items_per_thread = benchmark.size() / count_threads;
  auto threads = std::vector<std::thread>{};
  auto thread_local_results =
    std::vector<std::uint64_t>(count_threads, 0U); /// Array to store the thread-local results.

  for (auto thread_index = std::uint16_t(0U); thread_index < count_threads; ++thread_index) {
    threads.emplace_back([thread_index, items_per_thread, &thread_local_results, &benchmark, &sampler]() {
      auto local_value = 0ULL;

      /// Start sampling per thread.
      try {
        sampler.start(thread_index);
      } catch (std::runtime_error& exception) {
        std::cerr << exception.what() << std::endl;
        return;
      }

      /// Process the data.
      for (auto index = 0U; index < items_per_thread; ++index) {
        local_value += benchmark[(thread_index * items_per_thread) + index].value;
      }

      /// Stop sampling on this thread.
      sampler.stop(thread_index);

      thread_local_results[thread_index] = local_value;
    });
  }

  /// Wait for all threads to finish.
  for (auto& thread : threads) {
    thread.join();
  }

  /// Add up the results so that the compiler does not get the idea of
  /// optimizing away the accesses.
  auto value = std::accumulate(thread_local_results.begin(), thread_local_results.end(), 0UL);

  /// We do not want the compiler to optimize away this (otherwise) unused value.
  benchmark.pretend_to_use(value);

  /// Get all the recorded samples – ordered by timestamp.
  auto samples = sampler.result(true);

  /// Print the first samples.
  const auto count_show_samples = std::min<std::size_t>(samples.size(), 40U);
  std::cout << "\nRecorded " << samples.size() << " samples." << std::endl;
  std::cout << "Here are the first " << count_show_samples << " recorded samples:\n" << std::endl;
  for (auto index = 0U; index < count_show_samples; ++index) {
    const auto& sample = samples[index];

    /// Since we recorded the time, period, the instruction pointer, and the CPU
    /// id, we can only read these values.
    if (sample.metadata().timestamp().has_value() && sample.metadata().thread_id().has_value() &&
        sample.instruction_execution().logical_instruction_pointer().has_value() &&
        sample.metadata().cpu_id().has_value()) {
      std::cout << "Time = " << sample.metadata().timestamp().value()
                << " | CPU ID = " << sample.metadata().cpu_id().value()
                << " | Thread ID = " << sample.metadata().thread_id().value() << " | Instruction Pointer = 0x"
                << std::hex << sample.instruction_execution().logical_instruction_pointer().value() << std::dec << "\n";
    }
  }
  std::cout << std::flush;

  /// Close the sampler.
  /// Note that the sampler can only be closed after reading the samples.
  sampler.close();

  return 0;
}
#include "../access_benchmark.h"
#include <iostream>
#include <perfcpp/sampler.hpp>

int
main()
{
  std::cout << "libperf-cpp example: Record perf samples including performance "
               "counters for single-threaded random access to an in-memory array."
            << std::endl;

  /// Initialize counter definitions.
  /// Note that the perf::CounterDefinition holds all counter names and must be
  /// alive until the benchmark finishes.
  auto counter_definition = perf::CounterDefinition{};

  /// Add metric that calculates the L1d miss ratio.
  counter_definition.add("L1d-misses-per-load", "'L1-dcache-load-misses'/'L1-dcache-loads'");

  auto sampler = perf::Sampler{ counter_definition };

  /// Setup the event that will trigger writing samples.
  sampler.trigger(perf::Cycles{}, perf::Precision::AllowArbitrarySkid, perf::Period{ 50000 });

  /// Setup which data should be included (L1 hit and miss counter, timestamp).
  sampler.values().counter({ "L1-dcache-loads", "L1-dcache-load-misses", "L1d-misses-per-load" }).timestamp(true);

  /// Create random access benchmark.
  auto benchmark = perf::example::AccessBenchmark{ /*randomize the accesses*/ true,
                                                   /* create benchmark of 512 MB */ 512U };

  /// Start sampling.
  try {
    sampler.start();
  } catch (std::runtime_error& exception) {
    std::cerr << exception.what() << std::endl;
    return 1;
  }

  /// Execute the benchmark (accessing cache lines in a random order).
  auto value = 0ULL;
  for (auto index = 0U; index < benchmark.size(); ++index) {
    value += benchmark[index].value;
  }

  /// We do not want the compiler to optimize away this (otherwise) unused value (and consequently the loop above).
  benchmark.pretend_to_use(value);

  /// Stop sampling.
  sampler.stop();

  /// Get all the recorded samples.
  const auto samples = sampler.result();
  samples.to_csv("samples_performance_counters.csv");
  std::cout << "Wrote samples to `samples_performance_counters.csv`." << std::endl;

  /// Print the first samples.
  const auto count_show_samples = std::min<std::size_t>(samples.size(), 40U);
  std::cout << "\nRecorded " << samples.size() << " samples." << std::endl;
  std::cout << "Here are the first " << count_show_samples << " recorded samples:\n" << std::endl;

  std::optional<perf::CounterResult> last_counter_result = std::nullopt; /// Remember the last counter result to show
                                                                         /// only the difference.

  for (auto index = 0U; index < count_show_samples; ++index) {
    const auto& sample = samples[index];

    /// Since we recorded the time, period, the instruction pointer, and the CPU
    /// id, we can only read these values.
    if (sample.metadata().timestamp().has_value() && sample.counter().has_value()) {
      if (last_counter_result.has_value()) {
        std::cout << "Time = " << sample.metadata().timestamp().value() << " | cycles (diff) = "
                  << sample.counter()->get("cycles").value_or(.0) - last_counter_result->get("cycles").value_or(.0)
                  << " | L1-dcache-loads (diff) = "
                  << sample.counter()->get("L1-dcache-loads").value_or(.0) -
                       last_counter_result->get("L1-dcache-loads").value_or(.0)
                  << " | L1-dcache-load-misses (diff) = "
                  << sample.counter()->get("L1-dcache-load-misses").value_or(.0) -
                       last_counter_result->get("L1-dcache-load-misses").value_or(.0)
                  << " | L1d-misses-per-load = " << sample.counter()->get("L1d-misses-per-load").value_or(.0) << "\n";
      }

      last_counter_result = sample.counter();
    }
  }
  std::cout << std::flush;

  /// Close the sampler.
  /// Note that the sampler can only be closed after reading the samples.
  sampler.close();

  return 0;
}
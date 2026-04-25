#include "../access_benchmark.h"
#include <iostream>
#include <perfcpp/sampler.hpp>

int
main()
{
  std::cout << "libperf-cpp example: Record perf samples including time, "
               "instruction pointer, and callchain for flamegraph generation."
            << std::endl;

  auto sampler = perf::Sampler{};

  /// Event that generates an overflow which is samples.
  sampler.trigger(perf::Cycles{}, perf::Precision::RequestZeroSkid, perf::Period{ 50000U });

  /// Include Timestamp, period, instruction pointer, and CPU number into samples.
  sampler.values().timestamp(true).logical_instruction_pointer(true).callchain(true);

  /// Start sampling.
  try {
    sampler.start();
  } catch (std::runtime_error& exception) {
    std::cerr << exception.what() << std::endl;
    return 1;
  }

  /// Create random access benchmark.
  auto benchmark = perf::example::AccessBenchmark{ /*randomize the accesses*/ true,
                                                   /* create benchmark of 512 MB */ 512U };

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
  const auto samples = sampler.result(true);

  /// Translate into frame graph entries.
  samples.to_flamegraphs("flamegraphs.txt");
  std::cout << "Wrote samples into flamegraphs.txt" << std::endl;
  std::cout << "You can upload the flamegraphs.txt here: https://flamegraph.com/" << std::endl;

  /// Close the sampler.
  /// Note that the sampler can only be closed after reading the samples.
  sampler.close();

  return 0;
}
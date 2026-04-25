#include "../access_benchmark.h"
#include <iostream>
#include <perfcpp/analyzer/memory_access.hpp>
#include <perfcpp/hardware_info.hpp>
#include <perfcpp/sampler.hpp>

int
main()
{
  std::cout << "libperf-cpp example: Sample memory addresses and analyze data objects." << std::endl;

  /// Initialize sampler.
  auto sampler = perf::Sampler{};

  /// Setup which counters trigger the writing of samples (depends on the underlying hardware substrate).
  if (perf::HardwareInfo::is_amd_ibs_supported()) {
    sampler.trigger(perf::IbsOp{ /*is_uop=*/true }, perf::Precision::MustHaveZeroSkid, perf::Period{ 4000U });
  } else if (perf::HardwareInfo::is_intel()) {
    sampler.trigger(
      perf::MemoryLoads{ /*no latency filter*/ 0U }, perf::Precision::MustHaveZeroSkid, perf::Period{ 2000U });
  } else {
    std::cout << "Error: Memory sampling is not supported on this CPU." << std::endl;
    return 1;
  }

  /// Setup which data will be included into samples (timestamp, virtual memory address, data source like L1d or RAM,
  /// and latency).
  sampler.values().logical_memory_address(true).data_source(true).data_access_latency(true).instruction_latency(true);
  if (perf::HardwareInfo::is_amd()) {
    sampler.values().data_tlb_latency(true).mhb_allocations(true);
  }

  /// Create random access benchmark.
  auto benchmark = perf::example::AccessBenchmark{ /*randomize the accesses*/ true,
                                                   /* create benchmark of 2 GB */ 2048 };

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

  /// Create data types for analyzer.
  auto data_analyzer = perf::analyzer::MemoryAccess{};

  /// 1) Create and add the "index" data type (normal u64 that dictates the pattern through the data array in the random
  /// access benchmark).
  auto index = perf::analyzer::DataType{ "index", sizeof(std::uint64_t) };
  index.add<std::uint64_t>("index");
  data_analyzer.add(std::move(index));

  /// 2) Create and add the "data_cache_line" data type (single cache line that is accessed in the random access
  /// benchmark).
  auto cache_line = perf::analyzer::DataType{ "data_cache_line", sizeof(perf::example::AccessBenchmark::cache_line) };
  cache_line.add<std::uint64_t>("value");
  data_analyzer.add(std::move(cache_line));

  /// 3) Register instances in memory for both data types.
  data_analyzer.annotate("index", benchmark.indices());
  data_analyzer.annotate("data_cache_line", benchmark.data_to_read());

  /// 4) Get all the recorded samples.
  const auto samples = sampler.result();

  /// 5) Map the samples to data type instances.
  const auto result = data_analyzer.map(samples);

  /// 6) Print the results to the console.
  std::cout << result.to_string() << std::flush;

  return 0;
}
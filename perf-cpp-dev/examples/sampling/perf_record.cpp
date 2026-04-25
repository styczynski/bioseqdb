#include "../access_benchmark.h"
#include <iostream>
#include <perfcpp/hardware_info.hpp>
#include <perfcpp/sampler.hpp>

int
main()
{
  std::cout
    << "libperf-cpp example: Record perf samples including time, "
       "logical memory address, latency, data source, and instruction and write as a perf data file `perf.dat`. "
    << std::endl;

  /// Initialize sampler.
  auto sampler = perf::Sampler{};

  /// Setup which counters trigger the writing of samples (depends on the underlying hardware substrate).
  if (perf::HardwareInfo::is_amd_ibs_supported()) {
    sampler.trigger(perf::IbsOp{ /*is_uop=*/true }, perf::Precision::MustHaveZeroSkid, perf::Period{ 16000 });
  } else if (perf::HardwareInfo::is_intel()) {
    sampler.trigger(
      perf::MemoryLoads{ /*no latency filter*/ 0U }, perf::Precision::MustHaveZeroSkid, perf::Period{ 16000 });
  } else {
    std::cout << "Error: Memory sampling is not supported on this CPU." << std::endl;
    return 1;
  }

  /// Setup which data will be included into samples (timestamp, virtual memory address, data source like L1d or RAM,
  /// latency, instruction address, thread id, and the callstack).
  sampler.values()
    .timestamp(true)
    .logical_memory_address(true)
    .data_source(true)
    .data_access_latency(true)
    .logical_instruction_pointer(true)
    .thread_id(true)
    .callchain(true);

  /// Start sampling.
  try {
    sampler.start();
  } catch (std::runtime_error& exception) {
    std::cerr << exception.what() << std::endl;
    return 1;
  }

  /// Create random access benchmark.
  auto benchmark = perf::example::AccessBenchmark{ /*randomize the accesses*/ true,
                                                   /* create benchmark of 1024 MB */ 1024U };

  /// Execute the benchmark (accessing cache lines in a random order).
  auto value = 0ULL;
  for (auto index = 0U; index < benchmark.size(); ++index) {
    value += benchmark[index].value;
  }

  /// We do not want the compiler to optimize away this (otherwise) unused value (and consequently the loop above).
  benchmark.pretend_to_use(value);

  /// Stop sampling.
  sampler.stop();

  /// Write sample results to perf file.
  sampler.to_perf_file("perf.data");

  std::cout << "Wrote " << sampler.result().size() << " samples to `perf.data`."
            << "\n    Run `perf report`     to show overhead per symbol"
            << "\n    Run `perf mem report` to show overhead per data object" << std::endl;

  /// Close the sampler.
  /// Note that the sampler can only be closed after reading the samples.
  sampler.close();

  return 0;
}
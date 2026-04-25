#include "../access_benchmark.h"
#include <iostream>
#include <perfcpp/hardware_info.hpp>
#include <perfcpp/sampler.hpp>

int
main()
{
  std::cout << "libperf-cpp example: Record perf samples including time, "
               "logical memory address, latency, and data source for "
               "single-threaded random access to an in-memory array."
            << std::endl;

  /// Initialize sampler.
  auto sampler = perf::Sampler{};

  /// Setup which counters trigger the writing of samples (depends on the underlying hardware substrate).
  if (perf::HardwareInfo::is_amd_ibs_supported()) {
    sampler.trigger(perf::IbsOp{ /*is_uop=*/true }, perf::Precision::MustHaveZeroSkid, perf::Period{ 4000U });
  } else if (perf::HardwareInfo::is_intel()) {
    sampler.trigger(
      perf::MemoryLoads{ /*no latency filter*/ 0U }, perf::Precision::MustHaveZeroSkid, perf::Period{ 4000U });
  } else {
    std::cout << "Error: Memory sampling is not supported on this CPU." << std::endl;
    return 1;
  }

  /// Setup which data will be included into samples (timestamp, virtual memory address, data source like L1d or RAM,
  /// and latency).
  sampler.values()
    .timestamp(true)
    .logical_memory_address(true)
    .data_source(true)
    .data_access_latency(true)
    .instruction_latency(true);

  /// Create random access benchmark.
  auto benchmark = perf::example::AccessBenchmark{ /*randomize the accesses*/ true,
                                                   /* create benchmark of 1024 MB */ 1024U };

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
  auto samples = sampler.result();

  samples.to_csv("samples_memory_address.csv");
  std::cout << "Wrote samples to `samples_memory_address.csv`." << std::endl;

  const auto count_samples_before_filter = samples.size();

  /// Filter out samples without data source (AMD samples all instructions, not only data-related).
  samples.filter([](const auto& sample) {
    return !sample.count_loss().has_value() && sample.data_access().source().has_value() &&
           sample.data_access().logical_memory_address().value_or(0UL) != 0UL;
  });

  /// Print the first samples.
  const auto count_show_samples = std::min<std::size_t>(samples.size(), 40U);
  std::cout << "\nRecorded " << count_samples_before_filter << " samples. " << samples.size()
            << " remaining after filter." << std::endl;
  std::cout << "Here are the first " << count_show_samples << " recorded samples:\n" << std::endl;
  for (auto index = 0U; index < count_show_samples; ++index) {
    const auto& sample = samples[index];

    /// Since we recorded the time, period, the instruction pointer, and the CPU
    /// id, we can only read these values.
    auto data_source = "N/A";
    if (sample.data_access().source()->is_l1_hit()) {
      data_source = "L1d";
    } else if (sample.data_access().source()->is_mhb_hit().value_or(false)) {
      data_source = "LFB/MAB";
    } else if (sample.data_access().source()->is_l2_hit()) {
      data_source = "L2";
    } else if (sample.data_access().source()->is_l3_hit()) {
      data_source = "L3";
    } else if (sample.data_access().source()->is_memory_hit()) {
      data_source = "RAM";
    }

    auto instruction_latency = 0ULL;
    auto cache_latency = 0ULL;

    if (perf::HardwareInfo::is_intel()) {
      instruction_latency = sample.instruction_execution().latency().instruction_retirement().value_or(0U);
      cache_latency = sample.data_access().latency().cache_access().value_or(0U);
    } else if (perf::HardwareInfo::is_amd()) {
      instruction_latency = sample.instruction_execution().latency().uop_tag_to_retirement().value_or(0U);
      cache_latency = sample.data_access().latency().cache_miss().value_or(0U);
    }

    std::cout << "Time = " << sample.metadata().timestamp().value_or(0U) << " | Logical Mem Address = 0x" << std::hex
              << sample.data_access().logical_memory_address().value() << std::dec
              << " | Latency (cache, instruction) = " << cache_latency << ", " << instruction_latency
              << " | Is Load = " << sample.data_access().is_load() << " | Data Source = " << data_source << "\n";
  }
  std::cout << std::flush;

  /// Close the sampler.
  /// Note that the sampler can only be closed after reading the samples.
  sampler.close();

  return 0;
}
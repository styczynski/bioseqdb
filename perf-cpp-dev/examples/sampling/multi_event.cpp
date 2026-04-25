#include "../access_benchmark.h"
#include <iostream>
#include <perfcpp/hardware_info.hpp>
#include <perfcpp/sampler.hpp>

int
main()
{
  std::cout << "libperf-cpp example: Record perf samples including time, "
               "logical memory address, latency, and data source for "
               "single-threaded random access to an in-memory array "
               "using multiple events as trigger."
            << std::endl;

  /// Initialize sampler.
  auto sampler = perf::Sampler{};

  if (perf::HardwareInfo::is_intel()) {
    sampler.trigger(std::vector<std::vector<perf::Sampler::Trigger>>{
      { perf::Sampler::Trigger{ perf::MemoryLoads{ /*no latency filter*/ 0U },
                                perf::Precision::RequestZeroSkid,
                                perf::Period{ 8000U } } }, /// Loads
      { perf::Sampler::Trigger{
        perf::MemoryStores{}, perf::Precision::MustHaveZeroSkid, perf::Period{ 8000U } } } /// Stores
    });
  } else {
    std::cout << "Error: Memory sampling with multiple triggers is not supported on this CPU." << std::endl;
    return 1;
  }

  /// Define what to sample.
  sampler.values()
    .timestamp(true)
    .logical_memory_address(true)
    .data_source(true)
    .data_access_latency(true)
    .instruction_type(true);

  /// Create random access benchmark.
  auto benchmark = perf::example::AccessBenchmark{ /*randomize the accesses*/ true,
                                                   /* create benchmark of 512 MB */ 512U,
                                                   /* also support writing */ true };

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

    /// Also write a value to get store events.
    benchmark.set(index, value);
  }

  /// We do not want the compiler to optimize away this (otherwise) unused value.
  benchmark.pretend_to_use(value);

  /// Stop sampling.
  sampler.stop();

  /// Get all the recorded samples.
  auto samples = sampler.result(/* sort by time */ true);
  const auto count_samples_before_filter = samples.size();

  /// Print the first samples.
  const auto count_show_samples = std::min<std::size_t>(samples.size(), 40U);
  std::cout << "\nRecorded " << count_samples_before_filter << " samples. " << samples.size()
            << " remaining after filter." << std::endl;
  std::cout << "Here are the first " << count_show_samples << " recorded samples:\n" << std::endl;
  for (auto index = 0U; index < count_show_samples; ++index) {
    const auto& sample = samples[index];

    /// Since we recorded the time, period, the instruction pointer, and the CPU
    /// id, we can only read these values.
    if (sample.metadata().timestamp().has_value() && sample.data_access().logical_memory_address().has_value()) {
      auto* data_source = "N/A";

      if (sample.data_access().source().has_value()) {
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
      }

      auto* type = "N/A";
      if (sample.instruction_execution().type().has_value()) {
        if (sample.data_access().is_load()) {
          type = "Load";
        } else if (sample.data_access().is_store()) {
          type = "Store";
        }
      }

      const auto instruction_latency = sample.instruction_execution().latency().instruction_retirement().value_or(
        sample.instruction_execution().latency().uop_tag_to_retirement().value_or(0U));
      const auto cache_latency = sample.data_access().latency().cache_access().value_or(
        sample.data_access().latency().cache_miss().value_or(0U));

      std::cout << "Time = " << sample.metadata().timestamp().value() << " | Logical Mem Address = 0x" << std::hex
                << sample.data_access().logical_memory_address().value() << std::dec
                << " | Latency (cache, instruction) = " << cache_latency << ", " << instruction_latency
                << " | Type = " << type << " | Data Source = " << data_source << "\n";
    } else if (sample.count_loss().has_value()) {
      std::cout << "Loss = " << sample.count_loss().value() << "\n";
    }
  }
  std::cout << std::flush;

  /// Close the sampler.
  /// Note that the sampler can only be closed after reading the samples.
  sampler.close();

  return 0;
}
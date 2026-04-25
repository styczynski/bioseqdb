#include "../access_benchmark.h"
#include <iostream>
#include <perfcpp/sampler.hpp>

int
main()
{
  std::cout << "libperf-cpp example: Record perf samples including time, "
               "user_registers, and cpu id for single-threaded random "
               "access to an in-memory array."
            << std::endl;

  auto sampler = perf::Sampler{};
  sampler.trigger(perf::Cycles{}, perf::Period{ 100000 });
  sampler.values()
    .timestamp(true)
    .user_registers(
      perf::Registers{ { perf::Registers::x86::IP, perf::Registers::x86::DI, perf::Registers::x86::R10 } })
    .kernel_registers(
      perf::Registers{ { perf::Registers::x86::IP, perf::Registers::x86::DI, perf::Registers::x86::R10 } })
    .cpu_id(true);

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

  samples.to_csv("samples_registers.csv");
  std::cout << "Wrote samples to `samples_registers.csv`." << std::endl;

  /// Print the first samples.
  const auto count_show_samples = std::min<std::size_t>(samples.size(), 40U);
  std::cout << "\nRecorded " << samples.size() << " samples." << std::endl;
  std::cout << "Here are the first " << count_show_samples << " recorded samples:\n" << std::endl;
  for (auto index = 0U; index < count_show_samples; ++index) {
    const auto& sample = samples[index];

    /// Since we recorded the time, period, the instruction pointer, and the CPU
    /// id, we can only read these values.
    if (sample.metadata().timestamp().has_value() &&
        (sample.user_registers().has_value() || sample.kernel_registers().has_value()) &&
        sample.metadata().cpu_id().has_value()) {

      std::cout << "Time = " << sample.metadata().timestamp().value()
                << " | CPU ID = " << sample.metadata().cpu_id().value();

      if (sample.user_registers().has_value()) {
        const auto& user_registers = sample.user_registers().value();
        std::cout << " | User Registers = IP(" << user_registers.get(perf::Registers::x86::IP).value_or(0) << "), DI("
                  << user_registers.get(perf::Registers::x86::DI).value_or(0) << "), R10("
                  << user_registers.get(perf::Registers::x86::R10).value_or(0) << ")";
      }

      if (sample.kernel_registers().has_value()) {
        const auto& kernel_registers = sample.kernel_registers().value();
        std::cout << " | Kernel Registers = IP(" << kernel_registers.get(perf::Registers::x86::IP).value_or(0)
                  << "), DI(" << kernel_registers.get(perf::Registers::x86::DI).value_or(0) << "), R10("
                  << kernel_registers.get(perf::Registers::x86::R10).value_or(0) << ")";
      }

      std::cout << "\n";
    }
  }
  std::cout << std::flush;

  /// Close the sampler.
  /// Note that the sampler can only be closed after reading the samples.
  sampler.close();

  return 0;
}
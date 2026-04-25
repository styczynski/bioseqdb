#include "../access_benchmark.h"
#include <iostream>
#include <perfcpp/sampler.hpp>
#include <perfcpp/util/symbol_resolver.hpp>

int
main()
{
  std::cout << "libperf-cpp example: Record perf samples including time, "
               "instruction pointer, and cpu id for single-threaded random "
               "access to an in-memory array."
            << std::endl;

  auto sampler = perf::Sampler{};

  /// Event that generates an overflow which is samples.
  sampler.trigger(perf::Cycles{}, perf::Precision::RequestZeroSkid, perf::Period{ 50000U });

  /// Include Timestamp, period, instruction pointer, and CPU number into samples.
  sampler.values().timestamp(true).period(true).logical_instruction_pointer(true).instruction_type(true).cpu_id(true);

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

  samples.to_csv("samples_instruction_pointer.csv");
  std::cout << "Wrote samples to `samples_instruction_pointer.csv`." << std::endl;

  auto symbol_resolver = perf::util::SymbolResolver{};

  /// Print the first samples.
  const auto count_show_samples = std::min<std::size_t>(samples.size(), 400U);
  std::cout << "\nRecorded " << samples.size() << " samples." << std::endl;
  std::cout << "Here are the first " << count_show_samples << " recorded samples:\n" << std::endl;
  for (auto index = 0U; index < count_show_samples; ++index) {
    const auto& sample = samples[index];

    /// Since we recorded the time, period, the instruction pointer, and the CPU
    /// id, we can only read these values.
    if (sample.metadata().timestamp().has_value() && sample.metadata().period().has_value() &&
        sample.instruction_execution().logical_instruction_pointer().has_value() &&
        sample.metadata().cpu_id().has_value()) {

      auto symbol = std::string{ "??" };
      if (auto sym = symbol_resolver.resolve(sample.instruction_execution().logical_instruction_pointer().value());
          sym.has_value()) {
        symbol = sym->to_string();
      }

      std::cout << "Time = " << sample.metadata().timestamp().value()
                << " | Period = " << sample.metadata().period().value() << " | Instruction Pointer = 0x" << std::hex
                << sample.instruction_execution().logical_instruction_pointer().value() << std::dec
                << " | Symbol = " << symbol << " | CPU ID = " << sample.metadata().cpu_id().value() << " | "
                << (sample.instruction_execution().logical_instruction_pointer() ? "exact" : "not exact") << "\n";
    }
  }
  std::cout << std::flush;

  /// Close the sampler.
  /// Note that the sampler can only be closed after reading the samples.
  sampler.close();

  return 0;
}
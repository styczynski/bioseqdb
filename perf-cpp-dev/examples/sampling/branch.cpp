#include "../access_benchmark.h"
#include <iostream>
#include <perfcpp/sampler.hpp>

/**
 * A function using multiple branches hard to optimize for the compiler for
 * demonstrating branch-sampling.
 *
 * @param cache_line Cache line to use as an input.
 * @return Another value through a handful of branches.
 */
[[nodiscard]] std::uint64_t
branchy_function(const perf::example::AccessBenchmark::cache_line& cache_line);

int
main()
{

  std::cout << "libperf-cpp example: Record perf branch samples for "
               "single-threaded sequential access to an in-memory array."
            << std::endl;

  /// Initialize sampler.
  auto sampler = perf::Sampler{};

  /// Setup which counters trigger the writing of samples.
  sampler.trigger(perf::Cycles{}, perf::Precision::AllowArbitrarySkid, perf::Period{ 1000000U });

  /// Setup which data will be included into samples (timestamp and stack of branches).
  sampler.values().timestamp(true).branch_stack(
    { perf::BranchType::User, perf::BranchType::Conditional }) /// Only sample conditional branches in user-mode.
    ;

  /// Create random access benchmark.
  auto benchmark = perf::example::AccessBenchmark{ /*sequential accesses*/ false,
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
    value += branchy_function(benchmark[index]);
  }

  /// We do not want the compiler to optimize away this (otherwise) unused value (and consequently the loop above).
  benchmark.pretend_to_use(value);

  /// Stop sampling.
  sampler.stop();

  /// Get all the recorded samples.
  const auto samples = sampler.result();

  /// Print the first samples.
  const auto count_show_samples = std::min<std::size_t>(samples.size(), 10U);
  std::cout << "\nRecorded " << samples.size() << " samples." << std::endl;
  std::cout << "Here are the first " << count_show_samples << " recorded samples:\n" << std::endl;

  for (auto index = 0U; index < count_show_samples; ++index) {
    const auto& sample = samples[index];

    /// Since we recorded the time, period, the instruction pointer, and the CPU
    /// id, we can only read these values.
    if (sample.metadata().timestamp().has_value() && sample.branch_stack().has_value()) {
      std::cout << "Time = " << sample.metadata().timestamp().value() << "\n";
      for (const auto& branch : sample.branch_stack().value()) {
        std::cout << "\tpredicted correct = " << branch.is_predicted() << " | from instruction 0x" << std::hex
                  << branch.instruction_pointer_from() << std::dec << " | to instruction 0x" << std::hex
                  << branch.instruction_pointer_to() << std::dec;
        if (branch.cycles().has_value()) {
          std::cout << " | cycles = " << branch.cycles().value();
        }

        std::cout << "\n";
      }
    }
  }
  std::cout << std::flush;

  /// Close the sampler.
  /// Note that the sampler can only be closed after reading the samples.
  sampler.close();

  return 0;
}

std::uint64_t
branchy_function(const perf::example::AccessBenchmark::cache_line& cache_line)
{
  auto result = cache_line.value;

  for (auto i = 0U; i < 10U; ++i) {
    switch ((cache_line.value >> (4U * i)) & 0xF) { // Extract 4 bits at a time
      case 0ULL:
        result += cache_line.value * (i + 1U);
        break;
      case 1ULL:
        result -= cache_line.value / (i + 2U);
        break;
      case 2ULL:
        result *= cache_line.value + (i * 3U);
        break;
      case 3ULL:
        result /= (cache_line.value - i) | 1U;
        break; // Avoid division by zero
      case 4ULL:
        result ^= cache_line.value << i;
        break;
      case 5ULL:
        result %= (cache_line.value >> i) | 1U;
        break;
      case 6ULL:
        result = ~result;
        break;
      case 7ULL:
        result &= cache_line.value | (std::uint64_t(0xFF) << (i * 8U));
        break;
      case 8ULL:
        result |= cache_line.value & (std::uint64_t(0xFFFF) << (i * 16U));
        break;
      case 9ULL:
        result >>= cache_line.value % (i + 1);
        break;
      case 10ULL:
        result <<= cache_line.value % (i + 2);
        break;
      case 11ULL:
        result += cache_line.value + i * 7;
        break;
      case 12ULL:
        result -= cache_line.value - i * 11;
        break;
      case 13ULL:
        result *= cache_line.value * (i + 5);
        break;
      case 14ULL:
        result /= (cache_line.value / (i + 3)) | 1;
        break;
      case 15ULL:
        result ^= cache_line.value ^ (i * 13);
        break;
      default:
        result = cache_line.value;
    }
  }
  return result;
}
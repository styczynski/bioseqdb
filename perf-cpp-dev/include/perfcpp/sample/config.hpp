#pragma once

#include <perfcpp/counter/config.hpp>
#include <perfcpp/sample/period.hpp>
#include <perfcpp/sample/precision.hpp>

namespace perf {
/**
 * The SampleConfig extends Config with sampling-specific settings such as precision,
 * buffer allocation, and period/frequency for sampling.
 */
class SampleConfig final : public Config
{
public:
  SampleConfig() noexcept = default;
  ~SampleConfig() noexcept = default;

  SampleConfig(const SampleConfig&) noexcept = default;
  SampleConfig(SampleConfig&&) noexcept = default;

  SampleConfig& operator=(const SampleConfig&) noexcept = default;
  SampleConfig& operator=(SampleConfig&&) noexcept = default;

  /**
   * @return Default precision for sampling.
   */
  [[nodiscard]] Precision precise_ip() const noexcept { return _precise_ip; }

  /**
   * @return Number of pages to allocate for the user-level buffer that receives samples.
   */
  [[nodiscard]] std::uint64_t buffer_pages() const noexcept { return _buffer_pages; }

  /**
   * @return Default period or frequency for sampling.
   */
  [[nodiscard]] PeriodOrFrequency period_or_frequency() const noexcept { return _period_or_frequency; }

  /**
   * Default frequency to sample, if not specified along with a trigger. The frequency denotes to samples per second.
   * Note that either frequency or period can be specified.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#period--frequency
   *
   * @param frequency Frequency to sample (samples per second).
   */
  void frequency(const std::uint64_t frequency) noexcept { _period_or_frequency = Frequency{ frequency }; }

  /**
   * Default period to sample, if not specified along with a trigger. The period denotes to one sample every <period>
   * trigger events. Note that either frequency or period can be specified.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#period--frequency
   *
   * @param period Period to sample (one sample every <period> eventy reported by the trigger).
   */
  void period(const std::uint64_t period) noexcept { _period_or_frequency = Period{ period }; }

  /**
   * Default precision for sampling, if not specified along with a trigger.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#precision
   *
   * @param precision Default precision for sampling.
   */
  void precision(const Precision precision) noexcept { _precise_ip = precision; }

  /**
   * Specifies the number of pages allocated for the user-level buffer that receives samples.
   *
   * See https://github.com/jmuehlig/perf-cpp/blob/dev/docs/sampling.md#sample-buffer
   *
   * @param buffer_pages Number of pages allocated for the user-level buffer that receives samples.
   */
  void buffer_pages(const std::uint64_t buffer_pages) noexcept { _buffer_pages = buffer_pages; }

private:
  /// Number of pages allocated for the user-level buffer.
  std::uint64_t _buffer_pages{ /* pages for the data */ 4096U + /* one page for the metadata */ 1U };

  /// Default frequency or period, if not specified for a trigger.
  PeriodOrFrequency _period_or_frequency{ Period{ 4000U } };

  /// Default precision for sampling, if not specified for a trigger.
  Precision _precise_ip{ Precision::MustHaveConstantSkid /* Enable Intel PEBS by default */ };
};
}
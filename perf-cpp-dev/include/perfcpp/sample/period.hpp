#pragma once
#include <cstdint>
#include <variant>

namespace perf {
/**
 * Represents a sampling period, defining one sample every N trigger events.
 */
class Period
{
public:
  explicit Period(const std::uint64_t period) noexcept
    : _period(period)
  {
  }
  ~Period() noexcept = default;

  Period(const Period&) noexcept = default;
  Period(Period&&) noexcept = default;

  [[nodiscard]] Period& operator=(const Period&) noexcept = default;
  [[nodiscard]] Period& operator=(Period&&) noexcept = default;

  /**
   * Returns the sampling period value.
   *
   * @return The period value (one sample every N trigger events).
   */
  [[nodiscard]] std::uint64_t get() const noexcept { return _period; }

private:
  /// The period value (one sample every N trigger events).
  std::uint64_t _period;
};

/**
 * Represents a sampling frequency, defining the number of samples per second.
 */
class Frequency
{
public:
  explicit Frequency(const std::uint64_t frequency) noexcept
    : _frequency(frequency)
  {
  }
  ~Frequency() noexcept = default;

  Frequency(const Frequency&) noexcept = default;
  Frequency(Frequency&&) noexcept = default;

  [[nodiscard]] Frequency& operator=(const Frequency&) noexcept = default;
  [[nodiscard]] Frequency& operator=(Frequency&&) noexcept = default;

  /**
   * Returns the sampling frequency value.
   *
   * @return The frequency value (samples per second).
   */
  [[nodiscard]] std::uint64_t get() const noexcept { return _frequency; }

private:
  /// The frequency value (samples per second).
  std::uint64_t _frequency;
};

/**
 * Type alias for a variant that can hold either a Period or a Frequency.
 */
using PeriodOrFrequency = std::variant<Period, Frequency>;
}
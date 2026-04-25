#pragma once
#include <chrono>
#include <string>

namespace perf {
class TimeEvent
{
public:
  TimeEvent() = default;
  TimeEvent(const TimeEvent&) = default;
  TimeEvent(TimeEvent&&) = default;

  virtual ~TimeEvent() noexcept = default;

  TimeEvent& operator=(const TimeEvent&) = default;
  TimeEvent& operator=(TimeEvent&&) = default;

  [[nodiscard]] virtual double calculate(std::chrono::steady_clock::time_point start,
                                         std::chrono::steady_clock::time_point end) const noexcept = 0;
};

class SecondsTimeEvent final : public TimeEvent
{
public:
  SecondsTimeEvent() = default;
  SecondsTimeEvent(const SecondsTimeEvent&) = default;
  SecondsTimeEvent(SecondsTimeEvent&&) = default;

  ~SecondsTimeEvent() noexcept override = default;

  SecondsTimeEvent& operator=(const SecondsTimeEvent&) = default;
  SecondsTimeEvent& operator=(SecondsTimeEvent&&) = default;

  [[nodiscard]] double calculate(const std::chrono::steady_clock::time_point start,
                                 const std::chrono::steady_clock::time_point end) const noexcept override
  {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()) / 1000000000.;
  }
};

class MillisecondsTimeEvent final : public TimeEvent
{
public:
  MillisecondsTimeEvent() = default;
  MillisecondsTimeEvent(const MillisecondsTimeEvent&) = default;
  MillisecondsTimeEvent(MillisecondsTimeEvent&&) = default;

  ~MillisecondsTimeEvent() noexcept override = default;

  MillisecondsTimeEvent& operator=(const MillisecondsTimeEvent&) = default;
  MillisecondsTimeEvent& operator=(MillisecondsTimeEvent&&) = default;

  [[nodiscard]] double calculate(const std::chrono::steady_clock::time_point start,
                                 const std::chrono::steady_clock::time_point end) const noexcept override
  {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()) / 1000000.;
  }
};

class MicrosecondsTimeEvent final : public TimeEvent
{
public:
  MicrosecondsTimeEvent() = default;
  MicrosecondsTimeEvent(const MicrosecondsTimeEvent&) = default;
  MicrosecondsTimeEvent(MicrosecondsTimeEvent&&) = default;

  ~MicrosecondsTimeEvent() noexcept override = default;

  MicrosecondsTimeEvent& operator=(const MicrosecondsTimeEvent&) = default;
  MicrosecondsTimeEvent& operator=(MicrosecondsTimeEvent&&) = default;

  [[nodiscard]] double calculate(const std::chrono::steady_clock::time_point start,
                                 const std::chrono::steady_clock::time_point end) const noexcept override
  {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()) / 1000.;
  }
};

class NanosecondsTimeEvent final : public TimeEvent
{
public:
  NanosecondsTimeEvent() = default;
  NanosecondsTimeEvent(const NanosecondsTimeEvent&) = default;
  NanosecondsTimeEvent(NanosecondsTimeEvent&&) = default;

  ~NanosecondsTimeEvent() noexcept override = default;

  NanosecondsTimeEvent& operator=(const NanosecondsTimeEvent&) = default;
  NanosecondsTimeEvent& operator=(NanosecondsTimeEvent&&) = default;

  [[nodiscard]] double calculate(const std::chrono::steady_clock::time_point start,
                                 const std::chrono::steady_clock::time_point end) const noexcept override
  {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
  }
};
}
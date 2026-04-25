#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace perf {
class Metadata
{
public:
  enum class Mode : std::uint8_t
  {
    Kernel,
    User,
    Hypervisor,
    GuestKernel,
    GuestUser,
  };

  /**
   * Set the mode of the sample.
   * @param mode Mode.
   */
  void mode(const std::optional<Mode> mode) noexcept { _mode = mode; }

  /**
   * Set the sample ID.
   * @param sample_id Sample ID.
   */
  void sample_id(const std::uint64_t sample_id) noexcept { _sample_id = sample_id; }

  /**
   * Set the stream ID.
   * @param stream_id Stream ID.
   */
  void stream_id(const std::uint64_t stream_id) noexcept { _stream_id = stream_id; }

  /**
   * Set the timestamp.
   * @param timestamp Timestamp.
   */
  void timestamp(const std::uint64_t timestamp) noexcept { _timestamp = timestamp; }

  /**
   * Set the period.
   * @param period Period.
   */
  void period(const std::uint64_t period) noexcept { _period = period; }

  /**
   * Set the CPU ID.
   * @param cpu_id CPU ID.
   */
  void cpu_id(const std::uint32_t cpu_id) noexcept { _cpu_id = cpu_id; }

  /**
   * Set the process ID.
   * @param process_id Process ID.
   */
  void process_id(const std::uint32_t process_id) noexcept { _process_id = process_id; }

  /**
   * Set the thread ID.
   * @param thread_id Thread ID.
   */
  void thread_id(const std::uint32_t thread_id) noexcept { _thread_id = thread_id; }

  /**
   * @return Mode, if included in the sample. std::nullopt otherwise.
   */
  [[nodiscard]] std::optional<Mode> mode() const noexcept { return _mode; }

  /**
   * @return Sample ID, if included in the sample. std::nullopt otherwise.
   */
  [[nodiscard]] std::optional<std::uint64_t> sample_id() const noexcept { return _sample_id; }

  /**
   * @return Stream ID, if included in the sample. std::nullopt otherwise.
   */
  [[nodiscard]] std::optional<std::uint64_t> stream_id() const noexcept { return _stream_id; }

  /**
   * @return Timestamp, if included in the sample. std::nullopt otherwise.
   */
  [[nodiscard]] std::optional<std::uint64_t> timestamp() const noexcept { return _timestamp; }

  /**
   * @return Period, if included in the sample. std::nullopt otherwise.
   */
  [[nodiscard]] std::optional<std::uint64_t> period() const noexcept { return _period; }

  /**
   * @return CPU ID, if included in the sample. std::nullopt otherwise.
   */
  [[nodiscard]] std::optional<std::uint32_t> cpu_id() const noexcept { return _cpu_id; }

  /**
   * @return Process ID, if included in the sample. std::nullopt otherwise.
   */
  [[nodiscard]] std::optional<std::uint32_t> process_id() const noexcept { return _process_id; }

  /**
   * @return Thread ID, if included in the sample. std::nullopt otherwise.
   */
  [[nodiscard]] std::optional<std::uint32_t> thread_id() const noexcept { return _thread_id; }

private:
  std::optional<Mode> _mode;
  std::optional<std::uint64_t> _sample_id;
  std::optional<std::uint64_t> _stream_id;
  std::optional<std::uint64_t> _timestamp;
  std::optional<std::uint64_t> _period;
  std::optional<std::uint32_t> _cpu_id;
  std::optional<std::uint32_t> _process_id;
  std::optional<std::uint32_t> _thread_id;
};

/**
 * Convert Mode to its string representation.
 *
 * @param mode The mode to convert.
 * @return String representation of the mode.
 */
[[nodiscard]] inline std::string
to_string(const std::optional<Metadata::Mode> mode)
{
  if (mode.has_value()) {
    switch (mode.value()) {
      case Metadata::Mode::Kernel:
        return "kernel";
      case Metadata::Mode::User:
        return "user";
      case Metadata::Mode::Hypervisor:
        return "hypervisor";
      case Metadata::Mode::GuestKernel:
        return "guest_kernel";
      case Metadata::Mode::GuestUser:
        return "guest_user";
      default:
        return "unknown";
    }
  }

  return "";
}
}
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace perf {

/**
 * Parser for perf event descriptor files exposed by the kernel
 * under /sys/bus/event_source/devices/.
 * Constructed with a base path (e.g., /sys/bus/event_source/devices/cpu/)
 * and provides methods to parse type, format, event config, and scale
 * files relative to that path.
 */
class EventFileDescriptorParser
{
public:
  explicit EventFileDescriptorParser(std::string path) noexcept
    : _path(std::move(path))
  {
  }

  ~EventFileDescriptorParser() noexcept = default;

  EventFileDescriptorParser(const EventFileDescriptorParser&) = default;
  EventFileDescriptorParser(EventFileDescriptorParser&&) noexcept = default;
  EventFileDescriptorParser& operator=(const EventFileDescriptorParser&) = default;
  EventFileDescriptorParser& operator=(EventFileDescriptorParser&&) noexcept = default;

  /**
   * Reads the PMU type from the "type" file.
   *
   * @return Integer representation of type.
   */
  [[nodiscard]] std::optional<std::uint32_t> type() const;

  /**
   * Reads the scale factor from the scale file of the given event.
   *
   * @param event_name Name of the event (reads from events/<event_name>.scale).
   * @return Double representation of scale.
   */
  [[nodiscard]] std::optional<double> scale(const std::string& event_name) const;

  /**
   * Parses an event file descriptor for the given event.
   * Typically, event file descriptors contain the event code, umask, and some additional data
   * (e.g., ldlat for load latency).
   *
   * @param event_name Name of the event (reads from events/<event_name>).
   * @return A pair of configuration code and (optional) additional information, like load latency.
   */
  [[nodiscard]] std::optional<std::pair<std::uint64_t, std::optional<std::uint64_t>>> config(
    const std::string& event_name) const;

  /**
   * Reads a format file and returns the id of the config and the number of bits.
   * Some formats have multiple entries.
   *
   * @param format_name Name of the format entry (reads from format/<format_name>).
   * @return List of pairs (config id, bits).
   */
  [[nodiscard]] std::vector<std::pair<std::uint8_t, std::pair<std::uint8_t, std::optional<std::uint8_t>>>> format(
    const std::string& format_name) const;

  /**
   * @return The base path of this parser.
   */
  [[nodiscard]] const std::string& path() const noexcept { return _path; }

  /**
   * Parses an integer (decimal or hex) from a given string.
   *
   * @param value String to parse.
   * @return Integer, if parsable.
   */
  [[nodiscard]] static std::optional<std::uint64_t> integer(const std::string& value);

private:
  /// Base path of the PMU device (e.g., /sys/bus/event_source/devices/cpu/).
  std::string _path;
};

}

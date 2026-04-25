#include <algorithm>
#include <cctype>
#include <fstream>
#include <perfcpp/event_file_descriptor_parser.hpp>
#include <regex>
#include <sstream>
#include <unordered_map>

std::optional<std::uint32_t>
perf::EventFileDescriptorParser::type() const
{
  auto path = std::filesystem::path{ this->_path } / "type";
  if (!std::filesystem::exists(path)) {
    return std::nullopt;
  }

  auto type_stream = std::ifstream{ path };
  if (type_stream.is_open()) {
    auto type = std::uint32_t{};
    type_stream >> type;

    return type;
  }

  return std::nullopt;
}

std::optional<double>
perf::EventFileDescriptorParser::scale(const std::string& event_name) const
{
  auto path = std::filesystem::path{ this->_path } / "events" / (event_name + ".scale");
  if (!std::filesystem::exists(path)) {
    return std::nullopt;
  }

  auto scale_stream = std::ifstream{ path };
  if (scale_stream.is_open()) {
    auto scale = double{};
    scale_stream >> scale;

    return scale;
  }

  return std::nullopt;
}

std::optional<std::pair<std::uint64_t, std::optional<std::uint64_t>>>
perf::EventFileDescriptorParser::config(const std::string& event_name) const
{
  auto path = std::filesystem::path{ this->_path } / "events" / event_name;
  auto event_stream = std::ifstream{ path };
  if (event_stream.is_open()) {
    std::string line;
    std::getline(event_stream, line);

    if (line.empty()) {
      return std::nullopt;
    }

    /// Store all entries (A,B) from parsing the line in the format "A=B[,C=D]*", with entries being "event", "umask",
    /// or "ldlat".
    auto entries = std::unordered_map<std::string, std::uint64_t>{};

    auto token_stream = std::stringstream{ line };
    std::string token;

    /// Process every token where tokens are separated by ','.
    while (std::getline(token_stream, token, ',')) {

      /// Locate eq-char.
      const auto pos = token.find('=');
      if (pos == std::string::npos) {
        continue;
      }

      auto key = token.substr(0ULL, pos);
      auto value = token.substr(pos + 1ULL);

      /// Remove possible whitespace.
      key.erase(std::remove_if(key.begin(), key.end(), ::isspace), key.end());
      value.erase(std::remove_if(value.begin(), value.end(), ::isspace), value.end());

      /// Convert key to lowercase for case-insensitivity.
      std::transform(key.begin(), key.end(), key.begin(), ::tolower);

      /// Transform value into integer and add to entries.
      if (!key.empty()) {
        if (const auto integer_value = EventFileDescriptorParser::integer(value); integer_value.has_value()) {
          entries.insert(std::make_pair(std::move(key), integer_value.value()));
        }
      }
    }

    /// Combine event and umask to a single event id.
    if (const auto event = entries.find("event"); event != entries.end()) {

      /// Fetch event value.
      auto event_value = event->second;

      /// Apply umask, if available.
      if (const auto umask = entries.find("umask"); umask != entries.end()) {
        event_value = (umask->second << 8) | event_value;
      }

      /// Add load latency, if found (only available for mem-load on Intel PEBS).
      if (const auto load_latency = entries.find("ldlat"); load_latency != entries.end()) {
        return std::make_pair(event_value, load_latency->second);
      }

      return std::make_pair(event_value, std::nullopt);
    }

    /// Some AMD IO MMU events are configured via csource instead.
    if (const auto csource = entries.find("csource"); csource != entries.end()) {
      return std::make_pair(csource->second, std::nullopt);
    }
  }

  return std::nullopt;
}

std::vector<std::pair<std::uint8_t, std::pair<std::uint8_t, std::optional<std::uint8_t>>>>
perf::EventFileDescriptorParser::format(const std::string& format_name) const
{
  auto path = std::filesystem::path{ this->_path } / "format" / format_name;
  auto configs = std::vector<std::pair<std::uint8_t, std::pair<std::uint8_t, std::optional<std::uint8_t>>>>{};
  if (!std::filesystem::exists(path)) {
    return configs;
  }

  auto format_file = std::ifstream{ path };

  if (!format_file.is_open()) {
    return configs;
  }

  std::string line;
  if (std::getline(format_file, line); !line.empty()) {
    auto config_pattern = std::regex("config([0-9]?):(\\d+)(?:-(\\d+))?");

    auto stream = std::stringstream{ line };
    std::string entry;

    while (std::getline(stream, entry, ',')) {
      if (std::smatch match; std::regex_match(entry, match, config_pattern)) {
        const auto config_id = match[1U].length() == 0U ? 0 : std::stoi(match[1U].str());
        const auto bit_start = std::stoi(match[2U].str());
        const auto bit_end = match[3U].length() == 0U ? std::nullopt : std::make_optional(std::stoi(match[2U].str()));

        configs.emplace_back(config_id, std::make_pair(bit_start, bit_end));
      }
    }
  }

  return configs;
}

std::optional<std::uint64_t>
perf::EventFileDescriptorParser::integer(const std::string& value)
{
  if (value.empty()) {
    return std::nullopt;
  }

  /// Remove all whitespaces if the value has at least one.
  if (value.find_first_of(' ') != std::string::npos) {
    auto value_without_leading_whitespace = value;
    value_without_leading_whitespace.erase(
      std::remove_if(value_without_leading_whitespace.begin(), value_without_leading_whitespace.end(), ::isspace),
      value_without_leading_whitespace.end());
    return EventFileDescriptorParser::integer(value_without_leading_whitespace);
  }

  /// Strings starting with '0x' are considered hex numbers.
  if (value.rfind("0x", 0ULL) == 0ULL) {
    return std::stoull(value.substr(2ULL), nullptr, 16);
  }

  /// Strings containing digits are considered dec numbers.
  if (std::all_of(value.begin(), value.end(), [](const auto c) { return std::isdigit(c); })) {
    return std::stoull(value, nullptr, 0);
  }

  return std::nullopt;
}

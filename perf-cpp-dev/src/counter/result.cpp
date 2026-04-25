#include <algorithm>
#include <fstream>
#include <iomanip>
#include <perfcpp/counter/result.hpp>
#include <sstream>
#include <tuple>

std::optional<double>
perf::CounterResult::get(const std::string_view name) const noexcept
{
  if (const auto result_iterator = std::find_if(
        this->_results.begin(), this->_results.end(), [&name](const auto res) { return name == res.first; });
      result_iterator != this->_results.end()) {
    return result_iterator->second;
  }

  /// If the name is in '<package>/<event_name>' format, retry with just the event name.
  if (const auto slash_pos = name.find('/');
      slash_pos != std::string::npos && name.find('/', slash_pos + 1) == std::string::npos) {
    return this->get(name.substr(slash_pos + 1));
  }

  return std::nullopt;
}

std::string
perf::CounterResult::to_json() const
{
  auto json_stream = std::stringstream{};

  json_stream << "{";

  for (auto i = 0U; i < this->_results.size(); ++i) {
    if (i > 0U) {
      json_stream << ",";
    }

    json_stream << "\"" << this->_results[i].first << "\": " << this->_results[i].second;
  }

  json_stream << "}";

  return json_stream.str();
}

std::string
perf::CounterResult::to_csv(const char delimiter, const bool print_header) const
{
  auto csv_stream = std::stringstream{};

  if (print_header) {
    csv_stream << "counter" << delimiter << "value\n";
  }

  for (auto i = 0U; i < this->_results.size(); ++i) {
    if (i > 0U) {
      csv_stream << "\n";
    }

    csv_stream << this->_results[i].first << delimiter << this->_results[i].second;
  }

  return csv_stream.str();
}

void
perf::CounterResult::to_json(const std::string_view file_name) const
{
  auto json_file = std::ofstream{ std::string{ file_name } };
  json_file << this->to_json();
}

void
perf::CounterResult::to_csv(const std::string_view file_name, const char delimiter, const bool print_header) const
{
  auto csv_file = std::ofstream{ std::string{ file_name } };
  csv_file << this->to_csv(delimiter, print_header);
}

std::string
perf::CounterResult::to_string() const
{
  if (this->_results.empty()) {
    return "No results collected.";
  }

  auto rows = std::vector<std::tuple<std::string_view, std::string>>{};

  /// Collect counter names and values as strings.
  for (const auto& [name, value] : this->_results) {
    rows.emplace_back(name, std::to_string(value));
  }

  /// Find the max string lengths.
  auto max_string_lengths =
    std::tuple<std::size_t, std::size_t>{ std::get<0>(rows.front()).size(), std::get<1>(rows.front()).size() };
  for (auto i = 1U; i < rows.size(); ++i) {
    std::get<0>(max_string_lengths) = std::max(std::get<0>(max_string_lengths), std::get<0>(rows[i]).size());
    std::get<1>(max_string_lengths) = std::max(std::get<1>(max_string_lengths), std::get<1>(rows[i]).size());
  }

  /// Print to stream.
  auto out_stream = std::stringstream{};
  out_stream << "Performance counter stats:\n";
  for (const auto& [name, value] : rows) {
    out_stream << '\n'
               << std::string(8 + (std::get<1>(max_string_lengths) - value.size()), ' ') << value << std::string(6, ' ')
               << name;
  }

  out_stream << std::flush;
  return out_stream.str();
}
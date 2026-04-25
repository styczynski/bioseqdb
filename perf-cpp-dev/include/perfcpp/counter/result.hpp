#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace perf {
class CounterResult
{
public:
  using iterator = std::vector<std::pair<std::string_view, double>>::iterator;
  using const_iterator = std::vector<std::pair<std::string_view, double>>::const_iterator;

  CounterResult() = default;
  CounterResult(CounterResult&&) noexcept = default;
  CounterResult(const CounterResult&) = default;
  explicit CounterResult(std::vector<std::pair<std::string_view, double>>&& results) noexcept
    : _results(std::move(results))
  {
  }

  ~CounterResult() = default;

  CounterResult& operator=(CounterResult&&) noexcept = default;
  CounterResult& operator=(const CounterResult&) = default;

  /**
   * Access the result of the counter or metric with the given name.
   *
   * @param name Name of the counter or metric to access.
   * @return The value, or std::nullopt of the result has no counter or value with the requested name.
   */
  [[nodiscard]] std::optional<double> get(std::string_view name) const noexcept;

  [[nodiscard]] std::optional<double> operator[](const std::string_view name) const noexcept { return get(name); }

  [[nodiscard]] iterator begin() { return _results.begin(); }
  [[nodiscard]] iterator end() { return _results.end(); }
  [[nodiscard]] const_iterator begin() const { return _results.begin(); }
  [[nodiscard]] const_iterator end() const { return _results.end(); }

  /**
   * @return Number of results.
   */
  [[nodiscard]] std::size_t size() const noexcept { return _results.size(); }

  /**
   * @return True if no results were collected.
   */
  [[nodiscard]] bool empty() const noexcept { return _results.empty(); }

  /**
   * Adds the given result to the end of the results.
   *
   * @param name Name of the result.
   * @param value Value of the result.
   */
  void emplace_back(const std::string_view name, const double value) { _results.emplace_back(name, value); }

  /**
   * Converts the result to a json-formatted string.
   * @return Result in JSON format.
   */
  [[nodiscard]] std::string to_json() const;

  /**
   * Writes the result to a JSON file.
   *
   * @param file_name Path to the output file.
   */
  void to_json(std::string_view file_name) const;

  /**
   * Converts the result to a CSV-formatted string.
   *
   * @param delimiter Char to separate columns (',' by default).
   * @param print_header If true, the header will be printed first (true by default).
   * @return Result in CSV format.
   */
  [[nodiscard]] std::string to_csv(char delimiter = ',', bool print_header = true) const;

  /**
   * Writes the result to a CSV file.
   *
   * @param file_name Path to the output file.
   * @param delimiter Char to separate columns (',' by default).
   * @param print_header If true, the header will be printed first (true by default).
   */
  void to_csv(std::string_view file_name, char delimiter = ',', bool print_header = true) const;

  /**
   * Converts the result to a table-formatted string.
   * @return Result as a table-formatted string.
   */
  [[nodiscard]] std::string to_string() const;

private:
  std::vector<std::pair<std::string_view, double>> _results;
};
}
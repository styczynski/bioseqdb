#pragma once
#include <functional>
#include <ostream>
#include <perfcpp/sample/recording_values.hpp>
#include <perfcpp/sample/sample.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace perf {
/**
 * The SampleResult holds a collection of samples recorded by the perf subsystem and provides
 * utilities to access, iterate, and filter the samples.
 */
class SampleResult
{
public:
  using value_type = Sample;

  SampleResult() = default;
  explicit SampleResult(std::vector<value_type>&& samples) noexcept
    : _samples(std::move(samples))
  {
  }

  SampleResult(SampleRecordingValues values, std::vector<value_type>&& samples) noexcept
    : _sample_recording_values(std::move(values))
    , _samples(std::move(samples))
  {
  }

  SampleResult(const SampleResult&) = default;
  SampleResult(SampleResult&&) noexcept = default;

  SampleResult& operator=(const SampleResult&) = default;
  SampleResult& operator=(SampleResult&&) noexcept = default;

  ~SampleResult() = default;

  /**
   * Converts the SampleResult into a vector of samples by moving the underlying data.
   *
   * @return Vector of samples.
   */
  [[nodiscard]] operator std::vector<value_type>() noexcept { return std::move(_samples); }

  /**
   * Converts the SampleResult into a vector of samples by copying the underlying data.
   *
   * @return Vector of samples.
   */
  [[nodiscard]] operator std::vector<value_type>() const noexcept { return _samples; }

  /**
   * Returns the number of samples in the result.
   *
   * @return Number of samples.
   */
  [[nodiscard]] std::size_t size() const noexcept { return _samples.size(); }

  /**
   * Returns true if the result is empty, i.e., does not contain any samples.
   *
   * @return True if the result does not contain samples.
   */
  [[nodiscard]] bool empty() const noexcept { return _samples.empty(); }

  /**
   * Returns an iterator to the beginning of the samples.
   *
   * @return Iterator to the first sample.
   */
  [[nodiscard]] std::vector<value_type>::iterator begin() noexcept { return _samples.begin(); }

  /**
   * Returns an iterator to the end of the samples.
   *
   * @return Iterator to the element past the last sample.
   */
  [[nodiscard]] std::vector<value_type>::iterator end() noexcept { return _samples.end(); }

  /**
   * Returns a const iterator to the beginning of the samples.
   *
   * @return Const iterator to the first sample.
   */
  [[nodiscard]] std::vector<value_type>::const_iterator begin() const noexcept { return _samples.begin(); }

  /**
   * Returns a const iterator to the end of the samples.
   *
   * @return Const iterator to the element past the last sample.
   */
  [[nodiscard]] std::vector<value_type>::const_iterator end() const noexcept { return _samples.end(); }

  /**
   * Adds a sample to the end of the collection.
   *
   * @param sample Sample to add.
   */
  void push_back(value_type&& sample) noexcept { _samples.push_back(std::move(sample)); }

  /**
   * Accesses a sample at the specified index.
   *
   * @param index Index of the sample to access.
   * @return Reference to the sample at the specified index.
   */
  [[nodiscard]] const Sample& operator[](const std::size_t index) const noexcept { return _samples[index]; }

  /**
   * Accesses the first sample.
   *
   * @return Reference to the first sample.
   */
  [[nodiscard]] const Sample& front() const noexcept { return _samples.front(); }

  /**
   * Accesses the first sample.
   *
   * @return Reference to the first sample.
   */
  [[nodiscard]] Sample& front() noexcept { return _samples.front(); }

  /**
   * Accesses the last sample.
   *
   * @return Reference to the last sample.
   */
  [[nodiscard]] const Sample& back() const noexcept { return _samples.back(); }

  /**
   * Accesses the last sample.
   *
   * @return Reference to the last sample.
   */
  [[nodiscard]] Sample& back() noexcept { return _samples.back(); }

  /**
   * Filters the samples by unary function.
   *
   * @param filter Filter function mapping from sample to bool where "true" indicates to keep the value while "false"
   * discards the sample.
   */
  void filter(std::function<bool(const Sample&)> filter);

  /**
   * Returns the sample results as a CSV-formatted string.
   *
   * @param delimiter Character used to separate CSV columns (default: ',').
   * @param list_delimiter Character used to separate list elements within a cell (default: ';').
   * @return CSV-formatted string containing the sample results.
   */
  [[nodiscard]] std::string to_csv(char delimiter = ',', char list_delimiter = ';') const;

  /**
   * Writes the sample results as CSV to the given file.
   *
   * @param file_name File to write the sample results in CSV format.
   * @param delimiter Character used to separate CSV columns (default: ',').
   * @param list_delimiter Character used to separate list elements within a cell (default: ';').
   */
  void to_csv(std::string_view file_name, char delimiter = ',', char list_delimiter = ';') const;

  /**
   * Writes the sample results as flamegraphs input to the given file.
   *
   * @param file_name File to write the sample results in a format that can be read by flame graph generators.
   */
  void to_flamegraphs(std::string_view file_name) const;

private:
  /// Writes all sample data as CSV to the given output stream.
  void write_csv(std::ostream& stream, char delimiter, char list_delimiter) const;

  /// List of values recorded by the sample. These values are represented in the samples.
  SampleRecordingValues _sample_recording_values;

  /// List of samples.
  std::vector<value_type> _samples;

  /**
   * Helper class for writing sample data to CSV format.
   * Handles conditional field output based on SampleRecordingValues configuration,
   * type conversion, and formatting with configurable delimiters.
   */
  class CSVWriter
  {
  public:
    CSVWriter(std::ostream& stream,
              const SampleRecordingValues& sample_recording_values,
              const char delimiter = ',',
              const char list_delimiter = ';') noexcept
      : _csv_stream(stream)
      , _values(sample_recording_values)
      , _delimiter(delimiter)
      , _list_delimiter(list_delimiter)
    {
    }

    CSVWriter(const CSVWriter&) = delete;
    CSVWriter(CSVWriter&&) = delete;

    ~CSVWriter() = default;

    CSVWriter& operator=(const CSVWriter&) = delete;
    CSVWriter& operator=(CSVWriter&&) = delete;

    /**
     * Writes a CSV header column name if the field is enabled in the sample recording values.
     *
     * @param field The field to check if enabled.
     * @param name The column name to write.
     */
    void write_header(const SampleRecordingValues::Field field, std::string&& name)
    {
      if (this->_values.is_set(field)) {
        this->_csv_stream << _delimiter << name;
      }
    }

    /**
     * Writes a boolean value to CSV if the field is enabled.
     *
     * @param field The field to check if enabled.
     * @param value The boolean value to write.
     */
    void write_value(const SampleRecordingValues::Field field, const bool value)
    {
      if (this->_values.is_set(field)) {
        this->_csv_stream << _delimiter;
        write_raw_value(value);
      }
    }

    /**
     * Writes an optional value to CSV if the field is enabled.
     * Handles bool, types with to_string(), and numeric types with optional hex formatting.
     *
     * @param field The field to check if enabled.
     * @param value The optional value to write.
     * @param is_hex If true, format numeric output as hexadecimal.
     */
    template<typename T>
    void write_value(const SampleRecordingValues::Field field, const std::optional<T>& value, const bool is_hex = false)
    {
      if (this->_values.is_set(field)) {
        this->_csv_stream << _delimiter;
        if (value.has_value()) {
          write_raw_value(value.value(), is_hex);
        }
      }
    }

    /**
     * Write a value extracted from an optional object using a function.
     * If the optional is empty, writes an empty cell.
     * If the function returns an optional, it is not wrapped again.
     *
     * @param field The field to check if enabled.
     * @param opt The optional object to extract from.
     * @param func Function to apply to the contained value to get the output value.
     * @param is_hex If true, format numeric output as hexadecimal.
     */
    template<typename T, typename F>
    void write_value(const SampleRecordingValues::Field field,
                     const std::optional<T>& opt,
                     F&& func,
                     const bool is_hex = false)
    {
      using ResultType = std::invoke_result_t<F, const T&>;

      if constexpr (is_optional_v<ResultType>) {
        /// Function already returns an optional, don't wrap again.
        if (opt.has_value()) {
          write_value(field, std::forward<F>(func)(*opt), is_hex);
        } else {
          write_value(field, ResultType{ std::nullopt }, is_hex);
        }
      } else {
        /// Wrap the result in an optional.
        if (opt.has_value()) {
          write_value(field, std::make_optional<ResultType>(std::forward<F>(func)(*opt)), is_hex);
        } else {
          write_value(field, std::optional<ResultType>{ std::nullopt }, is_hex);
        }
      }
    }

    /**
     * Write a container (vector, list, etc.) as a delimited list.
     * Empty containers or nullopt write an empty cell.
     *
     * @param field The field to check if enabled.
     * @param container The optional container to write.
     * @param is_hex If true, format numeric output as hexadecimal.
     */
    template<typename T>
    void write_value(const SampleRecordingValues::Field field,
                     const std::optional<std::vector<T>>& container,
                     const bool is_hex = false)
    {
      if (this->_values.is_set(field)) {
        this->_csv_stream << _delimiter;
        if (container.has_value() && !container->empty()) {
          auto write_delimiter = false;
          for (const auto& item : *container) {
            if (!std::exchange(write_delimiter, true)) {
              this->_csv_stream << _list_delimiter;
            }
            write_raw_value(item, is_hex);
          }
        }
      }
    }

  private:
    /// Type trait to detect if a type is std::optional.
    template<typename T>
    struct is_optional : std::false_type
    {};

    template<typename T>
    struct is_optional<std::optional<T>> : std::true_type
    {};

    template<typename T>
    static constexpr bool is_optional_v = is_optional<T>::value;

    /// Type trait to detect if to_string(T) exists via ADL.
    template<typename T, typename = void>
    struct has_to_string : std::false_type
    {};

    template<typename T>
    struct has_to_string<T, std::void_t<decltype(to_string(std::declval<T>()))>> : std::true_type
    {};

    template<typename T>
    static constexpr bool has_to_string_v = has_to_string<T>::value;

    /**
     * Writes a single value to the stream without field checking or delimiter.
     * Handles bool, types with to_string(), and numeric types with hex formatting.
     *
     * @param value The value to write.
     * @param is_hex If true, format numeric output as hexadecimal.
     */
    template<typename T>
    void write_raw_value(const T& value, const bool is_hex = false)
    {
      if constexpr (std::is_same_v<T, bool>) {
        this->_csv_stream << (value ? "true" : "false");
      } else if constexpr (has_to_string_v<T>) {
        this->_csv_stream << to_string(value);
      } else {
        if (is_hex) {
          this->_csv_stream << std::hex << "0x" << value << std::dec;
        } else {
          this->_csv_stream << value;
        }
      }
    }

    /// Output stream for writing CSV data.
    std::ostream& _csv_stream;
    /// Configuration indicating which fields are included in the samples.
    const SampleRecordingValues& _values;
    /// Character used to separate CSV columns (typically ',').
    const char _delimiter;
    /// Character used to separate list elements within a single cell (typically ';').
    const char _list_delimiter;
  };
};
}
#pragma once

#include <cstdint>
#include <vector>

namespace perf::test {

/**
 * Generator for unique and zipf data sets.
 */
class DataGenerator
{
public:
  [[nodiscard]] static std::vector<std::uint64_t> generate_unique(std::size_t size);

private:
  [[nodiscard]] static std::vector<std::uint64_t> alphabet(std::size_t size);

  [[nodiscard]] static std::vector<double> lookup_table(double zipf_param, const std::vector<std::uint64_t>& alphabet);
};

/**
 * Benchmark accessing benchmarks in random or sequential order.
 * This is an example to demonstrate the perfcpp library.
 */
class AccessBenchmark
{
public:
  /**
   * Object sized of one cache line.
   */
  struct alignas(64U) cache_line
  {
    cache_line() noexcept = default;
    explicit cache_line(const std::uint64_t value_) noexcept
      : value(value_)
    {
    }
    ~cache_line() noexcept = default;

    std::uint64_t value;
  };

  AccessBenchmark(bool is_random, std::uint64_t access_data_size_in_mb, bool is_write = false);
  ~AccessBenchmark() = default;

  /**
   * @return Number of cache lines.
   */
  [[nodiscard]] std::size_t size() const noexcept { return _indices.size(); }

  /**
   * Grant access to the i-th cache line, considering the defined access order.
   *
   * @param index Index of the cache line to access.
   * @return Cache line.
   */
  [[nodiscard]] const cache_line& operator[](const std::size_t index) const noexcept
  {
    return _data_to_read[_indices[index]];
  }

  void set(const std::size_t index, const std::uint64_t value) { _data_to_write[_indices[index]].value = value; }

  void run();

  [[nodiscard]] const std::vector<std::uint64_t>& indices() const noexcept { return _indices; }
  [[nodiscard]] const std::vector<cache_line>& data_to_read() const noexcept { return _data_to_read; }

private:
  /// Indices, defining the order in which the memory chunk is accessed.
  std::vector<std::uint64_t> _indices;

  /// Memory chunk that is read during the benchmark.
  std::vector<cache_line> _data_to_read;

  /// Memory chunk that is written during the benchmark.
  std::vector<cache_line> _data_to_write;
};
}
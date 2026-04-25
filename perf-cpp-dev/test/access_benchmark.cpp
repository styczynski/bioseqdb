#include "access_benchmark.h"
#include <algorithm>
#include <numeric>
#include <random>

perf::test::AccessBenchmark::AccessBenchmark(const bool is_random,
                                             const std::uint64_t access_data_size_in_mb,
                                             const bool is_write)
{
  const auto count_cache_lines = (access_data_size_in_mb * 1024U * 1024U) / sizeof(cache_line);

  /// Fill the data array with some unique.
  this->_data_to_read.reserve(count_cache_lines);
  for (const auto item : DataGenerator::generate_unique(count_cache_lines)) {
    this->_data_to_read.emplace_back(item);
  }

  if (is_write) {
    this->_data_to_write.resize(count_cache_lines);
  }

  /// Create the access pattern by filling the indices and shuffle, if we want a
  /// random access pattern.
  this->_indices.resize(count_cache_lines);
  std::iota(this->_indices.begin(), this->_indices.end(), 0U);

  if (is_random) {
    std::shuffle(this->_indices.begin(), this->_indices.end(), std::mt19937{ std::random_device{}() });
  }
}

std::vector<std::uint64_t>
perf::test::DataGenerator::generate_unique(const std::size_t size)
{
  /// Create a list for the tuples.
  auto relation = std::vector<std::uint64_t>{};
  relation.reserve(size);

  /// Create tuples.
  auto generator = std::mt19937{ 864896UL };
  auto distribution = std::uniform_int_distribution<std::uint64_t>{};
  for (auto i = 0ULL; i < size; ++i) {
    relation.emplace_back(distribution(generator));
  }

  /// Shuffle the relation.
  std::shuffle(relation.begin(), relation.end(), generator);

  return relation;
}

void
perf::test::AccessBenchmark::run()
{
  const auto is_readonly = this->_data_to_read.size() != this->_data_to_write.size();
  auto value = 0ULL;

  if (is_readonly) {
    for (auto index = 0U; index < this->size(); ++index) {
      value += this->_data_to_read[this->_indices[index]].value;
    }
  } else {
    for (auto index = 0U; index < this->size(); ++index) {
      value += this->_data_to_read[this->_indices[index]].value;

      this->_data_to_write[this->_indices[index]].value = value;
    }
  }

  asm volatile(""
               : "+r,m"(value)
               :
               : "memory"); /// We do not want the compiler to optimize away
                            /// this unused value.
}
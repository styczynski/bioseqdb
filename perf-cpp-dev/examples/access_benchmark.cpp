#include "access_benchmark.h"
#include <algorithm>
#include <numeric>
#include <random>

perf::example::AccessBenchmark::AccessBenchmark(const bool is_random,
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
perf::example::DataGenerator::generate_unique(const std::size_t size)
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

std::vector<std::uint64_t>
perf::example::DataGenerator::generate_zipf(const std::size_t size,
                                            const std::size_t alphabet_size,
                                            const double zipf_param)
{
  /// Create a list for the tuples.
  auto relation = std::vector<std::uint64_t>{};
  relation.reserve(size);

  const auto alphabet = DataGenerator::alphabet(alphabet_size);
  const auto lookup_table = DataGenerator::lookup_table(zipf_param, alphabet);

  std::srand(6854686UL);
  for (auto i = 0ULL; i < size; ++i) {
    const auto random_key = static_cast<double>(std::rand()) / RAND_MAX;

    if (lookup_table[0U] >= random_key) {
      relation.emplace_back(alphabet[0U]);
    } else {
      auto left = 0ULL;
      auto right = alphabet_size - 1ULL;
      std::uint64_t mid;
      while (right - left > 1ULL) {
        mid = (left + right) / 2;
        if (lookup_table[mid] < random_key) {
          left = mid;
        } else {
          right = mid;
        }
      }

      relation.emplace_back(alphabet[right]);
    }
  }

  return relation;
}

std::vector<std::uint64_t>
perf::example::DataGenerator::alphabet(const std::size_t size)
{
  auto alphabet = std::vector<std::uint64_t>{};
  alphabet.reserve(size);

  /// Fill the alphabet.
  for (auto i = 0ULL; i < size; ++i) {
    alphabet.emplace_back(i);
  }

  /// Permute the alphabet.
  auto generator = std::mt19937{ 864896UL };
  std::shuffle(alphabet.begin(), alphabet.end(), generator);

  return alphabet;
}

std::vector<double>
perf::example::DataGenerator::lookup_table(double zipf_param, const std::vector<std::uint64_t>& alphabet)
{
  auto lookup_table = std::vector<double>{};
  lookup_table.reserve(alphabet.size());

  /// Compute scaling factor such that sum (lookup_table[i], i=1..alphabet_size) = 1.0
  auto scaling_factor = 0.0;
  for (auto i = 0ULL; i < alphabet.size(); ++i) {
    scaling_factor += 1.0 / std::pow(double(i) + 1., zipf_param);
  }

  /// Generate the lookup table.
  auto sum = 0.0;
  for (auto i = 0ULL; i < alphabet.size(); ++i) {
    sum += 1.0 / std::pow(double(i) + 1.0, zipf_param);
    lookup_table.emplace_back(sum / scaling_factor);
  }

  return lookup_table;
}
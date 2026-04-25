#include <algorithm>
#include <fstream>
#include <iterator>
#include <perfcpp/analyzer/flame_graph_generator.hpp>

std::vector<std::pair<std::vector<std::string>, std::uint64_t>>
perf::analyzer::FlameGraphGenerator::map(const std::vector<Sample>& samples)
{
  this->_trie.clear();

  /// Insert all samples into the trie.
  for (const auto& sample : samples) {
    this->_trie.insert(FlameGraphGenerator::build_callchain(sample));
  }

  /// Collect all unique stacks with their counts.
  auto result = std::vector<std::pair<std::vector<std::string>, std::uint64_t>>{};
  this->_trie.for_each_stack([&result](const auto& stack, const auto count) { result.emplace_back(stack, count); });

  return result;
}

std::vector<std::pair<std::vector<std::string>, std::uint64_t>>
perf::analyzer::FlameGraphGenerator::map(
  const std::vector<Sample>& samples,
  std::function<std::uint64_t(std::vector<Sample>::const_iterator, std::vector<Sample>::const_iterator)>&& mapper)
{
  this->_trie.clear();

  /// Insert all samples and collect their leaf node IDs.
  auto leaf_ids = std::vector<perf::util::CallchainTrie::node_id_t>{};
  leaf_ids.reserve(samples.size());
  for (const auto& sample : samples) {
    leaf_ids.push_back(this->_trie.insert(FlameGraphGenerator::build_callchain(sample)));
  }

  /// Group consecutive samples with the same leaf ID and apply the mapper.
  auto result = std::vector<std::pair<std::vector<std::string>, std::uint64_t>>{};
  for (auto i = std::size_t{ 0U }; i < samples.size();) {
    auto j = i + 1U;
    while (j < samples.size() && leaf_ids[j] == leaf_ids[i]) {
      ++j;
    }

    const auto weight =
      mapper(samples.begin() + static_cast<std::ptrdiff_t>(i), samples.begin() + static_cast<std::ptrdiff_t>(j));
    result.emplace_back(this->_trie.path(leaf_ids[i]), weight);
    i = j;
  }

  return result;
}

void
perf::analyzer::FlameGraphGenerator::map(const std::vector<Sample>& samples, const std::string_view out_file_path)
{
  this->map(samples, [](const auto begin, const auto end) { return std::distance(begin, end) + 1U; }, out_file_path);
}

void
perf::analyzer::FlameGraphGenerator::map(
  const std::vector<Sample>& samples,
  std::function<std::uint64_t(std::vector<Sample>::const_iterator, std::vector<Sample>::const_iterator)> mapper,
  const std::string_view out_file_path)
{
  /// Get the stacks.
  const auto stacks = this->map(samples, std::move(mapper));

  auto out_file = std::ofstream{ std::string{ out_file_path }, std::ios_base::trunc };

  /// Write every symbol in the stack to the file.
  for (const auto& [symbols, count] : stacks) {
    for (auto index = 0U; index < symbols.size(); ++index) {
      if (index > 0U) {
        out_file << ';';
      }

      out_file << symbols[index];
    }

    out_file << " " << count << '\n';
  }

  out_file << std::flush;
}

std::vector<std::uintptr_t>
perf::analyzer::FlameGraphGenerator::build_callchain(const perf::Sample& sample)
{
  auto result = std::vector<std::uintptr_t>{};

  /// Reverse perf's leaf-to-root callchain to root-to-leaf.
  if (const auto& callchain = sample.instruction_execution().callchain(); callchain.has_value()) {
    result.reserve(callchain->size() + 1U);
    result.assign(callchain->rbegin(), callchain->rend());
  }

  /// Append the logical instruction pointer as the leaf (top of stack).
  if (const auto ip = sample.instruction_execution().logical_instruction_pointer(); ip.has_value()) {
    result.push_back(*ip);
  }

  return result;
}

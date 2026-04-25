#pragma once
#include <cstdint>
#include <functional>
#include <perfcpp/sample/sample.hpp>
#include <perfcpp/util/callchain_trie.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace perf::analyzer {
class FlameGraphGenerator
{
public:
  FlameGraphGenerator() = default;
  FlameGraphGenerator(const FlameGraphGenerator&) = default;
  FlameGraphGenerator(FlameGraphGenerator&&) noexcept = default;
  ~FlameGraphGenerator() = default;
  FlameGraphGenerator& operator=(const FlameGraphGenerator&) = default;
  FlameGraphGenerator& operator=(FlameGraphGenerator&&) noexcept = default;

  [[nodiscard]] std::vector<std::pair<std::vector<std::string>, std::uint64_t>> map(const std::vector<Sample>& samples);

  [[nodiscard]] std::vector<std::pair<std::vector<std::string>, std::uint64_t>> map(
    const std::vector<Sample>& samples,
    std::function<std::uint64_t(std::vector<Sample>::const_iterator begin, std::vector<Sample>::const_iterator end)>&&
      mapper);

  void map(const std::vector<Sample>& samples, std::string_view out_file_path);

  void map(const std::vector<Sample>& samples,
           std::function<std::uint64_t(std::vector<Sample>::const_iterator begin,
                                       std::vector<Sample>::const_iterator end)> mapper,
           std::string_view out_file_path);

private:
  util::CallchainTrie _trie;

  /**
   * Builds a root-to-leaf address list from a sample's callchain and instruction pointer.
   * Reverses perf's leaf-to-root callchain order and appends the logical IP as the leaf.
   *
   * @param sample The sample to extract the callchain from.
   * @return Address list in root-to-leaf order.
   */
  [[nodiscard]] static std::vector<std::uintptr_t> build_callchain(const Sample& sample);
};
}
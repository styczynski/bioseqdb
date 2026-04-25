#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <perfcpp/util/symbol_resolver.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace perf::util {

/**
 * A prefix trie for call stack deduplication and aggregation.
 * Each path from root to a node represents a unique call stack prefix.
 * Nodes track how many samples terminate at that point in the stack.
 *
 * Callchains are inserted as raw instruction pointer addresses in root-to-leaf
 * order (outermost caller first). The trie resolves addresses to symbol names
 * internally, deduplicates shared prefixes, and assigns each node a stable
 * numeric ID for external reference (e.g., Perfetto export).
 */
class CallchainTrie
{
public:
  using node_id_t = std::uint32_t;

  /**
   * A single node in the trie, representing one stack frame.
   */
  struct Node
  {
    /// Symbol name for this stack frame (or hex address if unresolvable).
    std::string name;

    /// Parent node ID, or std::nullopt for top-level frames.
    std::optional<node_id_t> parent_id;

    /// Number of samples terminating at this node.
    std::uint64_t count{ 0U };

    /// Children indexed by symbol name.
    std::unordered_map<std::string, node_id_t> children;
  };

  CallchainTrie();

  /**
   * Inserts a callchain into the trie and increments the leaf node's count.
   * Each address is resolved to a symbol name via the internal resolver;
   * unresolvable addresses are represented as hex strings.
   * The callchain must be in root-to-leaf order (outermost caller first).
   *
   * @param callchain Instruction pointer addresses from root to leaf.
   * @return ID of the leaf node.
   */
  node_id_t insert(const std::vector<std::uintptr_t>& callchain);

  /**
   * Returns the number of nodes, excluding the virtual root.
   *
   * @return Number of real nodes.
   */
  [[nodiscard]] std::size_t size() const noexcept { return _nodes.size() - 1U; }

  /**
   * Returns true if the trie contains no real nodes.
   *
   * @return True if empty.
   */
  [[nodiscard]] bool empty() const noexcept { return _nodes.size() <= 1U; }

  /**
   * Accesses a node by its ID.
   * Valid IDs range from 1 to size() inclusive.
   *
   * @param id Node ID.
   * @return Reference to the node.
   */
  [[nodiscard]] const Node& node(const node_id_t id) const noexcept { return _nodes[id]; }

  /**
   * Removes all nodes from the trie but preserves the internal symbol resolver and its cache.
   */
  void clear() noexcept;

  /**
   * Returns the root-to-leaf symbol path for a given node.
   *
   * @param leaf_id ID of the node to trace back to the root.
   * @return Vector of symbol names from root to the given node.
   */
  [[nodiscard]] std::vector<std::string> path(node_id_t leaf_id) const;

  /**
   * Visits every root-to-node path where the sample count is greater than zero.
   * Useful for flamegraph-style output (e.g., "main;foo;bar 42").
   *
   * @param visitor Callback receiving the stack (root-to-leaf names) and the sample count.
   */
  void for_each_stack(std::function<void(const std::vector<std::string>&, std::uint64_t)>&& visitor) const;

private:
  /// Index of the virtual root node.
  static constexpr node_id_t ROOT_ID = 0U;

  /// Flat storage of all nodes. Index 0 is the virtual root.
  std::vector<Node> _nodes;

  /// Resolver for translating instruction pointer addresses to symbol names.
  SymbolResolver _symbol_resolver;

  /**
   * Resolves an instruction pointer to a symbol name, falling back to a hex string.
   *
   * @param address Instruction pointer address.
   * @return Resolved symbol name or hex representation.
   */
  [[nodiscard]] std::string resolve_name(std::uintptr_t address);

  /**
   * Recursive DFS helper for the public for_each_stack.
   *
   * @param node_id Current node being visited.
   * @param path Accumulated call stack from root to the current node.
   * @param visitor Callback to invoke for nodes with count > 0.
   */
  void for_each_stack(node_id_t node_id,
                      std::vector<std::string>& path,
                      const std::function<void(const std::vector<std::string>&, std::uint64_t)>& visitor) const;
};
}
#pragma once

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace perf::util {
template<typename N>
class DirectedGraph
{
public:
  DirectedGraph() = default;
  DirectedGraph(const DirectedGraph&) = default;
  DirectedGraph(DirectedGraph&&) noexcept = default;
  ~DirectedGraph() = default;
  DirectedGraph& operator=(const DirectedGraph&) = default;
  DirectedGraph& operator=(DirectedGraph&&) noexcept = default;

  /**
   * Inserts a node into the graph.
   *
   * @param node Node to insert.
   */
  void insert(const N& node) { _nodes_and_edges.insert(std::make_pair(node, std::unordered_set<N>())); }

  /**
   * Creates an edge between the node and the successor.
   *
   * @param node Starting node.
   * @param successor Ending node.
   */
  void connect(const N& node, const N& successor)
  {
    /// Insert the successor node, if the node does not exist in the graph.
    if (_nodes_and_edges.find(successor) == _nodes_and_edges.end()) {
      insert(successor);
    }

    /// If the node exists, add the successor into the successor set.
    if (auto iterator = _nodes_and_edges.find(node); iterator != _nodes_and_edges.end()) {
      iterator->second.insert(std::move(successor));
    }

    /// Otherwise, create a new node with the successor.
    else {
      _nodes_and_edges.insert(std::make_pair(node, std::unordered_set<N>{ successor }));
    }
  }

  /**
   * @return True, when the graph is empty.
   */
  [[nodiscard]] bool empty() const noexcept { return _nodes_and_edges.empty(); }

  /**
   * @return The first node that has no incoming edge.
   */
  [[nodiscard]] std::optional<N> pop()
  {
    for (auto& [node, _] : _nodes_and_edges) {
      /// Check every node if the node is not a successor.
      if (!this->is_successor(node)) {

        const auto node_without_successor = node;

        /// If the node is not a successor, remove the node and return it.
        this->erase(node);

        return node_without_successor;
      }
    }

    return std::nullopt;
  }

  /**
   * Checks if the directed graph contains a cycle.
   * Uses DFS with three-color approach for optimal O(V + E) performance.
   *
   * @return True if the graph has a cycle, false otherwise.
   */
  [[nodiscard]] bool is_cyclic() const noexcept
  {
    if (empty()) {
      return false;
    }

    // Three-color DFS: 0 = white (unvisited), 1 = gray (in current path), 2 = black (finished)
    auto node_color = std::unordered_map<N, std::uint8_t>{};

    // Initialize all nodes as white
    std::transform(_nodes_and_edges.begin(),
                   _nodes_and_edges.end(),
                   std::inserter(node_color, node_color.end()),
                   [](const auto& node_and_edges) { return std::make_pair(std::get<0>(node_and_edges), 0U); });

    // Check each unvisited node
    return std::any_of(
      _nodes_and_edges.begin(), _nodes_and_edges.end(), [this, &node_color](const auto& node_and_edges) {
        const auto& node = std::get<0>(node_and_edges);
        return node_color[node] == 0U && this->dfs_has_cycle(node, node_color);
      });
  }

private:
  /// Map of nodes and their successors.
  std::unordered_map<N, std::unordered_set<N>> _nodes_and_edges;

  /**
   * Checks if the node is in any successor list.
   *
   * @param node Node to check.
   * @return True, of the node is in any successor list.
   */
  [[nodiscard]] bool is_successor(const N& node) const noexcept
  {
    return std::any_of(_nodes_and_edges.begin(), _nodes_and_edges.end(), [&node](const auto& node_and_edges) {
      const auto& successors = std::get<1>(node_and_edges);
      return successors.find(node) != successors.end();
    });
  }

  /**
   * Removes the node from the graph.
   *
   * @param node Node to remove.
   */
  void erase(const N& node) { _nodes_and_edges.erase(node); }

  /**
   * DFS helper function for cycle detection.
   *
   * @param node Current node being explored.
   * @param node_color Color map tracking node states.
   * @return True if a cycle is found during this DFS traversal.
   */
  [[nodiscard]] bool dfs_has_cycle(const N& node, std::unordered_map<N, std::uint8_t>& node_color) const noexcept
  {
    // Mark current node as gray (in current path)
    node_color[node] = 1U;

    // Find the node's successors
    if (const auto iterator = _nodes_and_edges.find(node); iterator != _nodes_and_edges.end()) {
      for (const auto& successor : iterator->second) {
        if (node_color[successor] == 1U) {
          // Found a back edge (gray node) - cycle detected
          return true;
        }
        if (node_color[successor] == 0U && dfs_has_cycle(successor, node_color)) {
          // Recursively check unvisited successors
          return true;
        }
      }
    }

    // Mark current node as black (finished processing)
    node_color[node] = 2U;
    return false;
  }
};
}
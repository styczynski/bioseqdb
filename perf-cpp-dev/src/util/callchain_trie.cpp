#include <algorithm>
#include <perfcpp/util/callchain_trie.hpp>
#include <sstream>

perf::util::CallchainTrie::CallchainTrie()
{
  /// Reserve the virtual root node at index 0.
  this->_nodes.emplace_back(Node{ "", std::nullopt, 0U, {} });
}

perf::util::CallchainTrie::node_id_t
perf::util::CallchainTrie::insert(const std::vector<std::uintptr_t>& callchain)
{
  auto current_id = ROOT_ID;

  for (const auto address : callchain) {
    /// Resolve the address to a symbol name.
    auto name = this->resolve_name(address);

    /// Look up existing child by symbol name.
    const auto iterator = this->_nodes[current_id].children.find(name);

    if (iterator != this->_nodes[current_id].children.end()) {
      /// Node already exists for this symbol.
      current_id = iterator->second;
    } else {
      /// Create a new child node.
      const auto new_id = static_cast<node_id_t>(this->_nodes.size());
      const auto parent_id = (current_id == ROOT_ID) ? std::nullopt : std::optional{ current_id };

      /// Note: push_back may reallocate _nodes, so we must not hold references
      /// to elements across this call.
      this->_nodes.push_back(Node{ name, parent_id, 0U, {} });
      this->_nodes[current_id].children.emplace(std::move(name), new_id);
      current_id = new_id;
    }
  }

  /// Increment sample count at the leaf.
  ++this->_nodes[current_id].count;

  return current_id;
}

std::string
perf::util::CallchainTrie::resolve_name(const std::uintptr_t address)
{
  if (auto resolved = this->_symbol_resolver.resolve(address); resolved.has_value()) {
    return resolved->symbol().name();
  }

  /// Fall back to hex representation.
  auto stream = std::stringstream{};
  stream << "0x" << std::hex << address;
  return stream.str();
}

void
perf::util::CallchainTrie::clear() noexcept
{
  this->_nodes.clear();
  this->_nodes.emplace_back(Node{ "", std::nullopt, 0U, {} });
}

std::vector<std::string>
perf::util::CallchainTrie::path(const node_id_t leaf_id) const
{
  auto result = std::vector<std::string>{};

  /// Walk from leaf to root, collecting names.
  for (auto id = leaf_id; id != ROOT_ID;) {
    const auto& current_node = this->_nodes[id];
    result.push_back(current_node.name);

    if (current_node.parent_id.has_value()) {
      id = *current_node.parent_id;
    } else {
      break;
    }
  }

  /// Reverse to get root-to-leaf order.
  std::reverse(result.begin(), result.end());
  return result;
}

void
perf::util::CallchainTrie::for_each_stack(
  std::function<void(const std::vector<std::string>&, std::uint64_t)>&& visitor) const
{
  auto path = std::vector<std::string>{};
  this->for_each_stack(ROOT_ID, path, visitor);
}

void
perf::util::CallchainTrie::for_each_stack(
  const node_id_t node_id,
  std::vector<std::string>& path,
  const std::function<void(const std::vector<std::string>&, std::uint64_t)>& visitor) const
{
  const auto& current_node = this->_nodes[node_id];

  /// Add current node to path (skip virtual root).
  if (node_id != ROOT_ID) {
    path.push_back(current_node.name);
  }

  /// Emit this stack if samples terminated here.
  if (current_node.count > 0U) {
    visitor(path, current_node.count);
  }

  /// Recurse into children.
  for (const auto& [child_name, child_id] : current_node.children) {
    this->for_each_stack(child_id, path, visitor);
  }

  /// Restore path.
  if (node_id != ROOT_ID) {
    path.pop_back();
  }
}

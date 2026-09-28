/**
 * @file rmq.cppm
 * @brief Find chaining predecessors with an AVL range-minimum tree.
 *
 * Nodes are ordered by query  position and anchor index. Each subtree remembers
 * its lowest-priority node, letting a query reuse whole subtrees when looking
 * for a promising predecessor within a query interval.
 *
 * Reference-distance and size limits reqove old nodes. An optional second tree
 * keeps a shorter, window for scoring nearby alternatives.
 */
module;

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <libassert/assert.hpp>
#include <limits>
#include <optional>
#include <stdexcept>
#include <span>
#include <vector>

export module bioseq.chaining.rmq;

import bioseq.common.logging;
import bioseq.chaining.core;
import bioseq.common.context;
import bioseq.instrumentation;
import bioseq.common.checked_arithmetic;

export {
namespace bioseq::chaining {

/// One anchor in the AVL lookup tree, ordered by query position and index.
/// subtree_minimum points to the lowest-priority node in this node's subtree.
struct rmq_node {
    std::int64_t query_position = 0;   ///< Signed 64-bit query coordinate used  by range bounds.
    int64_t anchor_index   = 0;
    double  priority       = 0.0;  ///< Negative score adjusted for position, used to choose a promising predecessor

    std::array<rmq_node*, 2> children{nullptr, nullptr};
    rmq_node* subtree_minimum   = nullptr;
    int8_t   balance       = 0;
    uint32_t subtree_size  = 0;
};

/// Initial RMQ storage sizes. The recycled-node list may grow during chaining.
struct rmq_chaining_storage_plan {
    chaining_output_storage_plan output;
    scalar_backtracking_storage_plan backtracking;
    std::size_t forward_anchor_count;
    std::size_t nodes_per_tree;
    std::size_t initial_free_list_entries_per_tree;
    std::size_t tree_count;
    std::size_t logical_arena_scratch_bytes;
    std::size_t logical_initial_tree_bytes;
};

[[nodiscard]] constexpr std::optional<rmq_chaining_storage_plan>
derive_rmq_chaining_storage_plan(std::size_t count, const chaining_parameters& params,
                                 std::int32_t max_dist_inner = 0) noexcept {
    const auto output = derive_chaining_output_storage_plan(count, params.min_seed_count);
    const auto backtracking = derive_scalar_backtracking_storage_plan(count);
    const auto nodes = count == 0u ? std::optional<std::size_t>{0u}
                                  : bioseq::common::checked_add(count, std::size_t{64u});
    const auto arena_bytes = bioseq::common::checked_mul(count,
        2u * sizeof(std::int32_t) + sizeof(std::int64_t) + 2u * sizeof(rmq_node*));
    if (!output || !backtracking || !nodes || !arena_bytes) {
        return std::nullopt;
    }
    const std::int32_t max_dist = std::max(params.max_gap, params.bandwidth);
    const std::size_t trees = count == 0u ? 0u
        : (max_dist_inner > 0 && max_dist_inner < max_dist ? 2u : 1u);
    const auto pool_bytes = bioseq::common::checked_mul(*nodes, sizeof(rmq_node));
    const auto free_bytes = bioseq::common::checked_mul(*nodes / 4u, sizeof(rmq_node*));
    const auto total_arena = bioseq::common::checked_add(*arena_bytes,
        backtracking->logical_scratch_bytes);
    if (!pool_bytes || !free_bytes || !total_arena) {
        return std::nullopt;
    }
    const auto tree_bytes = bioseq::common::checked_add(*pool_bytes, *free_bytes);
    const auto total_tree = tree_bytes ? bioseq::common::checked_mul(*tree_bytes, trees)
                                       : std::nullopt;
    if (!total_tree) {
        return std::nullopt;
    }
    return rmq_chaining_storage_plan{*output, *backtracking, count, *nodes,
        *nodes / 4u, trees, *total_arena, *total_tree};
}

/// Compare query positions, breaking ties by anchor index.
/// @return Negative if left precedes right, zero if equal, positive otherwise.
[[nodiscard]] inline int compare_rmq_nodes(
        const rmq_node* left, const rmq_node* right) {
    if (left->query_position != right->query_position) {
        return (left->query_position < right->query_position) ? -1 : 1;
    }
    if (left->anchor_index != right->anchor_index) {
        return (left->anchor_index < right->anchor_index) ? -1 : 1;
    }
    return 0;
}

/// Cache the lowest-priority node from this node and its two subtrees.
/// Range queries can then inspect a covered subtree without visiting every node.
inline void update_subtree_minimum(rmq_node* node) {
    if (node == nullptr) return;
    node->subtree_minimum = node;
    if (node->children[0] != nullptr &&
        node->children[0]->subtree_minimum->priority < node->subtree_minimum->priority) {
        node->subtree_minimum = node->children[0]->subtree_minimum;
    }
    if (node->children[1] != nullptr &&
        node->children[1]->subtree_minimum->priority < node->subtree_minimum->priority) {
        node->subtree_minimum = node->children[1]->subtree_minimum;
    }
}

/// Update subtree_size for a node based on its children.
inline void update_subtree_size(rmq_node* node) {
    if (node == nullptr) return;
    node->subtree_size = 1;
    if (node->children[0] != nullptr) node->subtree_size += node->children[0]->subtree_size;
    if (node->children[1] != nullptr) node->subtree_size += node->children[1]->subtree_size;
}

/// AVL tree for finding chaining predecessors within a query-position range.
/// Nodes come from a preallocated pool  and erased nodes can be reused.
/// The free list itself may allocate more space as nodes are returned.
struct rmq_tree {
    rmq_node* root = nullptr;
    std::vector<rmq_node> node_pool;
    size_t next_pool_index = 0;
    std::vector<rmq_node*> free_list;

    /// Initialize a fresh tree's pool with max_nodes slots.
    void reserve(size_t max_nodes) {
        node_pool.resize(max_nodes);
        next_pool_index = 0;
        free_list.clear();
        free_list.reserve(max_nodes / 4);
    }

    /// Allocate a node from the pool or free list.
    [[nodiscard]] rmq_node* allocate_node() {
        if (!free_list.empty()) {
            rmq_node* node = free_list.back();
            free_list.pop_back();
            *node = rmq_node{};
            return node;
        }
        DEBUG_ASSERT(next_pool_index < node_pool.size(),
            "rmq_tree node pool exhausted");
        rmq_node* node = &node_pool[next_pool_index++];
        *node = rmq_node{};
        return node;
    }

    /// Return a node to the free list.
    void release_node(rmq_node* node) {
        if (node != nullptr) {
            free_list.push_back(node);
        }
    }

    /// Get the number of nodes in the tree.
    [[nodiscard]] uint32_t size() const {
        return (root != nullptr) ? root->subtree_size : 0;
    }

    // AVL rotations adapted from krmq.h

    /// Single rotation. Direction zero rotates left. One rotates right.
    static rmq_node* rotate_single(rmq_node* node, int direction) {
        rmq_node* other = node->children[1 - direction];
        node->children[1 - direction] = other->children[direction];
        other->children[direction] = node;
        update_subtree_size(node);
        update_subtree_minimum(node);
        update_subtree_size(other);
        update_subtree_minimum(other);
        return other;
    }

    /// Repair a left-right or right-left imbalance with two rotations.
    static rmq_node* rotate_double(rmq_node* node, int direction) {
        node->children[1 - direction] = rotate_single(
            node->children[1 - direction], 1 - direction);
        return rotate_single(node, direction);
    }

    // Insertion

    /// Insert new_node and rbalance the tree.
    /// Return the existing node for a duplicate key, or nullptr after insertion.
    /// The caller still owns an uninserted new_node.
    rmq_node* insert(rmq_node* new_node) {
        new_node->children[0] = new_node->children[1] = nullptr;
        new_node->balance = 0;
        new_node->subtree_size = 1;
        new_node->subtree_minimum = new_node;

        if (root == nullptr) {
            root = new_node;
            return nullptr;
        }

        // Path from root to insertion point
        std::array<rmq_node*, 64> path_nodes;
        std::array<std::uint8_t, 64> path_dirs;
        int path_depth = 0;

        rmq_node* current = root;
        while (current != nullptr) {
            const int comparison = compare_rmq_nodes(new_node, current);
            if (comparison == 0) return current;  // Duplicate
            const int direction = (comparison > 0) ? 1 : 0;
            path_nodes[path_depth] = current;
            path_dirs[path_depth] = static_cast<std::uint8_t>(direction);
            ++path_depth;
            current = current->children[direction];
        }

        // Link the new node
        path_nodes[path_depth - 1]->children[path_dirs[path_depth - 1]] = new_node;

        // Update ancestors after insertion. Balance changes stop when subtree
        // height stops changing, but every ancestor still needs a new  size
        // and minimum-priority pointer.
        bool still_propagating_balance = true;
        for (int depth = path_depth - 1; depth >= 0; --depth) {
            rmq_node* node = path_nodes[depth];
            if (!still_propagating_balance) {
                update_subtree_size(node);
                update_subtree_minimum(node);
                continue;
            }
            const int direction = path_dirs[depth];
            const int balance_delta = (direction == 0) ? -1 : 1;
            node->balance += static_cast<int8_t>(balance_delta);
            update_subtree_size(node);
            update_subtree_minimum(node);

            if (node->balance == 0) {
                // Height is unchanged above this node. Continue updating ancestor
                // sizes and minima without changing their balance factors.
                still_propagating_balance = false;
                continue;
            } else if (node->balance == -2 || node->balance == 2) {
                // Need rotation
                const int rebalance_dir = (node->balance < 0) ? 1 : 0;
                rmq_node* child = node->children[1 - rebalance_dir];
                rmq_node* rotated;
                if ((rebalance_dir == 1 && child->balance <= 0) ||
                    (rebalance_dir == 0 && child->balance >= 0)) {
                    // Single rotation
                    rotated = rotate_single(node, rebalance_dir);
                    if (child->balance == 0) {
                        node->balance = static_cast<int8_t>((rebalance_dir == 1) ? -1 : 1);
                        rotated->balance = static_cast<int8_t>((rebalance_dir == 1) ? 1 : -1);
                    } else {
                        node->balance = 0;
                        rotated->balance = 0;
                    }
                } else {
                    // Use the grandchild's old balance to set the two lower nodes'
                    // balances after the double, rotation
                    rmq_node* grandchild = child->children[rebalance_dir];
                    rotated = rotate_double(node, rebalance_dir);
                    const int8_t balance_sign = static_cast<int8_t>(
                        (rebalance_dir == 0) ? 1 : -1);
                    if (grandchild->balance == 0) {
                        node->balance  = 0;
                        child->balance = 0;
                    } else if (grandchild->balance == balance_sign) {
                        node->balance = static_cast<int8_t>(-balance_sign);
                        child->balance = 0;
                    } else {
                        node->balance  = 0;
                        child->balance = balance_sign;
                    }
                    rotated->balance = 0;
                }

                // Link rotated subtree to parent
                if (depth > 0) {
                    path_nodes[depth - 1]->children[path_dirs[depth - 1]] = rotated;
                } else {
                    root = rotated;
                }

                // Refresh cached sizes and minima above the repaired subtree.
                for (int ancestor = depth - 1; ancestor >= 0; --ancestor) {
                    update_subtree_size(path_nodes[ancestor]);
                    update_subtree_minimum(path_nodes[ancestor]);
                }
                break;
            }
        }

        return nullptr;
    }

    // Removal

    /// Remove the node with this key and rebalance the tree.
    /// Return the detached node, or nullptr when the key is absent.
    /// The caller can  pass the result to release_node() for reuse.
    rmq_node* erase(const rmq_node* key) {
        if (root == nullptr) return nullptr;

        std::array<rmq_node*, 64> path_nodes;
        std::array<std::uint8_t, 64> path_dirs;
        int path_depth = 0;

        // Find the node
        rmq_node* current = root;
        while (current != nullptr) {
            const int comparison = compare_rmq_nodes(key, current);
            if (comparison == 0) break;
            const int direction = (comparison > 0) ? 1 : 0;
            path_nodes[path_depth] = current;
            path_dirs[path_depth] = static_cast<std::uint8_t>(direction);
            ++path_depth;
            current = current->children[direction];
        }
        if (current == nullptr) return nullptr;

        rmq_node* erased = current;

        // Replace a node with two children by its next node in sorted order.
        if (current->children[0] != nullptr && current->children[1] != nullptr) {
            // Find in-order successor (leftmost in right subtree)
            const int swap_depth = path_depth;
            path_nodes[path_depth] = current;
            path_dirs[path_depth] = 1;
            ++path_depth;
            rmq_node* successor = current->children[1];
            while (successor->children[0] != nullptr) {
                path_nodes[path_depth] = successor;
                path_dirs[path_depth] = 0;
                ++path_depth;
                successor = successor->children[0];
            }

            // Move the successor into current's place by changing links.
            // Keeping each node's data intact preserves external node pointers.
            rmq_node* successor_right = successor->children[1];

            // The succesoor inherits the removed node's left subtree.
            if (path_nodes[path_depth - 1] == current) {
                // Successor is the direct right child
                successor->children[0] = current->children[0];
                successor->balance = current->balance;
                successor->subtree_size = current->subtree_size;
                // Its right child remains attached.
                path_nodes[swap_depth] = successor;
                path_dirs[swap_depth] = 1;
            } else {
                // Detach the successor from its old parent before moving it.
                path_nodes[path_depth - 1]->children[0] = successor_right;
                successor->children[0] = current->children[0];
                successor->children[1] = current->children[1];
                successor->balance = current->balance;
                successor->subtree_size = current->subtree_size;
                path_nodes[swap_depth] = successor;
            }

            // Link successor to parent
            if (swap_depth > 0) {
                path_nodes[swap_depth - 1]->children[path_dirs[swap_depth - 1]] = successor;
            } else {
                root = successor;
            }

        } else {
            // Replace a node with at most one child by that child, or nullptr.
            rmq_node* replacement = (current->children[0] != nullptr)
                ? current->children[0] : current->children[1];
            if (path_depth > 0) {
                path_nodes[path_depth - 1]->children[path_dirs[path_depth - 1]] = replacement;
            } else {
                root = replacement;
            }
        }

        // Repair  balance factors while the subtree height keeps changing.
        // Every ancestor still needs an updated size and minimum, even after
        // its height stops changing.
        bool still_propagating_balance = true;
        for (int depth = path_depth - 1; depth >= 0; --depth) {
            rmq_node* node = path_nodes[depth];
            if (!still_propagating_balance) {
                update_subtree_size(node);
                update_subtree_minimum(node);
                continue;
            }
            const int direction = path_dirs[depth];
            const int balance_delta = (direction == 0) ? 1 : -1;
            node->balance += static_cast<int8_t>(balance_delta);
            update_subtree_size(node);
            update_subtree_minimum(node);

            if (node->balance == -1 || node->balance == 1) {
                // Height is unchanged above this node. Ancestor sizes and minima
                // still need updating.
                still_propagating_balance = false;
                continue;
            } else if (node->balance == -2 || node->balance == 2) {
                const int rebalance_dir = (node->balance < 0) ? 1 : 0;
                rmq_node* child = node->children[1 - rebalance_dir];
                rmq_node* rotated;
                bool height_changed = true;

                if ((rebalance_dir == 1 && child->balance <= 0) ||
                    (rebalance_dir == 0 && child->balance >= 0)) {
                    rotated = rotate_single(node, rebalance_dir);
                    if (child->balance == 0) {
                        node->balance = static_cast<int8_t>((rebalance_dir == 1) ? -1 : 1);
                        rotated->balance = static_cast<int8_t>((rebalance_dir == 1) ? 1 : -1);
                        height_changed = false;
                    } else {
                        node->balance = 0;
                        rotated->balance = 0;
                    }
                } else {
                    // Restore the lower nodes' balances using the grandchild's old balance.
                    rmq_node* grandchild = child->children[rebalance_dir];
                    rotated = rotate_double(node, rebalance_dir);
                    const int8_t balance_sign = static_cast<int8_t>(
                        (rebalance_dir == 0) ? 1 : -1);
                    if (grandchild->balance == 0) {
                        node->balance  = 0;
                        child->balance = 0;
                    } else if (grandchild->balance == balance_sign) {
                        node->balance = static_cast<int8_t>(-balance_sign);
                        child->balance = 0;
                    } else {
                        node->balance  = 0;
                        child->balance = balance_sign;
                    }
                    rotated->balance = 0;
                }

                if (depth > 0) {
                    path_nodes[depth - 1]->children[path_dirs[depth - 1]] = rotated;
                } else {
                    root = rotated;
                }

                if (!height_changed) {
                    // This rotation restores the old height. Only ancestor sizes and
                    // minima need updating from here.
                    still_propagating_balance = false;
                }
                // Otherwise the shorter subtree still affects its parent's balance
            }
        }

        erased->children[0] = erased->children[1] = nullptr;
        return erased;
    }

    // Range-minimum lookup

    /// Find the minimum-priority node in the inclusive key range.
    ///
    /// Uses cached subtree minima to skip fully covered branches.
    /// Returns nullptr if no node falls within the range.
    [[nodiscard]] const rmq_node* find_minimum_in_range(
        const rmq_node* lower_bound,
        const rmq_node* upper_bound) const {
        if (root == nullptr) return nullptr;

        // Find the split node where the bounds diverge.
        const rmq_node* split = root;
        while (split != nullptr) {
            if (compare_rmq_nodes(upper_bound, split) < 0) {
                split = split->children[0];
            } else if (compare_rmq_nodes(lower_bound, split) > 0) {
                split = split->children[1];
            } else {
                break;
            }
        }
        if (split == nullptr) return nullptr;

        // Check if the split node itself is in range
        const rmq_node* best = nullptr;
        if (compare_rmq_nodes(lower_bound, split) <= 0
            && compare_rmq_nodes(split, upper_bound) <= 0) {
            best = split;
        }

        // Walk the left subtree and collect nodes at or above the lower bound.
        const rmq_node* left_walk = split->children[0];
        while (left_walk != nullptr) {
            if (compare_rmq_nodes(lower_bound, left_walk) <= 0) {
                // left_walk and its right subtree satisfy the lower bound.
                if (best == nullptr || left_walk->priority < best->priority) {
                    best = left_walk;
                }
                if (left_walk->children[1] != nullptr &&
                    left_walk->children[1]->subtree_minimum->priority <
                        (best ? best->priority : left_walk->priority)) {
                    best = left_walk->children[1]->subtree_minimum;
                }
                left_walk = left_walk->children[0];
            } else {
                left_walk = left_walk->children[1];
            }
        }

        // Walk the right subtree and collect nodes at or below the upper bound.
        const rmq_node* right_walk = split->children[1];
        while (right_walk != nullptr) {
            if (compare_rmq_nodes(right_walk, upper_bound) <= 0) {
                // right_walk and its left subtree satisfy the upper bound.
                if (best == nullptr || right_walk->priority < best->priority) {
                    best = right_walk;
                }
                if (right_walk->children[0] != nullptr &&
                    right_walk->children[0]->subtree_minimum->priority <
                        (best ? best->priority : right_walk->priority)) {
                    best = right_walk->children[0]->subtree_minimum;
                }
                right_walk = right_walk->children[1];
            } else {
                right_walk = right_walk->children[0];
            }
        }

        return best;
    }

    /// Walk candidate nodes in decreasing  key order for the shorter-window search.
    struct reverse_iterator {
        const rmq_node* stack_storage[64];
        const rmq_node** stack_top;
        const rmq_node* current_node = nullptr;

        /** @brief Creates an unpositioned cursor with empty traversal state. */
        reverse_iterator() : stack_top(stack_storage) {}

        /// Position the reverse_iterator at the largest node <= key.
        void seek_at_most(const rmq_node* tree_root, const rmq_node* key) {
            stack_top = stack_storage;
            current_node = nullptr;
            const rmq_node* node = tree_root;
            while (node != nullptr) {
                const int comparison = compare_rmq_nodes(key, node);
                if (comparison == 0) {
                    current_node = node;
                    return;
                } else if (comparison > 0) {
                    current_node = node;
                    *stack_top++ = node;
                    node = node->children[1];
                } else {
                    *stack_top++ = node;
                    node = node->children[0];
                }
            }
        }

        /// Move to the previous node in sorted order.
        /// @return True when a node is available, false at the beginning of the tree.
        bool move_previous() {
            if (current_node == nullptr) return false;

            // If left child exists, go to rightmost node in left subtree
            if (current_node->children[0] != nullptr) {
                const rmq_node* node = current_node->children[0];
                while (node->children[1] != nullptr) {
                    *stack_top++ = node;
                    node = node->children[1];
                }
                current_node = node;
                return true;
            }

            // Find the nearest ancestor that precedes the current node.
            while (stack_top > stack_storage) {
                const rmq_node* parent = *(--stack_top);
                if (parent->children[1] == current_node ||
                    compare_rmq_nodes(parent, current_node) < 0)
                {
                    current_node = parent;
                    return true;
                }
                current_node = parent;
            }

            current_node = nullptr;
            return false;
        }

        /// Return the current node, or nullptr when the cursor is exhauited.
        [[nodiscard]] const rmq_node* current() const {
            return current_node;
        }
    };
};

// RMQ chaining

/// Chain anchors with a tree of candidate predecessors.
/// Only earlier reference positions qualify. Remove anchors outside the
/// reference window and score, the lowest-priority node in the inclusive  query
/// interval. If that link leaves a gap or changes diagonal, an optional shorter
/// window checks nearby alternatives.
/// RMQ scoring allows equal query positions and imposes no diagonal bandwidth
/// limit. Keep the caller's chaining arena alive for both output slabs.
///
/// @param anchors Anchors in canonical_anchor_less order.
/// @param params Settings with prepare() already called.
/// @param chains_out Receives chains in endpoint-processing order.
/// @param reordered_anchors_out Receives each chain's anchors in forward order.
/// @param max_dist_inner Shorter reference/query window, or zero to disable it.
/// @param cap_rmq_size Limit on primary-tree nodes, enforced by removing oldest anchors.
void chain_anchors_rmq(
    std::span<const anchor> anchors,
    const chaining_parameters& params,
    arena_slab<chain>& chains_out,
    arena_slab<anchor>& reordered_anchors_out,
    int32_t max_dist_inner = 0,
    int32_t cap_rmq_size = 100000)
{
    static const auto* rmq_instrumentation_descriptor =
        bioseq::instrumentation::register_static_region("mg_lchain_rmq");
    auto rmq_instrumentation_scope = bioseq::instrumentation::region(rmq_instrumentation_descriptor);
    const auto storage_plan = derive_rmq_chaining_storage_plan(
        anchors.size(), params, max_dist_inner);
    if (!storage_plan) {
        throw std::length_error{"RMQ chaining storage exceeds its addressable domain"};
    }
    const auto scratch_count = storage_plan->forward_anchor_count;
    const int64_t anchor_count = static_cast<int64_t>(scratch_count);
    if (anchor_count == 0) return;

    int32_t max_dist = params.max_gap;
    if (max_dist < params.bandwidth) max_dist = params.bandwidth;
    if (max_dist_inner <= 0 || max_dist_inner >= max_dist) {
        max_dist_inner = 0;
    }

    // Keep scoring arrays in the chaining arena. A predecessor of -1
    // means that the anchor starts a chain.
    auto dp_scope = context::current().push<context::arena::chain>();
    auto& dp_ctx = context::current();
    auto best_scores  = dp_ctx.alloc_chain_array<int32_t>(scratch_count);
    auto predecessors = dp_ctx.alloc_chain_array<int64_t>(scratch_count);
    auto peak_scores  = dp_ctx.alloc_chain_array<int32_t>(scratch_count);
    std::memset(best_scores.data(),  0,
                static_cast<std::size_t>(anchor_count) * sizeof(int32_t));
    std::memset(predecessors.data(), 0xff,
                static_cast<std::size_t>(anchor_count) * sizeof(int64_t));
    std::memset(peak_scores.data(),  0,
                static_cast<std::size_t>(anchor_count) * sizeof(int32_t));

    // Prepare the primary tree and the  optional shorter-window tree
    rmq_tree primary_tree;
    primary_tree.reserve(storage_plan->nodes_per_tree);
    rmq_tree inner_tree;
    if (max_dist_inner > 0) {
        inner_tree.reserve(storage_plan->nodes_per_tree);
    }

    // Remember which reference groups have been inserted and removed.
    int64_t group_start = 0;
    int64_t prune_start = 0;
    int64_t prune_start_inner = 0;

    // Keep node pointers by anchor index for removal. This reset assumes
    // that supported platforms represent a null pointer with jero bytes.
    auto primary_nodes = dp_ctx.alloc_chain_array<rmq_node*>(
        scratch_count);
    auto inner_nodes   = dp_ctx.alloc_chain_array<rmq_node*>(
        scratch_count);
    std::memset(primary_nodes.data(), 0,
                static_cast<std::size_t>(anchor_count) * sizeof(rmq_node*));
    std::memset(inner_nodes.data(),   0,
                static_cast<std::size_t>(anchor_count) * sizeof(rmq_node*));

    {
    static const auto* forward_instrumentation_descriptor =
        bioseq::instrumentation::register_static_loop("mg_lchain_rmq_forward");
    auto forward_instrumentation_scope = bioseq::instrumentation::loop(forward_instrumentation_descriptor);
    for (int64_t current_index = 0;
         current_index < anchor_count;
         ++current_index) {
        forward_instrumentation_scope.record_iteration();
        int32_t best_score =
            static_cast<int32_t>(anchors[current_index].query_span);
        int64_t best_predecessor = -1;

        // Insert the completed reference group. Delaying insertion prevents
        // links between anchors at the same reference position.
        if (group_start < current_index &&
            anchors[group_start].reference_sort_key()
                != anchors[current_index].reference_sort_key())
        {
            for (int64_t predecessor_index = group_start;
                 predecessor_index < current_index;
                 ++predecessor_index) {
                rmq_node* node = primary_tree.allocate_node();
                node->query_position = static_cast<std::int64_t>(
                    anchors[predecessor_index].query_position);
                node->anchor_index = predecessor_index;
                node->priority = -(
                    static_cast<double>(best_scores[predecessor_index])
                    + 0.5 * static_cast<double>(params.gap_penalty)
                    * (static_cast<double>(
                            anchors[predecessor_index].ref_position)
                     + static_cast<double>(
                            anchors[predecessor_index].query_position)));
                primary_tree.insert(node);
                primary_nodes[predecessor_index] = node;

                if (max_dist_inner > 0) {
                    rmq_node* inner_node = inner_tree.allocate_node();
                    inner_node->query_position = node->query_position;
                    inner_node->anchor_index = predecessor_index;
                    inner_node->priority = node->priority;
                    inner_tree.insert(inner_node);
                    inner_nodes[predecessor_index] = inner_node;
                }
            }
            group_start = current_index;
        }

        // Remove anchors from other strands or outside the reference window.
        while (prune_start < current_index) {
            if (anchors[current_index].strand_partition_key()
                    != anchors[prune_start].strand_partition_key()
                || anchors[current_index].reference_sort_key()
                    > anchors[prune_start].reference_sort_key()
                    + static_cast<uint64_t>(max_dist))
            {
                if (primary_nodes[prune_start] != nullptr) {
                    rmq_node* erased = primary_tree.erase(primary_nodes[prune_start]);
                    if (erased != nullptr) primary_tree.release_node(erased);
                    primary_nodes[prune_start] = nullptr;
                }
                ++prune_start;
            } else {
                break;
            }
        }

        // Remove oldest anchors until the primary tree fits its size limit.
        if (primary_tree.size() > static_cast<uint32_t>(cap_rmq_size)) {
            while (prune_start < current_index &&
                   primary_tree.size() > static_cast<uint32_t>(cap_rmq_size))
            {
                if (primary_nodes[prune_start] != nullptr) {
                    rmq_node* erased = primary_tree.erase(primary_nodes[prune_start]);
                    if (erased != nullptr) primary_tree.release_node(erased);
                    primary_nodes[prune_start] = nullptr;
                }
                ++prune_start;
            }
        }

        // Apply the shorter reference  window to the inner tree.
        if (max_dist_inner > 0) {
            while (prune_start_inner < current_index) {
                if (anchors[current_index].strand_partition_key()
                        != anchors[prune_start_inner].strand_partition_key()
                    || anchors[current_index].reference_sort_key()
                        > anchors[prune_start_inner].reference_sort_key()
                        + static_cast<uint64_t>(max_dist_inner))
                {
                    if (inner_nodes[prune_start_inner] != nullptr) {
                        rmq_node* erased = inner_tree.erase(inner_nodes[prune_start_inner]);
                        if (erased != nullptr) inner_tree.release_node(erased);
                        inner_nodes[prune_start_inner] = nullptr;
                    }
                    ++prune_start_inner;
                } else {
                    break;
                }
            }
        }

        // Choose a candidate from the inclusive query-position interval.
        if (primary_tree.size() > 0) {
            const std::int64_t current_query_pos = static_cast<std::int64_t>(
                anchors[current_index].query_position);
            rmq_node lower_key;
            lower_key.query_position = current_query_pos - max_dist;
            lower_key.anchor_index = std::numeric_limits<int64_t>::min();
            rmq_node upper_key;
            upper_key.query_position = current_query_pos;
            upper_key.anchor_index = std::numeric_limits<int64_t>::max();

            const rmq_node* rmq_result =
                primary_tree.find_minimum_in_range(&lower_key, &upper_key);
            if (rmq_result != nullptr) {
                const int64_t predecessor_index = rmq_result->anchor_index;
                bool is_exact = false;
                int32_t diagonal_width = 0;
                const int32_t pair_score = compute_chaining_score_simple(
                    anchors[current_index], anchors[predecessor_index],
                    params.gap_penalty, params.skip_penalty,
                    is_exact, diagonal_width);
                const int32_t total_score =
                    pair_score + best_scores[predecessor_index];
                if (total_score > best_score) {
                    best_score = total_score;
                    best_predecessor = predecessor_index;
                }

                // If the first link leaves a gap or changes diagonal, check nearby
                // alternatives in decreasing query order, subject to the skip limit.
                if (!is_exact && max_dist_inner > 0 && inner_tree.size() > 0) {
                    rmq_node inner_hi_key;
                    inner_hi_key.query_position = current_query_pos;
                    inner_hi_key.anchor_index = std::numeric_limits<int64_t>::max();

                    typename rmq_tree::reverse_iterator cursor;
                    cursor.seek_at_most(inner_tree.root, &inner_hi_key);

                    int32_t skip_count = 0;
                    const int32_t max_skip = params.max_chain_skip;
                    while (cursor.current() != nullptr) {
                        const rmq_node* candidate = cursor.current();
                        const int64_t candidate_index = candidate->anchor_index;
                        if (static_cast<std::int64_t>(
                                anchors[current_index].query_position)
                            - candidate->query_position > max_dist_inner)
                        {
                            break;
                        }
                        bool inner_exact = false;
                        int32_t inner_diag_width = 0;
                        const int32_t inner_pair_score = compute_chaining_score_simple(
                            anchors[current_index], anchors[candidate_index],
                            params.gap_penalty, params.skip_penalty,
                            inner_exact, inner_diag_width);
                        const int32_t inner_total =
                            inner_pair_score + best_scores[candidate_index];
                        if (inner_total > best_score) {
                            best_score = inner_total;
                            best_predecessor = candidate_index;
                            if (skip_count > 0) --skip_count;
                        } else {
                            if (++skip_count > max_skip) break;
                        }
                        if (inner_exact) break;
                        if (!cursor.move_previous()) break;
                    }
                }
            }
        }

        // Save the best predecessor and score for this anchor.
        best_scores[current_index] = best_score;
        predecessors[current_index] = best_predecessor;
        peak_scores[current_index] = (best_predecessor >= 0
            && peak_scores[best_predecessor] > best_score)
            ? peak_scores[best_predecessor] : best_score;
    }

    }  // end mg_lchain_rmq_forward scope
    bioseq_log<logging_level::debug>("[chaining_rmq] rmq chaining pass complete for {} anchors", anchor_count);

    static const auto* backtrack_instrumentation_descriptor =
        bioseq::instrumentation::register_static_region("backtrack_chains");
    auto backtrack_instrumentation_scope = bioseq::instrumentation::region(backtrack_instrumentation_descriptor);
    backtrack_chains(anchors,
        std::span<const int32_t>(best_scores.data(),  best_scores.size()),
        std::span<const int64_t>(predecessors.data(), predecessors.size()),
        std::span<const int32_t>(peak_scores.data(),  peak_scores.size()),
        params.min_chain_score, params.min_seed_count,
        chains_out, reordered_anchors_out);
}

}  // namespace bioseq::chaining
}  // export

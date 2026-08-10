/**
 * @file chaining_rmq.cppm
 * @brief avl+rmq tree-based O(n log n) chaining algorithm.
 *
 * Translates minimap2's mg_lchain_rmq() (lchain.c:369-521) and the krmq.h
 * avl tree macros into modern C++.
 *
 * The algorithm maintains avl trees augmented with Range Minimum Query:
 *   - Primary tree: all in-range anchors, queried with [qpos - max_dist, qpos]
 *   - Inner tree (optional): finer-granularity for refinement
 *
 * Each tree node stores a priority = -(f[j] + 0.5 * gap_penalty * (ref_pos + query_pos)).
 * The subtree_minimum pointer tracks the minimum-priority node in each subtree,
 * enabling O(log n) range-minimum queries.
 *
 * Complexity: O(n log n) — each anchor does O(log n) insert + O(log n) rmq query.
 *
 * Reference: minimap2 lchain.c, krmq.h
 */
module;

#include <libassert/assert.hpp>

#include <cstdint>
#include <vector>
#include <span>
#include <limits>
#include <algorithm>
#include <bit>
#include <cstring>

export module bioseq.chaining.rmq;

import bioseq.common.logging;
import bioseq.chaining.core;
import bioseq.common.context;
import bioseq.instrumentation;

export {
namespace bioseq::chaining {

// ============================================================================
// avl+rmq Tree Node
// ============================================================================

/// Node in the avl tree with rmq support.
///
/// Equivalent to lc_elem_t in lchain.c:338-343.
/// Keyed by (query_position, anchor_index) ascending.
/// The priority field enables rmq: each node tracks the minimum-priority
/// node in its subtree via subtree_minimum.
struct rmq_node {
    std::int64_t query_position = 0;   ///< Full-width query coordinate; never truncated.
    int64_t anchor_index   = 0;
    double  priority       = 0.0;  ///< -(best_scores[j] + 0.5 * gap_penalty * (ref_pos + query_pos))

    rmq_node* children[2]   = {nullptr, nullptr};
    rmq_node* subtree_minimum   = nullptr;
    int8_t   balance       = 0;
    uint32_t subtree_size  = 0;
};

// ============================================================================
// avl+rmq Tree
// ============================================================================

/// Compare two rmq nodes by (query_position, anchor_index) ascending.
///
/// Matches lc_elem_cmp in lchain.c:345: same query_position ties are
/// broken by anchor index in ascending order (smaller index first).
///
/// @return <0 if a < b, 0 if equal, >0 if a > b
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

/// Update the subtree_minimum pointer for a node based on its children.
///
/// The subtree_minimum is the node with the smallest priority value in the subtree.
/// This is the core augmentation that enables O(log n) range-minimum queries.
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

/// avl+rmq tree for chaining predecessor lookup.
///
/// Translated from krmq.h macro-generated avl tree with rmq augmentation.
/// Supports insert, erase, find, and range-minimum query operations.
///
/// Uses a pool allocator: nodes are pre-allocated in a vector and recycled
/// via a free list for allocation-free operation during the chaining loop.
struct rmq_tree {
    rmq_node* root = nullptr;
    std::vector<rmq_node> node_pool;
    size_t next_pool_index = 0;
    std::vector<rmq_node*> free_list;

    /// Reserve pool capacity for up to max_nodes insertions.
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

    // --- avl rotations (translated from krmq.h) ---

    /// Single rotation. Direction zero rotates left; one rotates right.
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

    /// Double rotation.
    static rmq_node* rotate_double(rmq_node* node, int direction) {
        node->children[1 - direction] = rotate_single(
            node->children[1 - direction], 1 - direction);
        return rotate_single(node, direction);
    }

    // --- Insert ---

    /// Insert a node into the tree. Returns the node if it was a duplicate (not inserted).
    ///
    /// Translated from __krmq_insert in krmq.h.
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
        rmq_node* path_nodes[64];
        uint8_t path_dirs[64];
        int path_depth = 0;

        rmq_node* current = root;
        while (current != nullptr) {
            const int comparison = compare_rmq_nodes(new_node, current);
            if (comparison == 0) return current;  // Duplicate
            const int direction = (comparison > 0) ? 1 : 0;
            path_nodes[path_depth] = current;
            path_dirs[path_depth] = static_cast<uint8_t>(direction);
            ++path_depth;
            current = current->children[direction];
        }

        // Link the new node
        path_nodes[path_depth - 1]->children[path_dirs[path_depth - 1]] = new_node;

        // Walk back up, updating sizes, mins, and rebalancing.
        //
        // Propagate AVL balance only while height changes. Subtree size and
        // minimum augmentations must still reach every ancestor after height
        // stabilizes, so the loop switches to augmentation-only mode.
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
                // Height didn't change → balance is stable up the
                // tree, but size/min still need to be updated on
                // every ancestor. Continue the loop in
                // augmentation-only mode.
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
                    // Double-rotation assignments follow the post-rotation
                    // grandchild balance exactly; parent and child receive
                    // complementary values before the new root is cleared.
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

                // Update sizes and mins for ancestors above the rotation
                for (int ancestor = depth - 1; ancestor >= 0; --ancestor) {
                    update_subtree_size(path_nodes[ancestor]);
                    update_subtree_minimum(path_nodes[ancestor]);
                }
                break;
            }
        }

        return nullptr;
    }

    // --- Erase ---

    /// Find and erase a node matching the given key.
    ///
    /// Returns the erased node (caller should release_node it) or nullptr if not found.
    /// Translated from __krmq_erase in krmq.h.
    rmq_node* erase(const rmq_node* key) {
        if (root == nullptr) return nullptr;

        rmq_node* path_nodes[64];
        uint8_t path_dirs[64];
        int path_depth = 0;

        // Find the node
        rmq_node* current = root;
        while (current != nullptr) {
            const int comparison = compare_rmq_nodes(key, current);
            if (comparison == 0) break;
            const int direction = (comparison > 0) ? 1 : 0;
            path_nodes[path_depth] = current;
            path_dirs[path_depth] = static_cast<uint8_t>(direction);
            ++path_depth;
            current = current->children[direction];
        }
        if (current == nullptr) return nullptr;

        rmq_node* erased = current;

        // If node has two children, swap with in-order successor
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

            // Replace current's position in the tree with successor
            // Copy successor's data into current, then remove successor
            // Actually, we swap the tree links to avoid data copy issues
            rmq_node* successor_right = successor->children[1];

            // Successor takes current's children (except if successor is direct child)
            if (path_nodes[path_depth - 1] == current) {
                // Successor is the direct right child
                successor->children[0] = current->children[0];
                successor->balance = current->balance;
                successor->subtree_size = current->subtree_size;
                // Don't set children[1], it stays
                path_nodes[swap_depth] = successor;
                path_dirs[swap_depth] = 1;
            } else {
                // Successor is deeper
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
            // Zero or one child: replace with the existing child
            rmq_node* replacement = (current->children[0] != nullptr)
                ? current->children[0] : current->children[1];
            if (path_depth > 0) {
                path_nodes[path_depth - 1]->children[path_dirs[path_depth - 1]] = replacement;
            } else {
                root = replacement;
            }
        }

        // Rebalance upward.
        //
        // As in `insert`, AVL balance updates
        // stop once height stabilises (balance settles to ±1, or a
        // rotation restores the pre-erase height), but the
        // subtree_size / subtree_minimum augmentations must continue
        // walking to the root because we removed a node from every
        // ancestor's subtree.
        //
        // Continue augmentation updates after height propagation stops so the
        // root always retains the exact subtree size and range minimum.
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
                // Height didn't change → balance is stable upward,
                // but every ancestor still needs size/min updates.
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
                    // Double rotation uses the same complementary balance
                    // assignments as insertion before clearing the new root.
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
                    // Rotation absorbed the height change. Balance is
                    // stable upward; only augmentations remain.
                    still_propagating_balance = false;
                }
                // If height *did* change (the rotation propagated the
                // shrink upward), balance work continues at depth-1.
            }
        }

        erased->children[0] = erased->children[1] = nullptr;
        return erased;
    }

    // --- Range Minimum Query ---

    /// Find the minimum-priority node in the inclusive key range.
    ///
    /// Translated from __krmq_rmq in krmq.h.
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

    /// reverse_iterator for backwards traversal (used in inner-tree refinement).
    ///
    /// Provides move_previous() to step to the predecessor node.
    struct reverse_iterator {
        const rmq_node* stack_storage[64];
        const rmq_node** stack_top;
        const rmq_node* current_node = nullptr;

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

        /// Step to the predecessor (previous in-order node).
        ///
        /// @return true if a predecessor was found, false if we've exhausted the tree
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

            // Walk up the stack to find the first ancestor we came from the right of
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

        /// Get the current node.
        [[nodiscard]] const rmq_node* current() const {
            return current_node;
        }
    };
};

// ============================================================================
// rmq-based chaining entry point
// ============================================================================

/// rmq-based chaining.
///
/// Equivalent to mg_lchain_rmq() in lchain.c:369-521.
///
/// Uses avl trees augmented with Range Minimum Query to find the best
/// predecessor for each anchor in O(log n) instead of O(max_iter).
///
/// The algorithm groups anchors by reference position. When moving to a
/// new ref position, the previous group's anchors are inserted into the
/// tree(s). Out-of-range anchors are pruned. Then an rmq query finds
/// the best predecessor in the valid query-position range.
///
/// @param anchors                  Sorted anchor array (read-only span; the
///                                  kernel only reads positions and emits a
///                                  freshly-reordered anchor span).
/// @param params                   Chaining parameters (must have prepare()
///                                  called).
/// @param chains_out               Output slab — surviving chains are appended
///                                  here in score-descending order.
/// @param reordered_anchors_out    Output slab — anchors of surviving chains
///                                  appended in chain order; `chain::anchor_start`
///                                  indexes into this slab.
/// @param max_dist_inner           Inner tree max distance (0 = disabled).
/// @param cap_rmq_size             Maximum tree size before pruning (default
///                                  100000).
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
    const int64_t anchor_count = static_cast<int64_t>(anchors.size());
    if (anchor_count == 0) return;

    int32_t max_dist = params.max_gap;
    if (max_dist < params.bandwidth) max_dist = params.bandwidth;
    if (max_dist_inner <= 0 || max_dist_inner >= max_dist) {
        max_dist_inner = 0;
    }

    // Allocate all dynamic-programming arrays in the per-query chain arena.
    // Bulk initialization retains the exact sentinel values used by the
    // scalar and SIMD backends without introducing heap-owned intermediates.
    auto dp_scope = context::current().push<context::arena::chain>();
    auto& dp_ctx = context::current();
    auto best_scores  = dp_ctx.alloc_chain_array<int32_t>(static_cast<std::size_t>(anchor_count));
    auto predecessors = dp_ctx.alloc_chain_array<int64_t>(static_cast<std::size_t>(anchor_count));
    auto peak_scores  = dp_ctx.alloc_chain_array<int32_t>(static_cast<std::size_t>(anchor_count));
    std::memset(best_scores.data(),  0,
                static_cast<std::size_t>(anchor_count) * sizeof(int32_t));
    std::memset(predecessors.data(), 0xff,
                static_cast<std::size_t>(anchor_count) * sizeof(int64_t));
    std::memset(peak_scores.data(),  0,
                static_cast<std::size_t>(anchor_count) * sizeof(int32_t));

    // Allocate trees
    rmq_tree primary_tree;
    primary_tree.reserve(static_cast<size_t>(anchor_count) + 64);
    rmq_tree inner_tree;
    if (max_dist_inner > 0) {
        inner_tree.reserve(static_cast<size_t>(anchor_count) + 64);
    }

    // Track which anchors are in the trees for pruning
    int64_t group_start = 0;
    int64_t prune_start = 0;
    int64_t prune_start_inner = 0;

    // Per-anchor node pointers live in the chain arena. Supported targets use
    // the all-zero null-pointer representation required by this bulk reset.
    auto primary_nodes = dp_ctx.alloc_chain_array<rmq_node*>(
        static_cast<std::size_t>(anchor_count));
    auto inner_nodes   = dp_ctx.alloc_chain_array<rmq_node*>(
        static_cast<std::size_t>(anchor_count));
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

        // === Step 1: Insert completed ref-position groups into trees ===
        // Only insert when we move to a new reference position
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

        // === Step 2: Prune out-of-range nodes from primary tree ===
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

        // Cap tree size (lchain.c:434-438)
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

        // Prune inner tree
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

        // === Step 3: Primary rmq query ===
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

                // === Step 4: Inner-tree refinement ===
                // If the primary result is not exact and inner tree exists,
                // iterate backwards through inner tree applying skip logic
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

        // === Step 5: Store results ===
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

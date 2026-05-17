/**
 * @file chaining_rmq.cppm
 * @brief AVL+RMQ tree-based O(n log n) chaining algorithm.
 *
 * Translates minimap2's mg_lchain_rmq() (lchain.c:369-521) and the krmq.h
 * AVL tree macros into modern C++.
 *
 * The algorithm maintains AVL trees augmented with Range Minimum Query:
 *   - Primary tree: all in-range anchors, queried with [qpos - max_dist, qpos]
 *   - Inner tree (optional): finer-granularity for refinement
 *
 * Each tree node stores a priority = -(f[j] + 0.5 * gap_penalty * (ref_pos + query_pos)).
 * The subtree_min pointer tracks the minimum-priority node in each subtree,
 * enabling O(log n) range-minimum queries.
 *
 * Complexity: O(n log n) — each anchor does O(log n) insert + O(log n) RMQ query.
 *
 * Reference: minimap2 lchain.c, krmq.h
 */
module;

#include "common.h"

#include <cstdint>
#include <vector>
#include <span>
#include <limits>
#include <algorithm>
#include <cstring>

export module bioseq_sequences.chaining.rmq;

import mlogging;
import bioseq_sequences.chaining.utils;
import context;
import bioseq_instr;

export {
namespace bioseq::chaining {

// ============================================================================
// AVL+RMQ Tree Node
// ============================================================================

/// Node in the AVL tree with RMQ support.
///
/// Equivalent to lc_elem_t in lchain.c:338-343.
/// Keyed by (query_position, anchor_index) ascending.
/// The priority field enables RMQ: each node tracks the minimum-priority
/// node in its subtree via subtree_min.
struct RmqNode {
    int32_t query_position = 0;
    int64_t anchor_index   = 0;
    double  priority       = 0.0;  ///< -(best_scores[j] + 0.5 * gap_penalty * (ref_pos + query_pos))

    RmqNode* children[2]   = {nullptr, nullptr};
    RmqNode* subtree_min   = nullptr;
    int8_t   balance       = 0;
    uint32_t subtree_size  = 0;
};

// ============================================================================
// AVL+RMQ Tree
// ============================================================================

/// Compare two RMQ nodes by (query_position, anchor_index) ascending.
///
/// Matches lc_elem_cmp in lchain.c:345: same query_position ties are
/// broken by anchor index in ascending order (smaller index first).
///
/// @return <0 if a < b, 0 if equal, >0 if a > b
[[nodiscard]] inline int rmq_node_compare(const RmqNode* a, const RmqNode* b) {
    if (a->query_position != b->query_position) {
        return (a->query_position < b->query_position) ? -1 : 1;
    }
    if (a->anchor_index != b->anchor_index) {
        return (a->anchor_index < b->anchor_index) ? -1 : 1;
    }
    return 0;
}

/// Update the subtree_min pointer for a node based on its children.
///
/// The subtree_min is the node with the smallest priority value in the subtree.
/// This is the core augmentation that enables O(log n) range-minimum queries.
inline void rmq_update_min(RmqNode* node) {
    if (node == nullptr) return;
    node->subtree_min = node;
    if (node->children[0] != nullptr &&
        node->children[0]->subtree_min->priority < node->subtree_min->priority) {
        node->subtree_min = node->children[0]->subtree_min;
    }
    if (node->children[1] != nullptr &&
        node->children[1]->subtree_min->priority < node->subtree_min->priority) {
        node->subtree_min = node->children[1]->subtree_min;
    }
}

/// Update subtree_size for a node based on its children.
inline void rmq_update_size(RmqNode* node) {
    if (node == nullptr) return;
    node->subtree_size = 1;
    if (node->children[0] != nullptr) node->subtree_size += node->children[0]->subtree_size;
    if (node->children[1] != nullptr) node->subtree_size += node->children[1]->subtree_size;
}

/// AVL+RMQ tree for chaining predecessor lookup.
///
/// Translated from krmq.h macro-generated AVL tree with RMQ augmentation.
/// Supports insert, erase, find, and range-minimum query operations.
///
/// Uses a pool allocator: nodes are pre-allocated in a vector and recycled
/// via a free list for allocation-free operation during the chaining loop.
struct RmqTree {
    RmqNode* root = nullptr;
    std::vector<RmqNode> node_pool;
    size_t pool_next = 0;
    std::vector<RmqNode*> free_list;

    /// Reserve pool capacity for up to max_nodes insertions.
    void reserve(size_t max_nodes) {
        node_pool.resize(max_nodes);
        pool_next = 0;
        free_list.clear();
        free_list.reserve(max_nodes / 4);
    }

    /// Allocate a node from the pool or free list.
    [[nodiscard]] RmqNode* alloc_node() {
        if (!free_list.empty()) {
            RmqNode* node = free_list.back();
            free_list.pop_back();
            *node = RmqNode{};
            return node;
        }
        DEBUG_ASSERT(pool_next < node_pool.size(),
            "RmqTree node pool exhausted");
        RmqNode* node = &node_pool[pool_next++];
        *node = RmqNode{};
        return node;
    }

    /// Return a node to the free list.
    void free_node(RmqNode* node) {
        if (node != nullptr) {
            free_list.push_back(node);
        }
    }

    /// Get the number of nodes in the tree.
    [[nodiscard]] uint32_t size() const {
        return (root != nullptr) ? root->subtree_size : 0;
    }

    // --- AVL rotations (translated from krmq.h) ---

    /// Single rotation. dir=0 is left-rotation, dir=1 is right-rotation.
    static RmqNode* rotate_single(RmqNode* node, int dir) {
        RmqNode* other = node->children[1 - dir];
        node->children[1 - dir] = other->children[dir];
        other->children[dir] = node;
        rmq_update_size(node);
        rmq_update_min(node);
        rmq_update_size(other);
        rmq_update_min(other);
        return other;
    }

    /// Double rotation.
    static RmqNode* rotate_double(RmqNode* node, int dir) {
        node->children[1 - dir] = rotate_single(node->children[1 - dir], 1 - dir);
        return rotate_single(node, dir);
    }

    // --- Insert ---

    /// Insert a node into the tree. Returns the node if it was a duplicate (not inserted).
    ///
    /// Translated from __krmq_insert in krmq.h.
    RmqNode* insert(RmqNode* new_node) {
        new_node->children[0] = new_node->children[1] = nullptr;
        new_node->balance = 0;
        new_node->subtree_size = 1;
        new_node->subtree_min = new_node;

        if (root == nullptr) {
            root = new_node;
            return nullptr;
        }

        // Path from root to insertion point
        RmqNode* path_nodes[64];
        uint8_t path_dirs[64];
        int path_depth = 0;

        RmqNode* current = root;
        while (current != nullptr) {
            const int cmp = rmq_node_compare(new_node, current);
            if (cmp == 0) return current;  // Duplicate
            const int dir = (cmp > 0) ? 1 : 0;
            path_nodes[path_depth] = current;
            path_dirs[path_depth] = static_cast<uint8_t>(dir);
            ++path_depth;
            current = current->children[dir];
        }

        // Link the new node
        path_nodes[path_depth - 1]->children[path_dirs[path_depth - 1]] = new_node;

        // Walk back up, updating sizes, mins, and rebalancing.
        //
        // Two distinct bookkeeping concerns: the AVL balance values
        // propagate only as long as height is changing (we break out
        // of the balance walk as soon as it stabilises), but the
        // augmentations (subtree_size, subtree_min) MUST be updated
        // on every ancestor regardless of whether balance changed.
        // We track that with `still_propagating_balance` rather than
        // breaking out of the loop. If we break early, ancestors'
        // subtree_size stays stale and `tree.size()` reads the wrong
        // value — that was the actual root cause of the bioseq RMQ
        // segfault on LONG inputs (see RESEARCH/BENCHMARKING_SPEEDUP/
        // POOR_RESULTS_REPORT.md).
        bool still_propagating_balance = true;
        for (int depth = path_depth - 1; depth >= 0; --depth) {
            RmqNode* node = path_nodes[depth];
            if (!still_propagating_balance) {
                rmq_update_size(node);
                rmq_update_min(node);
                continue;
            }
            const int dir = path_dirs[depth];
            const int balance_delta = (dir == 0) ? -1 : 1;
            node->balance += static_cast<int8_t>(balance_delta);
            rmq_update_size(node);
            rmq_update_min(node);

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
                RmqNode* child = node->children[1 - rebalance_dir];
                RmqNode* rotated;
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
                    // Double rotation. Balance assignments mirror
                    // krmq.h's `__krmq_rotate2`: with `b1 =
                    // (dir == 0) ? +1 : -1` and `r` the post-rotation
                    // root (== `grandchild` here),
                    //   r.bal == b1   → q.bal = 0,   p.bal = -b1
                    //   r.bal == 0    → q.bal = 0,   p.bal = 0
                    //   r.bal == -b1  → q.bal = b1,  p.bal = 0
                    // where p = `node` and q = `child`. The earlier
                    // implementation had the p/q assignments swapped,
                    // which left the parent (`node`) with a stale ±1
                    // tilt while the AVL height was actually
                    // balanced. Over thousands of inserts and erases
                    // this drifted balance values into states that
                    // broke the rotation guard (`child` non-null when
                    // `node->balance == -2`), producing the SIGSEGV
                    // on long inputs.
                    RmqNode* grandchild = child->children[rebalance_dir];
                    rotated = rotate_double(node, rebalance_dir);
                    const int8_t b1 = static_cast<int8_t>(
                        (rebalance_dir == 0) ? 1 : -1);
                    if (grandchild->balance == 0) {
                        node->balance  = 0;
                        child->balance = 0;
                    } else if (grandchild->balance == b1) {
                        node->balance  = static_cast<int8_t>(-b1);
                        child->balance = 0;
                    } else {
                        node->balance  = 0;
                        child->balance = b1;
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
                    rmq_update_size(path_nodes[ancestor]);
                    rmq_update_min(path_nodes[ancestor]);
                }
                break;
            }
        }

        return nullptr;
    }

    // --- Erase ---

    /// Find and erase a node matching the given key.
    ///
    /// Returns the erased node (caller should free_node it) or nullptr if not found.
    /// Translated from __krmq_erase in krmq.h.
    RmqNode* erase(const RmqNode* key) {
        if (root == nullptr) return nullptr;

        RmqNode* path_nodes[64];
        uint8_t path_dirs[64];
        int path_depth = 0;

        // Find the node
        RmqNode* current = root;
        while (current != nullptr) {
            const int cmp = rmq_node_compare(key, current);
            if (cmp == 0) break;
            const int dir = (cmp > 0) ? 1 : 0;
            path_nodes[path_depth] = current;
            path_dirs[path_depth] = static_cast<uint8_t>(dir);
            ++path_depth;
            current = current->children[dir];
        }
        if (current == nullptr) return nullptr;

        RmqNode* erased = current;

        // If node has two children, swap with in-order successor
        if (current->children[0] != nullptr && current->children[1] != nullptr) {
            // Find in-order successor (leftmost in right subtree)
            const int swap_depth = path_depth;
            path_nodes[path_depth] = current;
            path_dirs[path_depth] = 1;
            ++path_depth;
            RmqNode* successor = current->children[1];
            while (successor->children[0] != nullptr) {
                path_nodes[path_depth] = successor;
                path_dirs[path_depth] = 0;
                ++path_depth;
                successor = successor->children[0];
            }

            // Replace current's position in the tree with successor
            // Copy successor's data into current, then remove successor
            // Actually, we swap the tree links to avoid data copy issues
            RmqNode* successor_right = successor->children[1];

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
            RmqNode* replacement = (current->children[0] != nullptr)
                ? current->children[0] : current->children[1];
            if (path_depth > 0) {
                path_nodes[path_depth - 1]->children[path_dirs[path_depth - 1]] = replacement;
            } else {
                root = replacement;
            }
        }

        // Rebalance upward.
        //
        // Same two-phase structure as `insert`: AVL balance updates
        // stop once height stabilises (balance settles to ±1, or a
        // rotation restores the pre-erase height), but the
        // subtree_size / subtree_min augmentations must continue
        // walking to the root because we removed a node from every
        // ancestor's subtree.
        //
        // The original code unconditionally `break`'d when balance
        // became ±1, leaving the root's subtree_size stale. That made
        // `tree.size()` lie about the actual node count, which then
        // tripped pruning/RMQ logic downstream and caused the SIGSEGV
        // on long inputs (RESEARCH/BENCHMARKING_SPEEDUP/POOR_RESULTS_REPORT.md).
        bool still_propagating_balance = true;
        for (int depth = path_depth - 1; depth >= 0; --depth) {
            RmqNode* node = path_nodes[depth];
            if (!still_propagating_balance) {
                rmq_update_size(node);
                rmq_update_min(node);
                continue;
            }
            const int dir = path_dirs[depth];
            const int balance_delta = (dir == 0) ? 1 : -1;
            node->balance += static_cast<int8_t>(balance_delta);
            rmq_update_size(node);
            rmq_update_min(node);

            if (node->balance == -1 || node->balance == 1) {
                // Height didn't change → balance is stable upward,
                // but every ancestor still needs size/min updates.
                still_propagating_balance = false;
                continue;
            } else if (node->balance == -2 || node->balance == 2) {
                const int rebalance_dir = (node->balance < 0) ? 1 : 0;
                RmqNode* child = node->children[1 - rebalance_dir];
                RmqNode* rotated;
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
                    // Double rotation. Same balance assignments as
                    // the insert path (see comment there); the
                    // earlier code had p/q swapped, leaving stale
                    // ±1 tilts on `node` after a double rotation.
                    RmqNode* grandchild = child->children[rebalance_dir];
                    rotated = rotate_double(node, rebalance_dir);
                    const int8_t b1 = static_cast<int8_t>(
                        (rebalance_dir == 0) ? 1 : -1);
                    if (grandchild->balance == 0) {
                        node->balance  = 0;
                        child->balance = 0;
                    } else if (grandchild->balance == b1) {
                        node->balance  = static_cast<int8_t>(-b1);
                        child->balance = 0;
                    } else {
                        node->balance  = 0;
                        child->balance = b1;
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

    /// Find the node with minimum priority in the range [lo, hi] (inclusive by key).
    ///
    /// Translated from __krmq_rmq in krmq.h.
    /// Returns nullptr if no node falls within the range.
    [[nodiscard]] const RmqNode* rmq(const RmqNode* lo, const RmqNode* hi) const {
        if (root == nullptr) return nullptr;

        // Find the split node where lo and hi diverge
        const RmqNode* split = root;
        while (split != nullptr) {
            if (rmq_node_compare(hi, split) < 0) {
                split = split->children[0];
            } else if (rmq_node_compare(lo, split) > 0) {
                split = split->children[1];
            } else {
                break;
            }
        }
        if (split == nullptr) return nullptr;

        // Check if the split node itself is in range
        const RmqNode* best = nullptr;
        if (rmq_node_compare(lo, split) <= 0 && rmq_node_compare(split, hi) <= 0) {
            best = split;
        }

        // Walk left subtree: collect nodes >= lo
        const RmqNode* left_walk = split->children[0];
        while (left_walk != nullptr) {
            if (rmq_node_compare(lo, left_walk) <= 0) {
                // left_walk and its right subtree are all >= lo
                if (best == nullptr || left_walk->priority < best->priority) {
                    best = left_walk;
                }
                if (left_walk->children[1] != nullptr &&
                    left_walk->children[1]->subtree_min->priority <
                        (best ? best->priority : left_walk->priority)) {
                    best = left_walk->children[1]->subtree_min;
                }
                left_walk = left_walk->children[0];
            } else {
                left_walk = left_walk->children[1];
            }
        }

        // Walk right subtree: collect nodes <= hi
        const RmqNode* right_walk = split->children[1];
        while (right_walk != nullptr) {
            if (rmq_node_compare(right_walk, hi) <= 0) {
                // right_walk and its left subtree are all <= hi
                if (best == nullptr || right_walk->priority < best->priority) {
                    best = right_walk;
                }
                if (right_walk->children[0] != nullptr &&
                    right_walk->children[0]->subtree_min->priority <
                        (best ? best->priority : right_walk->priority)) {
                    best = right_walk->children[0]->subtree_min;
                }
                right_walk = right_walk->children[1];
            } else {
                right_walk = right_walk->children[0];
            }
        }

        return best;
    }

    // --- Find lower/upper bounds ---

    /// Find the largest node < key (lower bound) and smallest node > key (upper bound).
    ///
    /// Used by the inner-tree refinement step.
    void interval(const RmqNode* key, const RmqNode*& lower, const RmqNode*& upper) const {
        lower = nullptr;
        upper = nullptr;
        const RmqNode* current = root;
        while (current != nullptr) {
            const int cmp = rmq_node_compare(key, current);
            if (cmp < 0) {
                upper = current;
                current = current->children[0];
            } else if (cmp > 0) {
                lower = current;
                current = current->children[1];
            } else {
                // Exact match: lower = predecessor, upper = successor
                if (current->children[0] != nullptr) {
                    lower = current->children[0];
                    while (lower->children[1] != nullptr) {
                        lower = lower->children[1];
                    }
                }
                if (current->children[1] != nullptr) {
                    upper = current->children[1];
                    while (upper->children[0] != nullptr) {
                        upper = upper->children[0];
                    }
                }
                return;
            }
        }
    }

    /// Iterator for backwards traversal (used in inner-tree refinement).
    ///
    /// Provides prev() to step to the predecessor node.
    struct Iterator {
        const RmqNode* stack_storage[64];
        const RmqNode** stack_top;
        const RmqNode* current_node = nullptr;

        Iterator() : stack_top(stack_storage) {}

        /// Position the iterator at the largest node <= key.
        void find_le(const RmqNode* tree_root, const RmqNode* key) {
            stack_top = stack_storage;
            current_node = nullptr;
            const RmqNode* node = tree_root;
            while (node != nullptr) {
                const int cmp = rmq_node_compare(key, node);
                if (cmp == 0) {
                    current_node = node;
                    return;
                } else if (cmp > 0) {
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
        bool prev() {
            if (current_node == nullptr) return false;

            // If left child exists, go to rightmost node in left subtree
            if (current_node->children[0] != nullptr) {
                const RmqNode* node = current_node->children[0];
                while (node->children[1] != nullptr) {
                    *stack_top++ = node;
                    node = node->children[1];
                }
                current_node = node;
                return true;
            }

            // Walk up the stack to find the first ancestor we came from the right of
            while (stack_top > stack_storage) {
                const RmqNode* parent = *(--stack_top);
                if (parent->children[1] == current_node ||
                    rmq_node_compare(parent, current_node) < 0)
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
        [[nodiscard]] const RmqNode* at() const { return current_node; }
    };
};

// ============================================================================
// RMQ-based chaining entry point
// ============================================================================

/// RMQ-based chaining.
///
/// Equivalent to mg_lchain_rmq() in lchain.c:369-521.
///
/// Uses AVL trees augmented with Range Minimum Query to find the best
/// predecessor for each anchor in O(log n) instead of O(max_iter).
///
/// The algorithm groups anchors by reference position. When moving to a
/// new ref position, the previous group's anchors are inserted into the
/// tree(s). Out-of-range anchors are pruned. Then an RMQ query finds
/// the best predecessor in the valid query-position range.
///
/// @param anchors         Sorted anchor array (read-only span; the kernel only
///                         reads positions and emits a freshly-reordered
///                         anchor vector alongside chains).
/// @param params          Chaining parameters (must have prepare() called)
/// @param max_dist_inner  Inner tree max distance (0 = disabled)
/// @param cap_rmq_size    Maximum tree size before pruning (default 100000)
/// @return Pair of (chains, reordered_anchors)
[[nodiscard]] std::pair<std::vector<Chain>, std::vector<Anchor>> chain_anchors_rmq(
    std::span<const Anchor> anchors,
    const ChainingParams& params,
    int32_t max_dist_inner = 0,
    int32_t cap_rmq_size = 100000)
{
    static const auto* _rmq_desc =
        bioseq::instr::register_region_static("mg_lchain_rmq");
    auto _rmq_scope = bioseq::instr::region(_rmq_desc);
    const int64_t anchor_count = static_cast<int64_t>(anchors.size());
    if (anchor_count == 0) return {{}, {}};

    int32_t max_dist = params.max_gap;
    if (max_dist < params.bandwidth) max_dist = params.bandwidth;
    if (max_dist_inner <= 0 || max_dist_inner >= max_dist) {
        max_dist_inner = 0;
    }

    // DP arrays — chain arena (SPEC §5.2).
    auto dp_scope = context::current().push<context::Arena::Chain>();
    auto& dp_ctx = context::current();
    auto best_scores_v  = dp_ctx.make_chain_vector<int32_t>(anchor_count);
    auto predecessors_v = dp_ctx.make_chain_vector<int64_t>(anchor_count);
    auto peak_scores_v  = dp_ctx.make_chain_vector<int32_t>(anchor_count);
    best_scores_v.assign(anchor_count, 0);
    predecessors_v.assign(anchor_count, -1);
    peak_scores_v.assign(anchor_count, 0);
    auto best_scores  = std::span<int32_t>(best_scores_v);
    auto predecessors = std::span<int64_t>(predecessors_v);
    auto peak_scores  = std::span<int32_t>(peak_scores_v);

    // Allocate trees
    RmqTree primary_tree;
    primary_tree.reserve(static_cast<size_t>(anchor_count) + 64);
    RmqTree inner_tree;
    if (max_dist_inner > 0) {
        inner_tree.reserve(static_cast<size_t>(anchor_count) + 64);
    }

    // Track which anchors are in the trees for pruning
    int64_t group_start = 0;
    int64_t prune_start = 0;
    int64_t prune_start_inner = 0;

    // Per-anchor node pointers (chain arena).
    auto primary_nodes_v = dp_ctx.make_chain_vector<RmqNode*>(anchor_count);
    auto inner_nodes_v   = dp_ctx.make_chain_vector<RmqNode*>(anchor_count);
    primary_nodes_v.assign(anchor_count, nullptr);
    inner_nodes_v.assign(anchor_count, nullptr);
    auto primary_nodes = std::span<RmqNode*>(primary_nodes_v);
    auto inner_nodes   = std::span<RmqNode*>(inner_nodes_v);

    {
    static const auto* _fwd_desc =
        bioseq::instr::register_loop_static("mg_lchain_rmq_forward");
    auto _fwd_scope = bioseq::instr::loop(_fwd_desc);
    for (int64_t current_idx = 0; current_idx < anchor_count; ++current_idx) {
        _fwd_scope.tick();
        int32_t best_score = static_cast<int32_t>(anchors[current_idx].query.query_span);
        int64_t best_predecessor = -1;

        // === Step 1: Insert completed ref-position groups into trees ===
        // Only insert when we move to a new reference position
        if (group_start < current_idx &&
            anchors[group_start].ref_bits != anchors[current_idx].ref_bits)
        {
            for (int64_t j = group_start; j < current_idx; ++j) {
                RmqNode* node = primary_tree.alloc_node();
                node->query_position = static_cast<int32_t>(anchors[j].query.query_position);
                node->anchor_index = j;
                node->priority = -(static_cast<double>(best_scores[j])
                    + 0.5 * static_cast<double>(params.gap_penalty)
                    * (static_cast<double>(static_cast<int32_t>(anchors[j].ref_bits))
                     + static_cast<double>(static_cast<int32_t>(anchors[j].query_bits))));
                primary_tree.insert(node);
                primary_nodes[j] = node;

                if (max_dist_inner > 0) {
                    RmqNode* inode = inner_tree.alloc_node();
                    inode->query_position = node->query_position;
                    inode->anchor_index = j;
                    inode->priority = node->priority;
                    inner_tree.insert(inode);
                    inner_nodes[j] = inode;
                }
            }
            group_start = current_idx;
        }

        // === Step 2: Prune out-of-range nodes from primary tree ===
        while (prune_start < current_idx) {
            if (anchors[current_idx].ref_bits >> 32 != anchors[prune_start].ref_bits >> 32 ||
                anchors[current_idx].ref_bits > anchors[prune_start].ref_bits
                    + static_cast<uint64_t>(max_dist))
            {
                if (primary_nodes[prune_start] != nullptr) {
                    RmqNode* erased = primary_tree.erase(primary_nodes[prune_start]);
                    if (erased != nullptr) primary_tree.free_node(erased);
                    primary_nodes[prune_start] = nullptr;
                }
                ++prune_start;
            } else {
                break;
            }
        }

        // Cap tree size (lchain.c:434-438)
        if (primary_tree.size() > static_cast<uint32_t>(cap_rmq_size)) {
            while (prune_start < current_idx &&
                   primary_tree.size() > static_cast<uint32_t>(cap_rmq_size))
            {
                if (primary_nodes[prune_start] != nullptr) {
                    RmqNode* erased = primary_tree.erase(primary_nodes[prune_start]);
                    if (erased != nullptr) primary_tree.free_node(erased);
                    primary_nodes[prune_start] = nullptr;
                }
                ++prune_start;
            }
        }

        // Prune inner tree
        if (max_dist_inner > 0) {
            while (prune_start_inner < current_idx) {
                if (anchors[current_idx].ref_bits >> 32 != anchors[prune_start_inner].ref_bits >> 32 ||
                    anchors[current_idx].ref_bits > anchors[prune_start_inner].ref_bits
                        + static_cast<uint64_t>(max_dist_inner))
                {
                    if (inner_nodes[prune_start_inner] != nullptr) {
                        RmqNode* erased = inner_tree.erase(inner_nodes[prune_start_inner]);
                        if (erased != nullptr) inner_tree.free_node(erased);
                        inner_nodes[prune_start_inner] = nullptr;
                    }
                    ++prune_start_inner;
                } else {
                    break;
                }
            }
        }

        // === Step 3: Primary RMQ query ===
        if (primary_tree.size() > 0) {
            const int32_t current_query_pos = static_cast<int32_t>(
                anchors[current_idx].query.query_position);
            RmqNode lo_key;
            lo_key.query_position = current_query_pos - max_dist;
            lo_key.anchor_index = std::numeric_limits<int64_t>::min();
            RmqNode hi_key;
            hi_key.query_position = current_query_pos;
            hi_key.anchor_index = std::numeric_limits<int64_t>::max();

            const RmqNode* rmq_result = primary_tree.rmq(&lo_key, &hi_key);
            if (rmq_result != nullptr) {
                const int64_t j = rmq_result->anchor_index;
                bool is_exact = false;
                int32_t diagonal_width = 0;
                const int32_t pair_score = compute_chaining_score_simple(
                    anchors[current_idx], anchors[j],
                    params.gap_penalty, params.skip_penalty,
                    is_exact, diagonal_width);
                const int32_t total_score = pair_score + best_scores[j];
                if (total_score > best_score) {
                    best_score = total_score;
                    best_predecessor = j;
                }

                // === Step 4: Inner-tree refinement ===
                // If the primary result is not exact and inner tree exists,
                // iterate backwards through inner tree applying skip logic
                if (!is_exact && max_dist_inner > 0 && inner_tree.size() > 0) {
                    RmqNode inner_hi_key;
                    inner_hi_key.query_position = current_query_pos;
                    inner_hi_key.anchor_index = std::numeric_limits<int64_t>::max();

                    typename RmqTree::Iterator iter;
                    iter.find_le(inner_tree.root, &inner_hi_key);

                    int32_t skip_count = 0;
                    const int32_t max_skip = params.max_chain_skip;
                    while (iter.at() != nullptr) {
                        const RmqNode* candidate = iter.at();
                        const int64_t cj = candidate->anchor_index;
                        if (static_cast<int32_t>(anchors[current_idx].query.query_position)
                            - candidate->query_position > max_dist_inner)
                        {
                            break;
                        }
                        bool inner_exact = false;
                        int32_t inner_diag_width = 0;
                        const int32_t inner_pair_score = compute_chaining_score_simple(
                            anchors[current_idx], anchors[cj],
                            params.gap_penalty, params.skip_penalty,
                            inner_exact, inner_diag_width);
                        const int32_t inner_total = inner_pair_score + best_scores[cj];
                        if (inner_total > best_score) {
                            best_score = inner_total;
                            best_predecessor = cj;
                            if (skip_count > 0) --skip_count;
                        } else {
                            if (++skip_count > max_skip) break;
                        }
                        if (inner_exact) break;
                        if (!iter.prev()) break;
                    }
                }
            }
        }

        // === Step 5: Store results ===
        best_scores[current_idx] = best_score;
        predecessors[current_idx] = best_predecessor;
        peak_scores[current_idx] = (best_predecessor >= 0
            && peak_scores[best_predecessor] > best_score)
            ? peak_scores[best_predecessor] : best_score;
    }

    }  // end mg_lchain_rmq_forward scope
    JustLog("[chaining_rmq] RMQ chaining pass complete for {} anchors", anchor_count);

    static const auto* _bt_desc =
        bioseq::instr::register_region_static("backtrack_chains");
    auto _bt_scope = bioseq::instr::region(_bt_desc);
    return backtrack_chains(anchors,
        std::span<const int32_t>(best_scores_v),
        std::span<const int64_t>(predecessors_v),
        std::span<const int32_t>(peak_scores_v),
        params.min_chain_score, params.min_seed_count);
}

}  // namespace bioseq::chaining
}  // export

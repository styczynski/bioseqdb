/**
 * @file chaining_scalar.cppm
 * @brief Scalar DP chaining algorithm, extracted from minimap_port.cppm.
 *
 * This is the original chain_anchors_dp() algorithm with no algorithmic changes.
 * Equivalent to mg_lchain_dp() in lchain.c:150-219.
 *
 * Complexity: O(n * max_iter) per anchor.
 *
 * Changes from the original minimap_port.cppm extraction:
 *   - Accepts ChainingParams instead of Minimap2Params
 *   - Uses precomputed gap_penalty/skip_penalty from ChainingParams
 *   - Delegates backtracking to shared backtrack_chains() from chaining_utils
 */
module;

#include "common.h"

#include <cstdint>
#include <vector>
#include <algorithm>
#include <limits>

export module bioseq_sequences.chaining.scalar;

import mlogging;
import bioseq_sequences.chaining.utils;
import context;
import bioseq_instr;

export {
namespace bioseq::chaining {

/// Scalar DP chaining.
///
/// This is the original chain_anchors_dp() from minimap_port.cppm, extracted verbatim.
/// Equivalent to mg_lchain_dp() in lchain.c:150-219.
///
/// Algorithm:
///   For each anchor (sorted by ref position):
///     1. Search backwards through prior anchors in [window_start, current-1]
///     2. Compute chaining score: pair_score(current, candidate) + best_scores[candidate]
///     3. Track best predecessor and score
///     4. Apply max_skip early termination
///     5. Try the globally best anchor (global_best_idx) as fallback
///
///   Backtrack from highest-scoring anchors to extract chains.
///
/// @param anchors  Sorted anchor array
/// @param params   Chaining parameters (must have prepare() called)
/// @return Pair of (chains, reordered_anchors)
[[nodiscard]] std::pair<std::vector<Chain>, std::vector<Anchor>> chain_anchors_scalar(
    std::vector<Anchor>& anchors,
    const ChainingParams& params)
{
    static const auto* _dp_desc =
        bioseq::instr::register_region_static("mg_lchain_dp");
    auto _dp_scope = bioseq::instr::region(_dp_desc);
    const int64_t anchor_count = static_cast<int64_t>(anchors.size());
    if (anchor_count == 0) {
        return {{}, {}};
    }

    int32_t max_dist_x = params.max_gap;
    int32_t max_dist_y = params.max_gap;
    if (max_dist_x < params.bandwidth) { max_dist_x = params.bandwidth; }
    if (max_dist_y < params.bandwidth) { max_dist_y = params.bandwidth; }

    const int32_t max_skip = params.max_chain_skip;
    const int32_t max_iter = params.max_chain_iter;

    // DP arrays — chain arena (SPEC §5.2). All four die at function exit,
    // so the scope-anchored arena recycles them for every chaining call.
    auto dp_scope = context::current().push<context::Arena::Chain>();
    auto& dp_ctx = context::current();
    auto best_scores_v  = dp_ctx.make_chain_vector<int32_t>(anchor_count);
    auto predecessors_v = dp_ctx.make_chain_vector<int64_t>(anchor_count);
    auto peak_scores_v  = dp_ctx.make_chain_vector<int32_t>(anchor_count);
    auto visited_v      = dp_ctx.make_chain_vector<int32_t>(anchor_count);
    best_scores_v.resize(anchor_count);
    predecessors_v.assign(anchor_count, -1);
    peak_scores_v.resize(anchor_count);
    visited_v.assign(anchor_count, 0);
    auto best_scores  = std::span<int32_t>(best_scores_v);
    auto predecessors = std::span<int64_t>(predecessors_v);
    auto peak_scores  = std::span<int32_t>(peak_scores_v);
    auto visited      = std::span<int32_t>(visited_v);

    int64_t window_start = 0;
    int64_t global_best_idx = -1;

    // --- Forward DP pass ---
    // Equivalent to lchain.c lines 171-206
    {
    static const auto* _fwd_desc =
        bioseq::instr::register_loop_static("mg_lchain_dp_forward");
    auto _fwd_scope = bioseq::instr::loop(_fwd_desc);
    for (int64_t current_idx = 0; current_idx < anchor_count; ++current_idx) {
        _fwd_scope.tick();
        int64_t best_predecessor_idx = -1;
        int32_t best_score = static_cast<int32_t>(anchors[current_idx].query.query_span);
        int32_t skip_count = 0;

        // Advance window start: skip anchors on different chromosomes or too far
        while (window_start < current_idx &&
               (anchors[current_idx].ref_bits >> 32 != anchors[window_start].ref_bits >> 32 ||
                anchors[current_idx].ref_bits > anchors[window_start].ref_bits
                    + static_cast<uint64_t>(max_dist_x)))
        {
            ++window_start;
        }
        if (current_idx - window_start > max_iter) {
            window_start = current_idx - max_iter;
        }

        int64_t search_end = window_start;

        // Search backwards for best predecessor
        for (int64_t candidate_idx = current_idx - 1;
             candidate_idx >= window_start; --candidate_idx)
        {
            const int32_t pair_score = compute_chaining_score(
                anchors[current_idx], anchors[candidate_idx],
                max_dist_x, max_dist_y, params.bandwidth,
                params.gap_penalty, params.skip_penalty);
            if (pair_score == std::numeric_limits<int32_t>::min()) {
                continue;
            }

            const int32_t total_score = pair_score + best_scores[candidate_idx];
            if (total_score > best_score) {
                best_score = total_score;
                best_predecessor_idx = candidate_idx;
                if (skip_count > 0) {
                    --skip_count;
                }
            } else if (visited[candidate_idx] == static_cast<int32_t>(current_idx)) {
                if (++skip_count > max_skip) {
                    search_end = candidate_idx;
                    break;
                }
            }
            if (predecessors[candidate_idx] >= 0) {
                visited[predecessors[candidate_idx]] = static_cast<int32_t>(current_idx);
            }
        }

        // Try the globally best anchor in the window (global_best_idx optimization)
        if (global_best_idx < 0 ||
            anchors[current_idx].ref_bits - anchors[global_best_idx].ref_bits
                > static_cast<uint64_t>(max_dist_x))
        {
            int32_t best_global = std::numeric_limits<int32_t>::min();
            global_best_idx = -1;
            for (int64_t scan_idx = current_idx - 1; scan_idx >= window_start; --scan_idx) {
                if (best_global < best_scores[scan_idx]) {
                    best_global = best_scores[scan_idx];
                    global_best_idx = scan_idx;
                }
            }
        }
        if (global_best_idx >= 0 && global_best_idx < search_end) {
            const int32_t global_pair_score = compute_chaining_score(
                anchors[current_idx], anchors[global_best_idx],
                max_dist_x, max_dist_y, params.bandwidth,
                params.gap_penalty, params.skip_penalty);
            if (global_pair_score != std::numeric_limits<int32_t>::min()
                && best_score < global_pair_score + best_scores[global_best_idx])
            {
                best_score = global_pair_score + best_scores[global_best_idx];
                best_predecessor_idx = global_best_idx;
            }
        }

        best_scores[current_idx] = best_score;
        predecessors[current_idx] = best_predecessor_idx;
        const bool predecessor_has_higher_peak = best_predecessor_idx >= 0
            && peak_scores[best_predecessor_idx] > best_score;
        peak_scores[current_idx] = predecessor_has_higher_peak
            ? peak_scores[best_predecessor_idx]
            : best_score;

        if (global_best_idx < 0 ||
            (anchors[current_idx].ref_bits - anchors[global_best_idx].ref_bits
                <= static_cast<uint64_t>(max_dist_x)
             && best_scores[global_best_idx] < best_scores[current_idx]))
        {
            global_best_idx = current_idx;
        }
    }

    }  // end mg_lchain_dp_forward scope
    JustLog("[chaining_scalar] Forward DP pass complete for {} anchors", anchor_count);

    static const auto* _bt_desc =
        bioseq::instr::register_region_static("backtrack_chains");
    auto _bt_scope = bioseq::instr::region(_bt_desc);
    return backtrack_chains(anchors, best_scores, predecessors, peak_scores,
                           params.min_chain_score, params.min_seed_count);
}

}  // namespace bioseq::chaining
}  // export

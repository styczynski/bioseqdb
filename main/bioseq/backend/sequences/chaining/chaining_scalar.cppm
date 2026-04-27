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
#include <cstring>
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
    //
    // The previous implementation created four std::vector<T, foonathan>
    // objects via make_chain_vector and then resize()'d / assign()'d each
    // one to value-init the elements. resize() / assign() on a foonathan
    // vector touches every element via placement-new, so for arrays whose
    // contents are written-before-read (best_scores, peak_scores,
    // predecessors[i] also overwritten before the read in the second
    // global_best_idx loop) the zero-fill is wasted work.
    //
    // Replace with `alloc_chain_array<T>` bump allocations (no zero-fill,
    // no allocator-state-carrying vector wrapper). best_scores and
    // peak_scores stay uninitialised: they're written on every iteration
    // of the forward DP pass before being read. predecessors and visited
    // are initialised to -1 / 0 via memset — one syscall-level memset
    // beats N placement-new copies.
    auto dp_scope = context::current().push<context::Arena::Chain>();
    auto& dp_ctx = context::current();
    auto best_scores  = dp_ctx.alloc_chain_array<int32_t>(anchor_count);
    auto predecessors = dp_ctx.alloc_chain_array<int64_t>(anchor_count);
    auto peak_scores  = dp_ctx.alloc_chain_array<int32_t>(anchor_count);
    auto visited      = dp_ctx.alloc_chain_array<int32_t>(anchor_count);
    // predecessors[] = -1 (the "no predecessor" sentinel checked at line
    // 143 below) — int64_t -1 is all-ones, so memset(0xff) does the job.
    std::memset(predecessors.data(), 0xff,
                static_cast<std::size_t>(anchor_count) * sizeof(int64_t));
    // visited[] = 0 (the "not visited" sentinel checked at line 137).
    std::memset(visited.data(), 0,
                static_cast<std::size_t>(anchor_count) * sizeof(int32_t));

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

        // Cache the pair_score for the current `global_best_idx` so the
        // post-loop fallback below can reuse it instead of calling
        // compute_chaining_score a second time. Negative initialisation
        // sentinel: only used when the matching candidate_idx is hit.
        int32_t cached_global_pair_score = std::numeric_limits<int32_t>::min();
        bool global_pair_score_cached = false;

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

            // Stash the pair_score for global_best_idx — we may need it
            // again right after this loop. compute_chaining_score is
            // ~5-10 % of the chaining loop's CPU cost, so avoiding a
            // second invocation per outer iter is a measurable win.
            if (candidate_idx == global_best_idx) {
                cached_global_pair_score = pair_score;
                global_pair_score_cached = true;
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
            // global_best_idx changed — the cache is stale.
            global_pair_score_cached = false;
        }
        if (global_best_idx >= 0 && global_best_idx < search_end) {
            // Use cached pair_score from pass 1 if available; otherwise
            // recompute (early-exit case where the inner loop broke past
            // global_best_idx, or a fresh global_best_idx from the rescan).
            const int32_t global_pair_score = global_pair_score_cached
                ? cached_global_pair_score
                : compute_chaining_score(
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

/**
 * @file scalar.cppm
 * @brief Scalar dynamic-programming anchor chaining.
 *
 * Scores coordinate-sorted anchors with prepared gap and skip penalties, then
 * writes retained chains through the shared arena-backed backtracking contract.
 * Complexity is O(n * max_chain_iter) per query.
 */
module;

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>
#include <algorithm>
#include <limits>
#include <stdexcept>

export module bioseq.chaining.scalar;

import bioseq.common.logging;
import bioseq.chaining.core;
import bioseq.common.context;
import bioseq.instrumentation;

export {
namespace bioseq::chaining {

/// Chain coordinate-sorted anchors with scalar dynamic programming.
///
/// For each coordinate-sorted anchor, the kernel:
///   1. Searches prior anchors in `[window_start, current - 1]`.
///   2. Adds the pair score to the candidate's best score.
///   3. Tracks the best predecessor and score.
///   4. Applies the configured maximum-skip termination.
///   5. Tries the globally best anchor as a fallback candidate.
///
/// It then backtracks from highest-scoring anchors to extract chains.
///
/// @param anchors                  Sorted anchor array (read-only span. The
///                                  kernel does not mutate the input — it only
///                                  reads positions to score and emit a
///                                  freshly-reordered anchor span alongside
///                                  the chains).
/// @param params                   Chaining parameters (must have prepare()
///                                  called).
/// @param chains_out               Output slab — surviving chains are appended
///                                  here in score-descending order.
/// @param reordered_anchors_out    Output slab — anchors of surviving chains
///                                  appended in chain order. `chain::anchor_start`
///                                  indexes into this slab.
///
/// Both output slabs must be live arena_slab values rooted in the caller's
/// per-query `arena::chain` scope. The kernel writes directly into them via
/// `backtrack_chains`, avoiding an intermediate output container and copy.
void chain_anchors_scalar(
    std::span<const anchor> anchors,
    const chaining_parameters& params,
    arena_slab<chain>& chains_out,
    arena_slab<anchor>& reordered_anchors_out)
{
    static const auto* dp_instrumentation_descriptor =
        bioseq::instrumentation::register_static_region("mg_lchain_dp");
    auto dp_instrumentation_scope = bioseq::instrumentation::region(dp_instrumentation_descriptor);
    const auto storage_plan = derive_scalar_chaining_storage_plan(anchors.size(), params);
    if (!storage_plan) {
        throw std::length_error{"scalar chaining storage exceeds its addressable domain"};
    }
    const auto scratch_count = storage_plan->forward_anchor_count;
    const int64_t anchor_count = static_cast<int64_t>(scratch_count);
    if (anchor_count == 0) {
        return;
    }

    int32_t max_dist_x = params.max_gap;
    int32_t max_dist_y = params.max_gap;
    if (max_dist_x < params.bandwidth) { max_dist_x = params.bandwidth; }
    if (max_dist_y < params.bandwidth) { max_dist_y = params.bandwidth; }

    const int32_t max_skip = params.max_chain_skip;
    const int32_t max_iter = params.max_chain_iter;

    // Keep all dynamic-programming storage in the per-query chain arena.
    // best_scores and peak_scores are written before their first read;
    // predecessors and visited require the exact -1 and zero sentinels below.
    auto dp_scope = context::current().push<context::arena::chain>();
    auto& dp_ctx = context::current();
    auto best_scores  = dp_ctx.alloc_chain_array<int32_t>(scratch_count);
    auto predecessors = dp_ctx.alloc_chain_array<int64_t>(scratch_count);
    auto peak_scores  = dp_ctx.alloc_chain_array<int32_t>(scratch_count);
    auto visited      = dp_ctx.alloc_chain_array<int32_t>(scratch_count);
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
    static const auto* forward_instrumentation_descriptor =
        bioseq::instrumentation::register_static_loop("mg_lchain_dp_forward");
    auto forward_instrumentation_scope = bioseq::instrumentation::loop(forward_instrumentation_descriptor);
    for (int64_t current_idx = 0; current_idx < anchor_count; ++current_idx) {
        forward_instrumentation_scope.record_iteration();
        int64_t best_predecessor_idx = -1;
        int32_t best_score = static_cast<int32_t>(anchors[current_idx].query_span);
        int32_t skip_count = 0;

        // Advance window start: skip anchors on different chromosomes or too far
        while (window_start < current_idx &&
               (anchors[current_idx].strand_partition_key() != anchors[window_start].strand_partition_key() ||
                anchors[current_idx].reference_sort_key() > anchors[window_start].reference_sort_key()
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
            anchors[current_idx].reference_sort_key() - anchors[global_best_idx].reference_sort_key()
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
            (anchors[current_idx].reference_sort_key() - anchors[global_best_idx].reference_sort_key()
                <= static_cast<uint64_t>(max_dist_x)
             && best_scores[global_best_idx] < best_scores[current_idx]))
        {
            global_best_idx = current_idx;
        }
    }

    }  // end mg_lchain_dp_forward scope
    bioseq_log<logging_level::debug>("[chaining_scalar] Forward DP pass complete for {} anchors", anchor_count);

    static const auto* backtrack_instrumentation_descriptor =
        bioseq::instrumentation::register_static_region("backtrack_chains");
    auto backtrack_instrumentation_scope = bioseq::instrumentation::region(backtrack_instrumentation_descriptor);
    backtrack_chains(anchors, best_scores, predecessors, peak_scores,
                     params.min_chain_score, params.min_seed_count,
                     chains_out, reordered_anchors_out);
}

}  // namespace bioseq::chaining
}  // export

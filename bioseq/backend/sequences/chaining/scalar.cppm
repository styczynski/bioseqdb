/**
 * @file scalar.cppm
 * @brief Chain anchors with scalar dynamic programming.
 *
 * Score possible predecessors within a moving reference window, then follow
 * the chosen predecessor links to recover chains. Temporary arrays and output
 * records use the chaining arena.
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

/// Build chains by choosing a predecessor for each anchor.
/// Search recent anchors first. Distance and iteration limits bound the search.
/// The visited-mark skip rule can stop repeated unsuccessful checks, while a
/// cached high-scoring anchor gets one extra check if the main search missed it.
/// Backtrack from highest-scoring endpoints, keeping chains that meet score and
/// seed-count limits. Claim shared anchors once. Both output slabs must stay
/// bound to the caller's live chaining arena.
///
/// @param anchors Anchors in canonical_anchor_less order.
/// @param params Chaining settings with prepare() already called.
/// @param chains_out Receives accepted chains in endpoint-processing order.
/// @param reordered_anchors_out Receives each chain's anchors in forward order.
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

    // Keep scoring arrays in the chaining arena. Scores are written before
    // use. Initialize the predecessor and visited arrays for their sentinel checks.
    auto dp_scope = context::current().push<context::arena::chain>();
    auto& dp_ctx = context::current();
    auto best_scores  = dp_ctx.alloc_chain_array<int32_t>(scratch_count);
    auto predecessors = dp_ctx.alloc_chain_array<int64_t>(scratch_count);
    auto peak_scores  = dp_ctx.alloc_chain_array<int32_t>(scratch_count);
    auto visited      = dp_ctx.alloc_chain_array<int32_t>(scratch_count);
    // -1 marks an anchor with no predecessor.
    std::memset(predecessors.data(), 0xff,
                static_cast<std::size_t>(anchor_count) * sizeof(int64_t));
    // Zero clears the visited-generation marks.
    std::memset(visited.data(), 0,
                static_cast<std::size_t>(anchor_count) * sizeof(int32_t));

    int64_t window_start = 0;
    int64_t global_best_idx = -1;

    // Score anchors in their sorted order.
    {
    static const auto* forward_instrumentation_descriptor =
        bioseq::instrumentation::register_static_loop("mg_lchain_dp_forward");
    auto forward_instrumentation_scope = bioseq::instrumentation::loop(forward_instrumentation_descriptor);
    for (int64_t current_idx = 0; current_idx < anchor_count; ++current_idx) {
        forward_instrumentation_scope.record_iteration();
        int64_t best_predecessor_idx = -1;
        int32_t best_score = static_cast<int32_t>(anchors[current_idx].query_span);
        int32_t skip_count = 0;

        // Move past anchors from another strand or outside the reference window.
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

        // Save the pair score if the search reaches the cached best anchor,
        // so the later extra-candidate check can reuse it.
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

            // Save this pair score for the extra-candidate check below.
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

        // Refresh the cached best anchor when it leaves the reference window.
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
            // The cached pair score belonged to the previous best anchor.
            global_pair_score_cached = false;
        }
        if (global_best_idx >= 0 && global_best_idx < search_end) {
            // Score the extra candidate if the main search did not save its score.
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

/**
 * @file chaining_simd_backtracking.cppm
 * @brief SIMD predecessor-path ranking and chain materialization.
 */
module;

#include <hwy/highway.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

export module bioseq.highway.sources.chaining_backtracking;

import std;
import bioseq.common.context;
import bioseq.common.logging;
import bioseq.instrumentation;
import bioseq.chaining.simd;
import bioseq.chaining.core;
import bioseq.highway.sources.chaining_candidate_scoring;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

using bioseq::chaining::anchor;
using bioseq::chaining::arena_slab;
using bioseq::chaining::chain;
using bioseq::chaining::chaining_parameters;
using bioseq::chaining::simd_chaining_execution_stats;
using bioseq::chaining::simd_chaining_policy_stats;
using bioseq::chaining::simd_chaining_policy;
using bioseq::chaining::simd_chaining_runtime_error;
using bioseq::chaining::simd_chaining_status;

namespace chaining_simd_detail {

/// @brief Couples a chain score with its source anchor index.
struct scored_anchor_index {
    std::int32_t score;
    std::int32_t anchor_index;
};
static_assert(sizeof(scored_anchor_index) == 8u);

/// @brief Orders scored anchors by score and then source index.
struct score_ascending {
    /// @brief Compares two scored indices for ascending processing order.
    [[nodiscard]] bool operator()(
        const scored_anchor_index& lhs,
        const scored_anchor_index& rhs) const noexcept
    {
        return lhs.score != rhs.score
            ? lhs.score < rhs.score
            : lhs.anchor_index < rhs.anchor_index;
    }
};

/// @brief Builds chain paths from SIMD-produced predecessor arrays.
/// @tparam p_collect_stats Whether this instantiation records counters.
/// @tparam p_descriptor_type Integer Highway descriptor type.
/// @param anchors Anchors corresponding to the score and predecessor arrays.
/// @param best_scores Best score ending at each anchor.
/// @param predecessors Predecessor index for each anchor.
/// @param min_score Minimum terminal score to consider.
/// @param min_seed_count Minimum anchors required for an emitted chain.
/// @param chains_out Arena-backed destination for chain descriptors.
/// @param reordered_anchors_out Arena-backed destination for anchors in chain order.
/// @param stats Optional full execution counters.
/// @param route_summary Optional lightweight route counters.
template<bool p_collect_stats, class p_descriptor_type>
void backtrack_chains_simd(
    p_descriptor_type d,
    std::span<const anchor> anchors,
    std::span<const std::int32_t> best_scores,
    std::span<const std::int32_t> predecessors,
    std::int32_t min_score,
    std::int32_t min_seed_count,
    arena_slab<chain>& chains_out,
    arena_slab<anchor>& reordered_anchors_out,
    simd_chaining_execution_stats* stats,
    simd_chaining_route_stats* route_summary)
{
    const std::int32_t anchor_count =
        static_cast<std::int32_t>(anchors.size());
    const std::int32_t lanes =
        static_cast<std::int32_t>(hn::Lanes(d));
    auto& ctx = context::current();

    auto scored = ctx.alloc_chain_array<scored_anchor_index>(
        anchors.size());
    std::size_t scored_count = 0;
    for (std::int32_t i = 0; i < anchor_count; ++i) {
        if (best_scores[i] >= min_score) {
            scored[scored_count++] =
                scored_anchor_index{best_scores[i], i};
        }
    }
    std::sort(
        scored.data(), scored.data() + scored_count,
        score_ascending{});

    auto visited = ctx.alloc_chain_array<std::int32_t>(
        anchors.size());
    std::memset(
        visited.data(), 0,
        anchors.size() * sizeof(std::int32_t));

    HWY_ALIGN std::int32_t start_lanes[hn::MaxLanes(d)];
    HWY_ALIGN std::int32_t length_lanes[hn::MaxLanes(d)];
    HWY_ALIGN std::int32_t terminal_lanes[hn::MaxLanes(d)];
    const auto one = hn::Set(d, 1);
    const auto minus_one = hn::Set(d, -1);
    bool route_rank_work = false;
    bool route_path_work = false;
    bool route_commit_work = false;
    std::uint64_t route_scalar_orchestration_steps = 0;
    std::uint64_t route_primary_contiguous_batches = 0;
    std::uint64_t route_all_starts_visited_fast_batches = 0;

    // Lane zero is also the primary path scratch. Reusing the eventual
    // lane-batched matrix avoids retaining a separate N-element allocation
    // for the dominant path.
    const auto matrix_plan = bioseq::chaining::simd_chaining_path_elements(
        anchors.size(), static_cast<std::size_t>(lanes));
    if (!matrix_plan) {
        throw std::length_error{"SIMD chaining backtracking path storage overflowed"};
    }
    const std::size_t matrix_elements = *matrix_plan;
    auto paths = ctx.alloc_chain_array<std::int32_t, 64>(
        matrix_elements);

    std::int64_t highest_rank =
        static_cast<std::int64_t>(scored_count) - 1;

    // The first ranked path cannot overlap previously accepted work because
    // visited[] is empty.  Claim it while traversing instead of collecting four
    // near-identical long paths and replaying all four before the first commit.
    // Long p[j] == j - 1 runs use full SIMD load/store chunks below; irregular
    // links retain genuine masked SIMD gather/scatter. Neither route calls the
    // shared scalar backtracker.
    if (highest_rank >= 0) {
        std::int32_t* const primary_path = paths.data();
        std::int32_t primary_current =
            scored[highest_rank].anchor_index;
        auto current = hn::Set(d, primary_current);
        auto active = hn::FirstN(d, 1u);
        std::int32_t primary_length = 0;

        while (primary_length < anchor_count
               && !hn::AllFalse(d, active))
        {
            route_path_work = true;
            route_commit_work = true;

            // HiFi primary chains commonly follow p[j] == j - 1 for long
            // runs. Validate a complete run with one SIMD comparison, then
            // append and claim all lanes with contiguous vector stores. The
            // fallback below retains the exact masked gather/scatter path for
            // every non-contiguous predecessor layout.
            if (primary_current >= lanes - 1
                && primary_length <= anchor_count - lanes)
            {
                const std::int32_t run_start =
                    primary_current - lanes + 1;
                const auto run_predecessors = hn::LoadU(
                    d, predecessors.data() + run_start);
                const auto expected_predecessors = hn::Iota(
                    d, run_start - 1);
                if (hn::AllTrue(
                        d,
                        hn::Eq(
                            run_predecessors,
                            expected_predecessors)))
                {
                    const auto descending_indices = hn::Reverse(
                        d, hn::Iota(d, run_start));
                    hn::StoreU(
                        descending_indices,
                        d,
                        primary_path + primary_length);
                    hn::StoreU(one, d, visited.data() + run_start);

                    primary_length += lanes;
                    primary_current = run_start - 1;
                    active = primary_current >= 0
                        ? hn::FirstN(d, 1u)
                        : hn::MaskFalse(d);
                    current = primary_current >= 0
                        ? hn::Set(d, primary_current)
                        : minus_one;

                    stats_add<p_collect_stats>(
                        stats,
                        &simd_chaining_execution_stats::
                            backtrack_path_vector_batches);
                    stats_add<p_collect_stats>(
                        stats,
                        &simd_chaining_execution_stats::
                            backtrack_commit_vector_batches);
                    stats_add<p_collect_stats>(
                        stats,
                        &simd_chaining_execution_stats::
                            backtrack_vector_lanes,
                        static_cast<std::uint64_t>(lanes));
                    stats_add<p_collect_stats>(
                        stats,
                        &simd_chaining_execution_stats::
                            backtrack_primary_contiguous_batches);
                    ++route_primary_contiguous_batches;
                    continue;
                }
            }

            hn::StoreN(
                current, d, primary_path + primary_length, 1u);
            // Generic Highway gathers validate masked-off indices too.
            const auto safe_current = hn::IfThenElse(
                active, current, hn::Zero(d));
            hn::MaskedScatterIndex(
                one, active, d, visited.data(), safe_current);
            const auto next = hn::MaskedGatherIndex(
                active, d, predecessors.data(), safe_current);
            active = hn::And(active, hn::Gt(next, minus_one));
            current = hn::IfThenElse(active, next, minus_one);
            primary_current = hn::GetLane(next);
            ++primary_length;

            stats_add<p_collect_stats>(
                stats,
                &simd_chaining_execution_stats::
                    backtrack_path_vector_batches);
            stats_add<p_collect_stats>(
                stats,
                &simd_chaining_execution_stats::
                    backtrack_commit_vector_batches);
            stats_add<p_collect_stats>(
                stats,
                &simd_chaining_execution_stats::backtrack_vector_lanes);
        }
        if (!hn::AllFalse(d, active)) {
            reject(
                stats,
                simd_chaining_status::cyclic_predecessors,
                "primary predecessor path exceeds anchor count");
        }

        stats_add<p_collect_stats>(
            stats,
            &simd_chaining_execution_stats::backtrack_rank_batches);
        route_rank_work = true;
        const std::int32_t primary_score = scored[highest_rank].score;
        if (primary_score >= min_score
            && primary_length >= min_seed_count)
        {
            chains_out.push_back(chain{
                .score = primary_score,
                .anchor_count = primary_length,
                .anchor_start = static_cast<std::int32_t>(
                    reordered_anchors_out.size()),
            });
            for (std::int32_t i = primary_length; i > 0; --i) {
                reordered_anchors_out.push_back(
                    anchors[primary_path[i - 1]]);
            }
        }
        --highest_rank;
    }

    const auto lane_bases = hn::Mul(
        hn::Iota(d, 0), hn::Set(d, anchor_count));

    while (highest_rank >= 0) {
        const std::int32_t batch_count =
            std::min<std::int32_t>(
                lanes,
                static_cast<std::int32_t>(highest_rank + 1));
        for (std::int32_t lane = 0; lane < lanes; ++lane) {
            start_lanes[lane] = lane < batch_count
                ? static_cast<std::int32_t>(
                      scored[highest_rank - lane].anchor_index)
                : -1;
        }

        auto current = hn::Load(d, start_lanes);
        auto active = hn::FirstN(
            d, static_cast<std::size_t>(batch_count));
        auto lengths = hn::Zero(d);
        auto terminals = minus_one;
        std::int32_t depth = 0;
        bool all_starts_visited_batch_rejected = false;

        for (; depth < anchor_count && !hn::AllFalse(d, active);
             ++depth)
        {
            route_path_work = true;
            const auto safe_indices = hn::IfThenElse(
                active, current, hn::Zero(d));
            const auto seen = hn::MaskedGatherIndex(
                active, d, visited.data(), safe_indices);
            const auto hit_visited = hn::And(
                active, hn::Ne(seen, hn::Zero(d)));
            const std::uint64_t visited_hit_count =
                static_cast<std::uint64_t>(
                    hn::CountTrue(d, hit_visited));
            stats_add<p_collect_stats>(
                stats,
                &simd_chaining_execution_stats::backtrack_overlap_stops,
                visited_hit_count);

            // After the primary chain is claimed, a ranked batch commonly
            // starts entirely inside that chain.  The exact path below would
            // stop all lanes at depth zero, then reject each zero-length,
            // zero-score chain.  When either threshold rejects that outcome,
            // preserve the same stats-visible rank/path work and skip the
            // otherwise empty per-lane commit loop.
            if (depth == 0
                && visited_hit_count
                    == static_cast<std::uint64_t>(batch_count)
                && (min_score > 0 || min_seed_count > 0))
            {
                stats_add<p_collect_stats>(
                    stats,
                    &simd_chaining_execution_stats::
                        backtrack_path_vector_batches);
                stats_add<p_collect_stats>(
                    stats,
                    &simd_chaining_execution_stats::backtrack_rank_batches);
                stats_add<p_collect_stats>(
                    stats,
                    &simd_chaining_execution_stats::
                        backtrack_all_starts_visited_fast_batches);
                ++route_all_starts_visited_fast_batches;
                route_rank_work = true;
                highest_rank -= batch_count;
                active = hn::MaskFalse(d);
                all_starts_visited_batch_rejected = true;
                break;
            }
            terminals = hn::IfThenElse(
                hit_visited, current, terminals);
            const auto append = hn::AndNot(hit_visited, active);
            const auto path_offsets =
                hn::Add(lane_bases, hn::Set(d, depth));
            hn::MaskedScatterIndex(
                current, append, d, paths.data(), path_offsets);
            lengths = hn::Add(
                lengths, hn::IfThenElse(append, one, hn::Zero(d)));

            const auto gather_indices = hn::IfThenElse(
                append, current, hn::Zero(d));
            const auto next = hn::MaskedGatherIndex(
                append, d, predecessors.data(), gather_indices);
            const auto next_active = hn::And(
                append, hn::Gt(next, minus_one));
            current = hn::IfThenElse(
                next_active, next, minus_one);
            active = next_active;

            stats_add<p_collect_stats>(
                stats,
                &simd_chaining_execution_stats::
                    backtrack_path_vector_batches);
            stats_add<p_collect_stats>(
                stats,
                &simd_chaining_execution_stats::backtrack_vector_lanes,
                hn::CountTrue(d, append));
        }
        if (all_starts_visited_batch_rejected) {
            continue;
        }
        if (!hn::AllFalse(d, active)) {
            reject(
                stats,
                simd_chaining_status::cyclic_predecessors,
                "predecessor path exceeds anchor count");
        }

        hn::Store(lengths, d, length_lanes);
        hn::Store(terminals, d, terminal_lanes);
        stats_add<p_collect_stats>(
            stats,
            &simd_chaining_execution_stats::backtrack_rank_batches);
        route_rank_work = true;

        // Rank order is semantically observable when paths overlap.  Each
        // lane therefore commits after all higher-priority lanes in this
        // batch, but every path prefix is tested and marked in SIMD chunks.
        for (std::int32_t lane = 0; lane < batch_count; ++lane) {
            const std::int32_t collected_length = length_lanes[lane];
            const std::int32_t* lane_path =
                paths.data()
                + static_cast<std::size_t>(lane) * anchors.size();
            std::int32_t committed_length = 0;
            std::int32_t terminal = terminal_lanes[lane];

            for (std::int32_t offset = 0;
                 offset < collected_length;)
            {
                route_commit_work = true;
                const std::int32_t active_lanes =
                    std::min(lanes, collected_length - offset);
                const auto valid = hn::FirstN(
                    d, static_cast<std::size_t>(active_lanes));
                const auto path_indices = hn::MaskedLoad(
                    valid, d, lane_path + offset);
                const auto safe_indices = hn::IfThenElse(
                    valid, path_indices, hn::Zero(d));
                const auto seen = hn::MaskedGatherIndex(
                    valid, d, visited.data(), safe_indices);
                const auto hit = hn::And(
                    valid, hn::Ne(seen, hn::Zero(d)));
                const std::intptr_t first_hit =
                    hn::FindFirstTrue(d, hit);
                const std::int32_t commit_count =
                    first_hit >= 0
                    ? static_cast<std::int32_t>(first_hit)
                    : active_lanes;
                const auto commit = hn::FirstN(
                    d, static_cast<std::size_t>(commit_count));
                hn::MaskedScatterIndex(
                    one, commit, d, visited.data(), safe_indices);

                committed_length += commit_count;
                stats_add<p_collect_stats>(
                    stats,
                    &simd_chaining_execution_stats::
                        backtrack_commit_vector_batches);
                stats_add<p_collect_stats>(
                    stats,
                    &simd_chaining_execution_stats::scalar_orchestration_steps);
                ++route_scalar_orchestration_steps;
                if (first_hit >= 0) {
                    terminal = lane_path[offset + first_hit];
                    stats_add<p_collect_stats>(
                        stats,
                        &simd_chaining_execution_stats::
                            backtrack_overlap_stops);
                    break;
                }
                offset += active_lanes;
            }

            const std::int64_t rank = highest_rank - lane;
            const std::int32_t chain_score =
                terminal < 0
                ? scored[rank].score
                : scored[rank].score - best_scores[terminal];
            if (chain_score >= min_score
                && committed_length >= min_seed_count)
            {
                chains_out.push_back(chain{
                    .score = chain_score,
                    .anchor_count = committed_length,
                    .anchor_start = static_cast<std::int32_t>(
                        reordered_anchors_out.size()),
                });
                for (std::int32_t i = committed_length;
                     i > 0; --i)
                {
                    reordered_anchors_out.push_back(
                        anchors[lane_path[i - 1]]);
                }
            }
        }
        highest_rank -= batch_count;
    }
    if (route_summary != nullptr) {
        route_summary->backtrack_rank_work = route_rank_work;
        route_summary->backtrack_path_vector_work = route_path_work;
        route_summary->backtrack_commit_vector_work = route_commit_work;
        route_summary->backtrack_primary_contiguous_batches +=
            route_primary_contiguous_batches;
        route_summary->backtrack_all_starts_visited_fast_batches +=
            route_all_starts_visited_fast_batches;
        route_summary->scalar_orchestration_steps +=
            route_scalar_orchestration_steps;
    }
}


}  // namespace chaining_simd_detail

}  // namespace target
}  // namespace bioseq::simd

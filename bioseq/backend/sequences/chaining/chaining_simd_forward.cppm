/**
 * @file chaining_simd_forward.cppm
 * @brief Exact and bounded SIMD forward recurrences for chaining.
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

export module bioseq.highway.sources.chaining_forward;

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

/// @brief Runs exact visited-set dynamic programming for every anchor.
/// @tparam p_collect_stats Whether this instantiation records counters.
/// @tparam p_descriptor_type Integer Highway descriptor type.
/// @param anchors Canonically ordered anchors.
/// @param soa Structure-of-arrays coordinates for vector loads.
/// @param best_scores Destination score for every anchor.
/// @param predecessors Destination predecessor index for every anchor.
/// @param peak_scores Destination peak score for every anchor.
/// @param params Chaining distances, penalties, and skip limits.
/// @param stats Optional full execution counters.
/// @param route_summary Optional lightweight route counters.
template<bool p_collect_stats, class p_descriptor_type>
void simd_dp_forward_pass_exact(
    p_descriptor_type d,
    std::span<const anchor> anchors,
    const soa_anchors& soa,
    std::span<std::int32_t> best_scores,
    std::span<std::int32_t> predecessors,
    std::span<std::int32_t> peak_scores,
    const chaining_parameters& params,
    simd_chaining_execution_stats* stats,
    simd_chaining_route_stats* route_summary)
{
    const std::int32_t anchor_count =
        static_cast<std::int32_t>(anchors.size());
    const std::int32_t lanes =
        static_cast<std::int32_t>(hn::Lanes(d));
    const std::int32_t max_ref_dist =
        std::max(params.max_gap, params.bandwidth);
    const std::int32_t max_query_dist =
        std::max(params.max_gap, params.bandwidth);

    auto visited_generation =
        context::current().alloc_chain_array<std::int32_t>(
            anchors.size());
    std::memset(
        visited_generation.data(), 0,
        anchors.size() * sizeof(std::int32_t));

    HWY_ALIGN std::int32_t score_lanes[hn::MaxLanes(d)];
    HWY_ALIGN std::int32_t visited_hit_lanes[hn::MaxLanes(d)];
    std::int32_t window_start = 0;
    std::int32_t global_best_idx = -1;
    bool route_score_vector_work = false;
    bool route_global_best_vector_work = false;
    std::uint64_t route_score_vector_batches = 0;
    std::uint64_t route_score_vector_lanes = 0;
    std::uint64_t route_score_masked_lanes = 0;
    std::uint64_t route_no_improvement_fast_batches = 0;
    std::uint64_t route_improvement_replay_batches = 0;
    std::uint64_t route_improvement_replay_lanes = 0;
    std::uint64_t route_contiguous_predecessor_store_batches = 0;
    std::uint64_t route_scalar_orchestration_steps = 0;

    for (std::int32_t current_idx = 0;
         current_idx < anchor_count; ++current_idx)
    {
        std::int32_t best_predecessor = -1;
        std::int32_t best_score =
            static_cast<std::int32_t>(anchors[current_idx].query_span);
        std::int64_t skip_count = 0;

        while (window_start < current_idx
               && (anchors[current_idx].strand_partition_key()
                       != anchors[window_start].strand_partition_key()
                   || anchors[current_idx].reference_sort_key()
                       > anchors[window_start].reference_sort_key()
                           + static_cast<std::uint64_t>(max_ref_dist)))
        {
            ++window_start;
        }
        if (current_idx - window_start > params.max_chain_iter) {
            window_start = current_idx - params.max_chain_iter;
        }

        std::int32_t search_end = window_start;
        std::int32_t cached_global_pair_score =
            std::numeric_limits<std::int32_t>::min();
        bool global_pair_score_cached = false;
        bool stopped = false;

        for (std::int32_t candidate_end = current_idx;
             candidate_end > window_start && !stopped;)
        {
            route_score_vector_work = true;
            const std::int32_t active_lanes =
                std::min(lanes, candidate_end - window_start);
            const std::int32_t load_start =
                candidate_end - active_lanes;
            const auto scores = score_candidate_batch<p_collect_stats>(
                d,
                static_cast<std::size_t>(active_lanes),
                load_start,
                current_idx,
                soa,
                max_ref_dist,
                max_query_dist,
                params,
                stats);
            ++route_score_vector_batches;
            route_score_vector_lanes +=
                static_cast<std::uint64_t>(active_lanes);
            route_score_masked_lanes +=
                static_cast<std::uint64_t>(lanes - active_lanes);
            const auto active = hn::FirstN(
                d, static_cast<std::size_t>(active_lanes));
            const bool is_full_batch = active_lanes == lanes;
            const auto predecessor_scores = is_full_batch
                ? hn::LoadU(d, best_scores.data() + load_start)
                : hn::MaskedLoad(
                      active, d, best_scores.data() + load_start);
            const auto predecessor_indices = is_full_batch
                ? hn::LoadU(d, predecessors.data() + load_start)
                : hn::MaskedLoad(
                      active, d, predecessors.data() + load_start);
            const auto invalid_score = hn::Set(
                d, std::numeric_limits<std::int32_t>::min());
            const auto valid_scores = hn::And(
                active, hn::Ne(scores, invalid_score));

            // p[j] is always older than j. Therefore marking every valid
            // predecessor in this batch before examining candidates produces
            // the same generation observed by the descending scalar order:
            // only a higher candidate can mark a lower candidate. Marks made
            // below an eventual stop are harmless because this generation is
            // never queried again after the stop.
            const auto predecessor_marks = hn::And(
                valid_scores,
                hn::Gt(predecessor_indices, hn::Set(d, -1)));
            const auto expected_contiguous_predecessors = hn::Iota(
                d, load_start - 1);
            const bool contiguous_predecessor_run = load_start > 0
                && hn::AllTrue(
                    d,
                    hn::And(
                        valid_scores,
                        hn::Eq(
                            predecessor_indices,
                            expected_contiguous_predecessors)));
            if (contiguous_predecessor_run) {
                // NEON lowers MaskedScatterIndex to one extract/branch/store
                // sequence per lane.  The common diagonal p[j] == j - 1 run
                // has contiguous destinations, so one real vector store is
                // exactly equivalent and leaves the general scatter path
                // untouched for irregular or invalid candidates.
                hn::StoreN(
                    hn::Set(d, current_idx),
                    d,
                    visited_generation.data() + load_start - 1,
                    static_cast<std::size_t>(active_lanes));
                stats_add<p_collect_stats>(
                    stats,
                    &simd_chaining_execution_stats::visited_marks,
                    static_cast<std::uint64_t>(active_lanes));
                stats_add<p_collect_stats>(
                    stats,
                    &simd_chaining_execution_stats::
                        contiguous_predecessor_store_batches);
                ++route_contiguous_predecessor_store_batches;
            } else {
                hn::MaskedScatterIndex(
                    hn::Set(d, current_idx),
                    predecessor_marks,
                    d,
                    visited_generation.data(),
                    predecessor_indices);
                if constexpr (p_collect_stats) {
                    stats_add<true>(
                        stats,
                        &simd_chaining_execution_stats::visited_marks,
                        hn::CountTrue(d, predecessor_marks));
                }
            }

            const auto generations = is_full_batch
                ? hn::LoadU(
                      d, visited_generation.data() + load_start)
                : hn::MaskedLoad(
                      active,
                      d,
                      visited_generation.data() + load_start);
            const auto visited_hits = hn::And(
                valid_scores,
                hn::Eq(generations, hn::Set(d, current_idx)));
            const auto total_scores = hn::IfThenElse(
                valid_scores,
                hn::Add(scores, predecessor_scores),
                invalid_score);
            const auto can_improve = hn::And(
                valid_scores,
                hn::Gt(total_scores, hn::Set(d, best_score)));
            stats_add<p_collect_stats>(
                stats,
                &simd_chaining_execution_stats::descending_candidate_steps,
                static_cast<std::uint64_t>(active_lanes));

            if (hn::AllFalse(d, can_improve)) {
                // best_score can only increase, so if no lane beats it at
                // batch entry no lane can improve later in this batch. Exact
                // max-skip state is then just the number of visited hits.
                stats_add<p_collect_stats>(
                    stats,
                    &simd_chaining_execution_stats::
                        no_improvement_fast_batches);
                ++route_no_improvement_fast_batches;
                const std::int64_t hit_count =
                    static_cast<std::int64_t>(
                        hn::CountTrue(d, visited_hits));
                const std::int64_t max_skip =
                    static_cast<std::int64_t>(
                        params.max_chain_skip);
                if (skip_count + hit_count > max_skip) {
                    hn::StoreN(
                        hn::IfThenElse(
                            visited_hits,
                            hn::Set(d, 1),
                            hn::Zero(d)),
                        d,
                        visited_hit_lanes,
                        static_cast<std::size_t>(active_lanes));
                    std::int64_t hits_until_stop =
                        max_skip - skip_count + 1;
                    std::int32_t stop_lane = -1;
                    for (std::int32_t lane = active_lanes - 1;
                         lane >= 0; --lane)
                    {
                        if (visited_hit_lanes[lane] == 0) {
                            continue;
                        }
                        if (--hits_until_stop == 0) {
                            stop_lane = lane;
                            search_end = load_start + lane;
                            stopped = true;
                            break;
                        }
                    }
                    const std::uint64_t processed_lanes =
                        static_cast<std::uint64_t>(
                            stop_lane >= 0
                                ? active_lanes - stop_lane
                                : active_lanes);
                    route_scalar_orchestration_steps += processed_lanes;
                    stats_add<p_collect_stats>(
                        stats,
                        &simd_chaining_execution_stats::
                            scalar_orchestration_steps,
                        processed_lanes);
                    stats_add<p_collect_stats>(
                        stats,
                        &simd_chaining_execution_stats::
                            max_chain_skip_stops);
                } else {
                    skip_count += hit_count;
                }
            } else {
                // At least one vector-produced biological score may change
                // scalar state. Replay only this batch's control decisions in
                // exact descending order; no scalar score implementation is
                // called.
                stats_add<p_collect_stats>(
                    stats,
                    &simd_chaining_execution_stats::
                        improvement_replay_batches);
                ++route_improvement_replay_batches;
                stats_add<p_collect_stats>(
                    stats,
                    &simd_chaining_execution_stats::
                        improvement_replay_lanes,
                    static_cast<std::uint64_t>(active_lanes));
                route_improvement_replay_lanes +=
                    static_cast<std::uint64_t>(active_lanes);
                hn::StoreN(
                    scores,
                    d,
                    score_lanes,
                    static_cast<std::size_t>(active_lanes));
                hn::StoreN(
                    hn::IfThenElse(
                        visited_hits,
                        hn::Set(d, 1),
                        hn::Zero(d)),
                    d,
                    visited_hit_lanes,
                    static_cast<std::size_t>(active_lanes));

                std::int32_t processed_lanes = active_lanes;
                for (std::int32_t lane = active_lanes - 1;
                     lane >= 0; --lane)
                {
                    const std::int32_t pair_score = score_lanes[lane];
                    if (pair_score
                        == std::numeric_limits<std::int32_t>::min())
                    {
                        continue;
                    }
                    const std::int32_t candidate_idx =
                        load_start + lane;
                    if (candidate_idx == global_best_idx) {
                        cached_global_pair_score = pair_score;
                        global_pair_score_cached = true;
                    }

                    const std::int32_t total_score =
                        pair_score + best_scores[candidate_idx];
                    if (total_score > best_score) {
                        best_score = total_score;
                        best_predecessor = candidate_idx;
                        if (skip_count > 0) {
                            --skip_count;
                        }
                    } else if (visited_hit_lanes[lane] != 0) {
                        if (++skip_count
                            > static_cast<std::int64_t>(
                                params.max_chain_skip))
                        {
                            search_end = candidate_idx;
                            stats_add<p_collect_stats>(
                                stats,
                                &simd_chaining_execution_stats::
                                    max_chain_skip_stops);
                            processed_lanes = active_lanes - lane;
                            stopped = true;
                            break;
                        }
                    }
                }
                const std::uint64_t processed_lane_count =
                    static_cast<std::uint64_t>(processed_lanes);
                route_scalar_orchestration_steps +=
                    processed_lane_count;
                stats_add<p_collect_stats>(
                    stats,
                    &simd_chaining_execution_stats::
                        scalar_orchestration_steps,
                    processed_lane_count);
            }
            candidate_end = load_start;
        }

        if (global_best_idx < 0
            || anchors[current_idx].reference_sort_key()
                    - anchors[global_best_idx].reference_sort_key()
                > static_cast<std::uint64_t>(max_ref_dist))
        {
            route_global_best_vector_work =
                route_global_best_vector_work || window_start < current_idx;
            global_best_idx = exact_global_best<p_collect_stats>(
                d, best_scores.data(), window_start, current_idx, stats);
            global_pair_score_cached = false;
        }

        if (global_best_idx >= 0 && global_best_idx < search_end) {
            route_score_vector_work =
                route_score_vector_work || !global_pair_score_cached;
            if (!global_pair_score_cached) {
                ++route_score_vector_batches;
                ++route_score_vector_lanes;
                route_score_masked_lanes +=
                    static_cast<std::uint64_t>(lanes - 1);
            }
            const std::int32_t global_pair_score =
                global_pair_score_cached
                ? cached_global_pair_score
                : score_one_candidate<p_collect_stats>(
                      d,
                      global_best_idx,
                      current_idx,
                      soa,
                      max_ref_dist,
                      max_query_dist,
                      params,
                      stats);
            if (global_pair_score
                    != std::numeric_limits<std::int32_t>::min()
                && best_score
                    < global_pair_score + best_scores[global_best_idx])
            {
                best_score =
                    global_pair_score + best_scores[global_best_idx];
                best_predecessor = global_best_idx;
            }
        }

        best_scores[current_idx] = best_score;
        predecessors[current_idx] = best_predecessor;
        peak_scores[current_idx] =
            best_predecessor >= 0
                    && peak_scores[best_predecessor] > best_score
                ? peak_scores[best_predecessor]
                : best_score;

        if (global_best_idx < 0
            || (anchors[current_idx].reference_sort_key()
                        - anchors[global_best_idx].reference_sort_key()
                    <= static_cast<std::uint64_t>(max_ref_dist)
                && best_scores[global_best_idx]
                    < best_scores[current_idx]))
        {
            global_best_idx = current_idx;
        }
    }
    if (route_summary != nullptr) {
        route_summary->score_vector_work = route_score_vector_work;
        route_summary->global_best_vector_work =
            route_global_best_vector_work;
        route_summary->score_vector_batches =
            route_score_vector_batches;
        route_summary->score_vector_lanes = route_score_vector_lanes;
        route_summary->score_masked_lanes = route_score_masked_lanes;
        route_summary->no_improvement_fast_batches =
            route_no_improvement_fast_batches;
        route_summary->improvement_replay_batches =
            route_improvement_replay_batches;
        route_summary->improvement_replay_lanes =
            route_improvement_replay_lanes;
        route_summary->contiguous_predecessor_store_batches =
            route_contiguous_predecessor_store_batches;
        route_summary->scalar_orchestration_steps =
            route_scalar_orchestration_steps;
    }
}

/// Explicitly approximate forward chaining. Every biological pair score,
/// including masked tails and the one-candidate rescue, is produced by
/// the specialized SIMD batch scorer. Scalar work is limited to batch
/// orchestration and extracting the vector-reduced winning predecessor.
/// @tparam p_descriptor_type Integer Highway descriptor type.
/// @return Mask of lanes that improved their retained winner.
template<class p_descriptor_type>
HWY_INLINE hn::Mask<p_descriptor_type> update_bounded_lane_best(
    p_descriptor_type d,
    hn::Vec<p_descriptor_type> pair_scores,
    hn::Vec<p_descriptor_type> predecessor_scores,
    hn::Mask<p_descriptor_type> valid,
    std::int32_t load_start,
    hn::Vec<p_descriptor_type>& lane_best_scores,
    hn::Vec<p_descriptor_type>& lane_best_predecessors)
{
    const auto invalid_score = hn::Set(
        d, std::numeric_limits<std::int32_t>::min());
    const auto total_scores = hn::IfThenElse(
        valid,
        hn::Add(pair_scores, predecessor_scores),
        invalid_score);
    const auto improved = hn::And(
        valid, hn::Gt(total_scores, lane_best_scores));
    const auto predecessor_indices = hn::Add(
        hn::Set(d, load_start), hn::Iota(d, 0));
    lane_best_scores = hn::IfThenElse(
        improved, total_scores, lane_best_scores);
    lane_best_predecessors = hn::IfThenElse(
        improved,
        predecessor_indices,
        lane_best_predecessors);
    return improved;
}

/// @brief Scores and folds one bounded predecessor batch into lane winners.
/// @tparam p_collect_stats Whether this instantiation records counters.
/// @tparam p_full_batch Whether every physical lane contains a candidate.
/// @tparam p_has_skip_penalty Whether skip-penalty arithmetic is enabled.
/// @tparam p_descriptor_type Integer Highway descriptor type.
/// @return Mask of lanes that improved their retained winner.
template<bool p_collect_stats, bool p_full_batch, bool p_has_skip_penalty, class p_descriptor_type>
HWY_INLINE hn::Mask<p_descriptor_type> score_bounded_candidate_batch(
    p_descriptor_type d,
    std::size_t active_lanes,
    std::int32_t load_start,
    std::int32_t current_idx,
    const soa_anchors& soa,
    std::span<const std::int32_t> best_scores,
    std::int32_t max_ref_dist,
    std::int32_t max_query_dist,
    const chaining_parameters& params,
    hn::Vec<p_descriptor_type>& lane_best_scores,
    hn::Vec<p_descriptor_type>& lane_best_predecessors,
    simd_chaining_execution_stats* stats)
{
    const auto pair_scores = score_candidate_batch_specialized<
        p_collect_stats, p_full_batch, p_has_skip_penalty>(
        d,
        active_lanes,
        load_start,
        current_idx,
        soa,
        max_ref_dist,
        max_query_dist,
        params,
        stats);
    const auto invalid_score = hn::Set(
        d, std::numeric_limits<std::int32_t>::min());
    if constexpr (p_full_batch) {
        const auto predecessor_scores = hn::LoadU(
            d, best_scores.data() + load_start);
        const auto valid = hn::Ne(pair_scores, invalid_score);
        return update_bounded_lane_best(
            d,
            pair_scores,
            predecessor_scores,
            valid,
            load_start,
            lane_best_scores,
            lane_best_predecessors);
    } else {
        const auto active = hn::FirstN(d, active_lanes);
        const auto predecessor_scores = hn::MaskedLoad(
            active, d, best_scores.data() + load_start);
        const auto valid = hn::And(
            active, hn::Ne(pair_scores, invalid_score));
        return update_bounded_lane_best(
            d,
            pair_scores,
            predecessor_scores,
            valid,
            load_start,
            lane_best_scores,
            lane_best_predecessors);
    }
}

/// @brief Runs bounded-batch dynamic programming for one penalty specialization.
/// @tparam p_collect_stats Whether this instantiation records counters.
/// @tparam p_has_skip_penalty Whether skip-penalty arithmetic is enabled.
/// @tparam p_descriptor_type Integer Highway descriptor type.
template<bool p_collect_stats, bool p_has_skip_penalty, class p_descriptor_type>
void simd_dp_forward_pass_bounded_impl(
    p_descriptor_type d,
    std::span<const anchor> anchors,
    const soa_anchors& soa,
    std::span<std::int32_t> best_scores,
    std::span<std::int32_t> predecessors,
    std::span<std::int32_t> peak_scores,
    const chaining_parameters& params,
    simd_chaining_execution_stats* stats,
    simd_chaining_route_stats* route_summary)
{
    const std::int32_t anchor_count =
        static_cast<std::int32_t>(anchors.size());
    const std::int32_t lanes =
        static_cast<std::int32_t>(hn::Lanes(d));
    const std::int32_t max_ref_dist =
        std::max(params.max_gap, params.bandwidth);
    const std::int32_t max_query_dist =
        std::max(params.max_gap, params.bandwidth);
    const std::int32_t nonimproving_batch_limit =
        std::max<std::int32_t>(
            1, params.max_chain_skip / lanes + 1);

    std::int32_t window_start = 0;
    std::int32_t global_best_idx = -1;
    bool route_score_vector_work = false;
    bool route_global_best_vector_work = false;
    std::uint64_t route_score_vector_batches = 0;
    std::uint64_t route_score_vector_lanes = 0;
    std::uint64_t route_score_masked_lanes = 0;
    std::uint64_t bounded_score_batches = 0;
    std::uint64_t bounded_improving_batches = 0;
    std::uint64_t bounded_nonimproving_batches = 0;
    std::uint64_t bounded_cutoff_stops = 0;
    std::uint64_t bounded_candidates_skipped = 0;
    std::uint64_t global_best_rescue_probes = 0;
    std::uint64_t global_best_rescue_wins = 0;

    for (std::int32_t current_idx = 0;
         current_idx < anchor_count; ++current_idx)
    {
        while (window_start < current_idx
               && (anchors[current_idx].strand_partition_key()
                       != anchors[window_start].strand_partition_key()
                   || anchors[current_idx].reference_sort_key()
                       > anchors[window_start].reference_sort_key()
                           + static_cast<std::uint64_t>(max_ref_dist)))
        {
            ++window_start;
        }
        if (current_idx - window_start > params.max_chain_iter) {
            window_start = current_idx - params.max_chain_iter;
        }

        auto lane_best_scores = hn::Set(
            d, static_cast<std::int32_t>(
                   anchors[current_idx].query_span));
        auto lane_best_predecessors = hn::Set(d, -1);
        std::int32_t consecutive_nonimproving = 0;
        // No unscanned interval unless a cutoff replaces this with a value
        // strictly greater than window_start.
        std::int32_t unscanned_end_exclusive = window_start;

        // Classifies one scored batch and applies the bounded non-improvement cutoff.
        auto record_batch = [&route_score_vector_batches,
                             &route_score_vector_lanes,
                             &route_score_masked_lanes,
                             &lanes,
                             &bounded_score_batches,
                             &stats,
                             &d,
                             &bounded_improving_batches,
                             &consecutive_nonimproving,
                             &bounded_nonimproving_batches,
                             &window_start,
                             &nonimproving_batch_limit,
                             &unscanned_end_exclusive,
                             &bounded_cutoff_stops,
                             &bounded_candidates_skipped](hn::Mask<p_descriptor_type> improved,
                                                          std::int32_t active_lanes,
                                                          std::int32_t load_start) {
            ++route_score_vector_batches;
            route_score_vector_lanes +=
                static_cast<std::uint64_t>(active_lanes);
            route_score_masked_lanes +=
                static_cast<std::uint64_t>(lanes - active_lanes);
            ++bounded_score_batches;
            stats_add<p_collect_stats>(
                stats,
                &simd_chaining_execution_stats::descending_candidate_steps,
                static_cast<std::uint64_t>(active_lanes));

            if (!hn::AllFalse(d, improved)) {
                ++bounded_improving_batches;
                consecutive_nonimproving = 0;
            } else {
                ++bounded_nonimproving_batches;
                ++consecutive_nonimproving;
                if (load_start > window_start
                    && consecutive_nonimproving
                        >= nonimproving_batch_limit)
                {
                    unscanned_end_exclusive = load_start;
                    ++bounded_cutoff_stops;
                    bounded_candidates_skipped +=
                        static_cast<std::uint64_t>(
                            load_start - window_start);
                    return true;
                }
            }
            return false;
        };

        std::int32_t candidate_end = current_idx;
        bool cutoff = false;
        while (candidate_end - window_start >= lanes) {
            route_score_vector_work = true;
            const std::int32_t load_start = candidate_end - lanes;
            const auto improved = score_bounded_candidate_batch<
                p_collect_stats, true, p_has_skip_penalty>(
                d,
                static_cast<std::size_t>(lanes),
                load_start,
                current_idx,
                soa,
                best_scores,
                max_ref_dist,
                max_query_dist,
                params,
                lane_best_scores,
                lane_best_predecessors,
                stats);
            cutoff = record_batch(improved, lanes, load_start);
            if (cutoff) {
                break;
            }
            candidate_end = load_start;
        }
        if (!cutoff && candidate_end > window_start) {
            route_score_vector_work = true;
            const std::int32_t active_lanes =
                candidate_end - window_start;
            const std::int32_t load_start = window_start;
            const auto improved = score_bounded_candidate_batch<
                p_collect_stats, false, p_has_skip_penalty>(
                d,
                static_cast<std::size_t>(active_lanes),
                load_start,
                current_idx,
                soa,
                best_scores,
                max_ref_dist,
                max_query_dist,
                params,
                lane_best_scores,
                lane_best_predecessors,
                stats);
            (void)record_batch(
                improved, active_lanes, load_start);
        }

        const auto maximum_score =
            hn::MaxOfLanes(d, lane_best_scores);
        const auto tied_predecessors = hn::IfThenElse(
            hn::Eq(lane_best_scores, maximum_score),
            lane_best_predecessors,
            hn::Set(d, -1));
        std::int32_t best_score = hn::GetLane(maximum_score);
        std::int32_t best_predecessor = hn::GetLane(
            hn::MaxOfLanes(d, tied_predecessors));
        if (best_score
            == static_cast<std::int32_t>(
                anchors[current_idx].query_span))
        {
            best_predecessor = -1;
        }

        bool global_best_invalid = global_best_idx < window_start;
        if (!global_best_invalid) {
            global_best_invalid =
                anchors[global_best_idx].strand_partition_key()
                    != anchors[current_idx].strand_partition_key()
                || anchors[current_idx].ref_position
                        - anchors[global_best_idx].ref_position
                    > static_cast<std::uint64_t>(max_ref_dist);
        }
        if (global_best_invalid) {
            route_global_best_vector_work =
                route_global_best_vector_work
                || window_start < current_idx;
            global_best_idx = exact_global_best<p_collect_stats>(
                d,
                best_scores.data(),
                window_start,
                current_idx,
                stats);
        }

        if (global_best_idx >= window_start
            && global_best_idx < unscanned_end_exclusive)
        {
            route_score_vector_work = true;
            ++route_score_vector_batches;
            ++route_score_vector_lanes;
            route_score_masked_lanes +=
                static_cast<std::uint64_t>(lanes - 1);
            ++global_best_rescue_probes;
            const std::int32_t global_pair_score =
                score_one_candidate<p_collect_stats, p_has_skip_penalty>(
                    d,
                    global_best_idx,
                    current_idx,
                    soa,
                    max_ref_dist,
                    max_query_dist,
                    params,
                    stats);
            if (global_pair_score
                    != std::numeric_limits<std::int32_t>::min()
                && global_pair_score + best_scores[global_best_idx]
                    > best_score)
            {
                best_score =
                    global_pair_score + best_scores[global_best_idx];
                best_predecessor = global_best_idx;
                ++global_best_rescue_wins;
            }
        }

        best_scores[current_idx] = best_score;
        predecessors[current_idx] = best_predecessor;
        peak_scores[current_idx] =
            best_predecessor >= 0
                    && peak_scores[best_predecessor] > best_score
                ? peak_scores[best_predecessor]
                : best_score;

        // The current anchor is the largest index and therefore replaces the
        // cached winner on an equal score as well as a strict improvement.
        if (global_best_idx < window_start
            || best_scores[current_idx]
                >= best_scores[global_best_idx])
        {
            global_best_idx = current_idx;
        }
    }

    if constexpr (p_collect_stats) {
        simd_chaining_policy_stats bounded_stats{
            .bounded_score_batches = bounded_score_batches,
            .bounded_improving_batches = bounded_improving_batches,
            .bounded_nonimproving_batches =
                bounded_nonimproving_batches,
            .bounded_cutoff_stops = bounded_cutoff_stops,
            .bounded_candidates_skipped = bounded_candidates_skipped,
            .global_best_rescue_probes = global_best_rescue_probes,
            .global_best_rescue_wins = global_best_rescue_wins,
        };
        stats->policy.merge(bounded_stats);
    }
    if (route_summary != nullptr) {
        route_summary->score_vector_work = route_score_vector_work;
        route_summary->global_best_vector_work =
            route_global_best_vector_work;
        route_summary->score_vector_batches =
            route_score_vector_batches;
        route_summary->score_vector_lanes = route_score_vector_lanes;
        route_summary->score_masked_lanes = route_score_masked_lanes;
        route_summary->bounded_score_batches = bounded_score_batches;
        route_summary->bounded_improving_batches =
            bounded_improving_batches;
        route_summary->bounded_nonimproving_batches =
            bounded_nonimproving_batches;
        route_summary->bounded_cutoff_stops = bounded_cutoff_stops;
        route_summary->bounded_candidates_skipped =
            bounded_candidates_skipped;
        route_summary->global_best_rescue_probes =
            global_best_rescue_probes;
        route_summary->global_best_rescue_wins =
            global_best_rescue_wins;
    }
}

/// @brief Selects the zero or nonzero skip-penalty bounded kernel.
/// @tparam p_collect_stats Whether this instantiation records counters.
/// @tparam p_descriptor_type Integer Highway descriptor type.
template<bool p_collect_stats, class p_descriptor_type>
void simd_dp_forward_pass_bounded(
    p_descriptor_type d,
    std::span<const anchor> anchors,
    const soa_anchors& soa,
    std::span<std::int32_t> best_scores,
    std::span<std::int32_t> predecessors,
    std::span<std::int32_t> peak_scores,
    const chaining_parameters& params,
    simd_chaining_execution_stats* stats,
    simd_chaining_route_stats* route_summary)
{
    // Select the arithmetic policy once, outside every anchor/candidate loop.
    // The zero-penalty specialization removes gap-length float work while the
    // nonzero specialization preserves the general scoring formula.
    if (params.skip_penalty == 0.0F) {
        simd_dp_forward_pass_bounded_impl<p_collect_stats, false>(
            d,
            anchors,
            soa,
            best_scores,
            predecessors,
            peak_scores,
            params,
            stats,
            route_summary);
    } else {
        simd_dp_forward_pass_bounded_impl<p_collect_stats, true>(
            d,
            anchors,
            soa,
            best_scores,
            predecessors,
            peak_scores,
            params,
            stats,
            route_summary);
    }
}

/// @brief Dispatches the selected policy outside the candidate loop.
///
/// Compile-time forward dispatch keeps the policy test outside the candidate
/// loop while sharing allocation, SoA conversion, and backtracking.
/// @tparam p_policy Forward-search policy selected at compile time.
/// @tparam p_collect_stats Whether this instantiation records counters.
/// @tparam p_descriptor_type Integer Highway descriptor type.
template<simd_chaining_policy p_policy, bool p_collect_stats, class p_descriptor_type>
void simd_dp_forward_pass(
    p_descriptor_type d,
    std::span<const anchor> anchors,
    const soa_anchors& soa,
    std::span<std::int32_t> best_scores,
    std::span<std::int32_t> predecessors,
    std::span<std::int32_t> peak_scores,
    const chaining_parameters& params,
    simd_chaining_execution_stats* stats,
    simd_chaining_route_stats* route_summary)
{
    if constexpr (p_policy == simd_chaining_policy::exact_visited) {
        simd_dp_forward_pass_exact<p_collect_stats>(
            d,
            anchors,
            soa,
            best_scores,
            predecessors,
            peak_scores,
            params,
            stats,
            route_summary);
    } else {
        static_assert(
            p_policy
                == simd_chaining_policy::bounded_batch_approximate);
        simd_dp_forward_pass_bounded<p_collect_stats>(
            d,
            anchors,
            soa,
            best_scores,
            predecessors,
            peak_scores,
            params,
            stats,
            route_summary);
    }
}


}  // namespace chaining_simd_detail

}  // namespace target
}  // namespace bioseq::simd

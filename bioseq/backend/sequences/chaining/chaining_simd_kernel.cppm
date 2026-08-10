/**
 * @file chaining_simd_kernel.cppm
 * @brief Canonical generator input for exact SIMD DP chaining.
 *
 * This module is scanned by bioseq_codegen and is not a final production
 * provider. The generator replaces the annotated `target` namespace with one
 * namespace per compiled Highway target and adds HWY_ATTR to every callable.
 *
 * Every biological score is evaluated by Highway vectors, including ranges
 * with one to five predecessors and final partial vectors. Batches that cannot
 * improve the current score update exact max-skip state from SIMD masks;
 * improvement-capable batches replay only scalar control in descending order.
 * chain extraction is local to this backend: predecessor paths are gathered
 * in lane batches and committed with vector visited-prefix operations in
 * strict score-rank order.
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

export module bioseq.highway.sources.chaining;

import std;
import bioseq.common.context;
import bioseq.common.logging;
import bioseq.instrumentation;
import bioseq.chaining.simd;
import bioseq.chaining.core;
import bioseq.highway.sources.simd_utils;

namespace bioseq::simd {
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

// Production attestation needs route stats without turning the O(n*iter)
// score loop into a stream of writes through a stats pointer. These local
// counters and flags live in the caller's frame, are updated only at the actual
// SIMD/control call sites, and are folded into simd_chaining_execution_stats once
// after the kernels return. Detailed stats mode remains available to tests.
struct simd_chaining_route_stats {
    bool score_vector_work = false;
    bool global_best_vector_work = false;
    bool backtrack_rank_work = false;
    bool backtrack_path_vector_work = false;
    bool backtrack_commit_vector_work = false;
    std::uint64_t score_vector_batches = 0;
    std::uint64_t score_vector_lanes = 0;
    std::uint64_t score_masked_lanes = 0;
    std::uint64_t no_improvement_fast_batches = 0;
    std::uint64_t improvement_replay_batches = 0;
    std::uint64_t improvement_replay_lanes = 0;
    std::uint64_t contiguous_predecessor_store_batches = 0;
    std::uint64_t backtrack_primary_contiguous_batches = 0;
    std::uint64_t backtrack_all_starts_visited_fast_batches = 0;
    std::uint64_t scalar_orchestration_steps = 0;
    std::uint64_t bounded_score_batches = 0;
    std::uint64_t bounded_improving_batches = 0;
    std::uint64_t bounded_nonimproving_batches = 0;
    std::uint64_t bounded_cutoff_stops = 0;
    std::uint64_t bounded_candidates_skipped = 0;
    std::uint64_t global_best_rescue_probes = 0;
    std::uint64_t global_best_rescue_wins = 0;
};

template<bool p_collect_stats>
HWY_INLINE void stats_add(
    simd_chaining_execution_stats* stats,
    std::uint64_t simd_chaining_execution_stats::* member,
    std::uint64_t value = 1) noexcept
{
    if constexpr (p_collect_stats) {
        stats->*member += value;
    } else {
        (void)stats;
        (void)member;
        (void)value;
    }
}

[[noreturn]] inline void reject(
    simd_chaining_execution_stats* stats,
    simd_chaining_status status,
    std::string_view detail)
{
    if (stats != nullptr) {
        ++stats->fail_closed_rejections;
        if (status == simd_chaining_status::invalid_policy) {
            ++stats->policy.policy_rejections;
        }
    }
    throw simd_chaining_runtime_error(status, detail);
}

struct soa_anchors {
    std::uint32_t* ref_positions = nullptr;
    std::uint32_t* query_positions = nullptr;
    std::uint32_t* query_spans = nullptr;
    std::size_t count = 0;

    [[nodiscard]] std::uint32_t* ref_data() noexcept {
        return ref_positions;
    }
    [[nodiscard]] std::uint32_t* query_data() noexcept {
        return query_positions;
    }
    [[nodiscard]] std::uint32_t* span_data() noexcept {
        return query_spans;
    }
    [[nodiscard]] const std::uint32_t* ref_data() const noexcept {
        return ref_positions;
    }
    [[nodiscard]] const std::uint32_t* query_data() const noexcept {
        return query_positions;
    }
    [[nodiscard]] const std::uint32_t* span_data() const noexcept {
        return query_spans;
    }
};

template<class p_descriptor_type>
[[nodiscard]] soa_anchors create_soa_anchors(
    p_descriptor_type,
    std::span<const anchor> anchors)
{
    const std::size_t count = anchors.size();
    auto storage =
        context::current().alloc_chain_array<std::uint32_t, 64>(3 * count);
    soa_anchors result{
        .ref_positions = storage.data(),
        .query_positions = storage.data() + count,
        .query_spans = storage.data() + 2 * count,
        .count = count,
    };
    for (std::size_t i = 0; i < count; ++i) {
        // Bounded same-range deltas are preserved modulo 2^32, including
        // references crossing the 4-Gbp boundary.
        result.ref_positions[i] =
            static_cast<std::uint32_t>(anchors[i].ref_position);
        result.query_positions[i] =
            static_cast<std::uint32_t>(anchors[i].query_position);
        result.query_spans[i] =
            static_cast<std::uint32_t>(anchors[i].query_span);
    }
    return result;
}

template<bool p_full_batch, bool p_has_skip_penalty, class p_descriptor_type, class p_float_descriptor_type>
HWY_INLINE hn::Vec<p_descriptor_type> vectorized_chaining_score(
    p_descriptor_type d,
    p_float_descriptor_type df,
    std::size_t active_lanes,
    hn::Vec<p_descriptor_type> current_ref,
    hn::Vec<p_descriptor_type> current_query,
    hn::Vec<p_descriptor_type> predecessor_ref,
    hn::Vec<p_descriptor_type> predecessor_query,
    hn::Vec<p_descriptor_type> predecessor_span,
    hn::Vec<p_descriptor_type> max_ref_dist,
    hn::Vec<p_descriptor_type> max_query_dist,
    hn::Vec<p_descriptor_type> bandwidth,
    hn::Vec<p_float_descriptor_type> gap_penalty,
    float skip_penalty)
{
    const auto zero = hn::Zero(d);
    const auto invalid_score =
        hn::Set(d, std::numeric_limits<std::int32_t>::min());
    const auto query_distance = hn::Sub(current_query, predecessor_query);
    const auto ref_distance = hn::Sub(current_ref, predecessor_ref);
    const auto diagonal_deviation =
        hn::Abs(hn::Sub(ref_distance, query_distance));
    const auto gap_length = hn::Min(ref_distance, query_distance);

    auto invalid = hn::Or(
        hn::Or(
            hn::Or(
                hn::Le(query_distance, zero),
                hn::Gt(query_distance, max_ref_dist)),
            hn::Eq(ref_distance, zero)),
        hn::Or(
            hn::Gt(query_distance, max_query_dist),
            hn::Gt(diagonal_deviation, bandwidth)));
    if constexpr (!p_full_batch) {
        const auto active = hn::FirstN(d, active_lanes);
        invalid = hn::Or(hn::Not(active), invalid);
    }

    auto score = hn::IfThenElse(
        invalid, invalid_score, hn::Min(predecessor_span, gap_length));
    const auto needs_penalty = hn::AndNot(
        invalid,
        hn::Or(
            hn::Ne(diagonal_deviation, zero),
            hn::Gt(gap_length, predecessor_span)));
    if (hn::AllFalse(d, needs_penalty)) {
        return score;
    }

    const auto deviation_f = hn::ConvertTo(df, diagonal_deviation);
    const auto linear_penalty = [&] {
        if constexpr (p_has_skip_penalty) {
            const auto gap_length_f = hn::ConvertTo(df, gap_length);
            // Same source ordering as scalar:
            // gap_penalty * diagonal_deviation
            //     + skip_penalty * gap_length.
            return hn::MulAdd(
                gap_penalty,
                deviation_f,
                hn::Mul(hn::Set(df, skip_penalty), gap_length_f));
        } else {
            // The common benchmark policy uses an exact zero skip penalty.
            // Selecting this specialization before the candidate loop avoids
            // converting gap lengths to float and multiplying them by zero.
            return hn::Mul(gap_penalty, deviation_f);
        }
    }();
    const auto raw_log2 = approximate_log2(
        d, df, hn::Add(diagonal_deviation, hn::Set(d, 1)));
    const auto deviation_positive_f = hn::RebindMask(
        df, hn::Gt(diagonal_deviation, zero));
    const auto logarithmic_penalty =
        hn::IfThenElseZero(deviation_positive_f, raw_log2);
    const auto penalty_f = hn::MulAdd(
        hn::Set(df, 0.5F), logarithmic_penalty, linear_penalty);
    const auto penalty_i = hn::IfThenElseZero(
        needs_penalty, hn::ConvertTo(d, penalty_f));
    return hn::Sub(score, penalty_i);
}

template<bool p_collect_stats, bool p_full_batch, bool p_has_skip_penalty, class p_descriptor_type>
HWY_INLINE hn::Vec<p_descriptor_type> score_candidate_batch_specialized(
    p_descriptor_type d,
    std::size_t active_lanes,
    std::int64_t load_start,
    std::int64_t current_idx,
    const soa_anchors& soa,
    std::int32_t max_ref_dist,
    std::int32_t max_query_dist,
    const chaining_parameters& params,
    simd_chaining_execution_stats* stats)
{
    using float_descriptor = hn::RebindToFloat<p_descriptor_type>;
    const float_descriptor df;
    const std::int32_t* predecessor_ref_ptr =
        reinterpret_cast<const std::int32_t*>(
            soa.ref_data() + load_start);
    const std::int32_t* predecessor_query_ptr =
        reinterpret_cast<const std::int32_t*>(
            soa.query_data() + load_start);
    const std::int32_t* predecessor_span_ptr =
        reinterpret_cast<const std::int32_t*>(
            soa.span_data() + load_start);
    const auto predecessor_ref = [&] {
        if constexpr (p_full_batch) {
            return hn::LoadU(d, predecessor_ref_ptr);
        } else {
            return hn::MaskedLoad(
                hn::FirstN(d, active_lanes), d, predecessor_ref_ptr);
        }
    }();
    const auto predecessor_query = [&] {
        if constexpr (p_full_batch) {
            return hn::LoadU(d, predecessor_query_ptr);
        } else {
            return hn::MaskedLoad(
                hn::FirstN(d, active_lanes), d, predecessor_query_ptr);
        }
    }();
    const auto predecessor_span = [&] {
        if constexpr (p_full_batch) {
            return hn::LoadU(d, predecessor_span_ptr);
        } else {
            return hn::MaskedLoad(
                hn::FirstN(d, active_lanes), d, predecessor_span_ptr);
        }
    }();
    const auto scores =
        vectorized_chaining_score<p_full_batch, p_has_skip_penalty>(
            d,
            df,
            active_lanes,
            hn::Set(d, static_cast<std::int32_t>(
                soa.ref_data()[current_idx])),
            hn::Set(d, static_cast<std::int32_t>(
                soa.query_data()[current_idx])),
            predecessor_ref,
            predecessor_query,
            predecessor_span,
            hn::Set(d, max_ref_dist),
            hn::Set(d, max_query_dist),
            hn::Set(d, params.bandwidth),
            hn::Set(df, params.gap_penalty),
            params.skip_penalty);
    stats_add<p_collect_stats>(
        stats, &simd_chaining_execution_stats::score_vector_batches);
    stats_add<p_collect_stats>(
        stats, &simd_chaining_execution_stats::score_vector_lanes,
        active_lanes);
    stats_add<p_collect_stats>(
        stats, &simd_chaining_execution_stats::score_masked_lanes,
        hn::Lanes(d) - active_lanes);
    return scores;
}

template<bool p_collect_stats, class p_descriptor_type>
HWY_INLINE hn::Vec<p_descriptor_type> score_candidate_batch(
    p_descriptor_type d,
    std::size_t active_lanes,
    std::int64_t load_start,
    std::int64_t current_idx,
    const soa_anchors& soa,
    std::int32_t max_ref_dist,
    std::int32_t max_query_dist,
    const chaining_parameters& params,
    simd_chaining_execution_stats* stats)
{
    if (active_lanes == hn::Lanes(d)) {
        return score_candidate_batch_specialized<
            p_collect_stats, true, true>(
            d,
            active_lanes,
            load_start,
            current_idx,
            soa,
            max_ref_dist,
            max_query_dist,
            params,
            stats);
    }
    return score_candidate_batch_specialized<
        p_collect_stats, false, true>(
        d,
        active_lanes,
        load_start,
        current_idx,
        soa,
        max_ref_dist,
        max_query_dist,
        params,
        stats);
}

template<bool p_collect_stats, bool p_has_skip_penalty = true, class p_descriptor_type>
[[nodiscard]] std::int32_t score_one_candidate(
    p_descriptor_type d,
    std::int64_t candidate_idx,
    std::int64_t current_idx,
    const soa_anchors& soa,
    std::int32_t max_ref_dist,
    std::int32_t max_query_dist,
    const chaining_parameters& params,
    simd_chaining_execution_stats* stats)
{
    HWY_ALIGN std::int32_t lanes[hn::MaxLanes(d)];
    const auto scores = score_candidate_batch_specialized<
        p_collect_stats, false, p_has_skip_penalty>(
        d, 1, candidate_idx, current_idx, soa,
        max_ref_dist, max_query_dist, params, stats);
    hn::Store(scores, d, lanes);
    return lanes[0];
}

template<bool p_collect_stats, class p_descriptor_type>
[[nodiscard]] std::int32_t exact_global_best(
    p_descriptor_type d,
    const std::int32_t* best_scores,
    std::int32_t begin,
    std::int32_t end,
    simd_chaining_execution_stats* stats)
{
    const std::size_t lanes = hn::Lanes(d);
    auto lane_best_scores =
        hn::Set(d, std::numeric_limits<std::int32_t>::min());
    auto lane_best_indices = hn::Set(d, -1);

    for (std::int32_t block = begin; block < end;
         block += static_cast<std::int32_t>(lanes))
    {
        const std::size_t active_lanes = std::min<std::size_t>(
            lanes, static_cast<std::size_t>(end - block));
        const auto active = hn::FirstN(d, active_lanes);
        const auto scores = hn::MaskedLoad(
            active, d, best_scores + block);
        const auto indices = hn::Add(
            hn::Set(d, block), hn::Iota(d, 0));
        const auto better = hn::And(
            active,
            hn::Or(
                hn::Gt(scores, lane_best_scores),
                hn::And(
                    hn::Eq(scores, lane_best_scores),
                    hn::Gt(indices, lane_best_indices))));
        lane_best_scores = hn::IfThenElse(
            better, scores, lane_best_scores);
        lane_best_indices = hn::IfThenElse(
            better, indices, lane_best_indices);
        stats_add<p_collect_stats>(
            stats,
            &simd_chaining_execution_stats::global_best_vector_batches);
        stats_add<p_collect_stats>(
            stats,
            &simd_chaining_execution_stats::global_best_vector_lanes,
            active_lanes);
    }

    const auto maximum_score = hn::MaxOfLanes(d, lane_best_scores);
    const auto tied_indices = hn::IfThenElse(
        hn::Eq(lane_best_scores, maximum_score),
        lane_best_indices,
        hn::Set(d, -1));
    // Scalar scans descending and replaces only on strict improvement, so the
    // largest index wins every score tie.
    return hn::GetLane(hn::MaxOfLanes(d, tied_indices));
}

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
/// the specialized SIMD batch scorer; scalar work is limited to batch
/// orchestration and extracting the vector-reduced winning predecessor.
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

        auto record_batch = [&](hn::Mask<p_descriptor_type> improved,
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

/// Compile-time forward dispatch keeps the policy test outside the candidate
/// loop while sharing allocation, SoA conversion, and backtracking.
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

struct scored_anchor_index {
    std::int32_t score;
    std::int32_t anchor_index;
};
static_assert(sizeof(scored_anchor_index) == 8u);

struct score_ascending {
    [[nodiscard]] bool operator()(
        const scored_anchor_index& lhs,
        const scored_anchor_index& rhs) const noexcept
    {
        return lhs.score != rhs.score
            ? lhs.score < rhs.score
            : lhs.anchor_index < rhs.anchor_index;
    }
};

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
    const std::size_t matrix_elements =
        anchors.size() * static_cast<std::size_t>(lanes);
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

template<bool p_collect_stats, class p_descriptor_type>
void run_simd_chaining(
    p_descriptor_type d,
    std::span<const anchor> anchors,
    const chaining_parameters& params,
    arena_slab<chain>& chains_out,
    arena_slab<anchor>& reordered_anchors_out,
    simd_chaining_execution_stats* stats,
    simd_chaining_route_stats* route_summary)
{
    const std::size_t count = anchors.size();
    auto soa = create_soa_anchors(d, anchors);
    auto best_scores =
        context::current().alloc_chain_array<std::int32_t>(count);
    auto predecessors =
        context::current().alloc_chain_array<std::int32_t>(count);
    auto peak_scores =
        context::current().alloc_chain_array<std::int32_t>(count);
    std::memset(
        predecessors.data(), 0xFF,
        count * sizeof(std::int32_t));

    switch (params.simd_policy) {
        case simd_chaining_policy::exact_visited:
            simd_dp_forward_pass<
                simd_chaining_policy::exact_visited, p_collect_stats>(
                    d,
                    anchors,
                    soa,
                    best_scores,
                    predecessors,
                    peak_scores,
                    params,
                    stats,
                    route_summary);
            break;
        case simd_chaining_policy::bounded_batch_approximate:
            simd_dp_forward_pass<
                simd_chaining_policy::bounded_batch_approximate, p_collect_stats>(
                    d,
                    anchors,
                    soa,
                    best_scores,
                    predecessors,
                    peak_scores,
                    params,
                    stats,
                    route_summary);
            break;
        default:
            reject(
                stats,
                simd_chaining_status::invalid_policy,
                "unknown SIMD chaining policy reached forward dispatch");
    }
    {
        // Keep `backtrack_chains` attached to the rewritten local SIMD
        // backtracker. A surrounding orchestration scope would preserve the
        // label while timing unrelated work and could conceal a fallback.
        static const auto* backtrack_region_desc =
            bioseq::instrumentation::register_static_region("backtrack_chains");
        auto backtrack_region_scope =
            bioseq::instrumentation::region(backtrack_region_desc);
        backtrack_chains_simd<p_collect_stats>(
            d,
            anchors,
            best_scores,
            predecessors,
            params.min_chain_score,
            params.min_seed_count,
            chains_out,
            reordered_anchors_out,
            stats,
            route_summary);
    }
}

}  // namespace chaining_simd_detail

/**
 * Named SIMD DP chaining. Supplying stats is optional and does not alter the
 * requested policy. Unsupported inputs throw; this entry point never calls a
 * scalar scoring or backtracking implementation.
 */
void chain_anchors_simd(
    std::span<const anchor> anchors,
    const chaining_parameters& params,
    arena_slab<chain>& chains_out,
    arena_slab<anchor>& reordered_anchors_out,
    simd_chaining_execution_stats* stats = nullptr,
    bool aggregate_stats = false)
{
    if (stats != nullptr) {
        *stats = {};
        stats->anchors_consumed = anchors.size();
    }

    hn::ScalableTag<std::int32_t> d;
    const std::size_t lanes = hn::Lanes(d);
    switch (params.simd_policy) {
        case simd_chaining_policy::exact_visited:
        case simd_chaining_policy::bounded_batch_approximate:
            break;
        default:
            chaining_simd_detail::reject(
                stats,
                simd_chaining_status::invalid_policy,
                "unknown SIMD chaining policy");
    }
    if (lanes < 2) {
        chaining_simd_detail::reject(
            stats,
            simd_chaining_status::unsupported_simd_target,
            "the selected Highway target has fewer than two int32 lanes");
    }
    if (params.max_gap < 0
        || params.bandwidth < 0
        || params.max_chain_skip < 0
        || params.max_chain_iter < 0
        || params.min_seed_count < 0
        || !std::isfinite(params.gap_penalty)
        || !std::isfinite(params.skip_penalty)
        || params.gap_penalty < 0.0F
        || params.skip_penalty < 0.0F)
    {
        chaining_simd_detail::reject(
            stats,
            simd_chaining_status::invalid_parameters,
            "distances, iteration limits, seed count, and penalties must "
            "be finite and non-negative");
    }
    if (params.simd_policy
            == simd_chaining_policy::bounded_batch_approximate
        && (params.max_chain_skip <= 0
            || params.max_chain_iter <= 0))
    {
        if (stats != nullptr) {
            ++stats->policy.policy_rejections;
        }
        chaining_simd_detail::reject(
            stats,
            simd_chaining_status::invalid_parameters,
            "bounded SIMD chaining requires positive max_chain_skip and "
            "max_chain_iter");
    }
    if (anchors.size()
        > static_cast<std::size_t>(
            std::numeric_limits<std::int32_t>::max()))
    {
        chaining_simd_detail::reject(
            stats,
            simd_chaining_status::anchor_count_overflow,
            "anchor indices do not fit the exact int32 SIMD ABI");
    }
    if (!anchors.empty()
        && anchors.size()
            > static_cast<std::size_t>(
                  std::numeric_limits<std::int32_t>::max())
                  / lanes)
    {
        chaining_simd_detail::reject(
            stats,
            simd_chaining_status::workspace_overflow,
            "lane-batched backtracking workspace exceeds int32 gather ABI");
    }
    if (stats != nullptr) {
        switch (params.simd_policy) {
            case simd_chaining_policy::exact_visited:
                ++stats->policy.exact_visited_calls;
                break;
            case simd_chaining_policy::bounded_batch_approximate:
                ++stats->policy.bounded_batch_approximate_calls;
                break;
            default:
                // The policy was validated before all allocation/workspace
                // checks, so this branch is intentionally unreachable.
                chaining_simd_detail::reject(
                    stats,
                    simd_chaining_status::invalid_policy,
                    "unknown SIMD chaining policy after validation");
        }
    }
    if (anchors.empty()) {
        return;
    }

    static const auto* region_desc =
        bioseq::instrumentation::register_static_region("mg_lchain_simd");
    auto region_scope = bioseq::instrumentation::region(region_desc);
    auto chain_scope =
        context::current().push<context::arena::chain>();

    if (stats != nullptr && aggregate_stats) {
        chaining_simd_detail::simd_chaining_route_stats route_summary{};
        chaining_simd_detail::run_simd_chaining<false>(
            d,
            anchors,
            params,
            chains_out,
            reordered_anchors_out,
            nullptr,
            &route_summary);
        stats->score_vector_batches =
            route_summary.score_vector_batches;
        stats->score_vector_lanes = route_summary.score_vector_lanes;
        stats->score_masked_lanes = route_summary.score_masked_lanes;
        stats->no_improvement_fast_batches =
            route_summary.no_improvement_fast_batches;
        stats->improvement_replay_batches =
            route_summary.improvement_replay_batches;
        stats->improvement_replay_lanes =
            route_summary.improvement_replay_lanes;
        stats->contiguous_predecessor_store_batches =
            route_summary.contiguous_predecessor_store_batches;
        stats->scalar_orchestration_steps =
            route_summary.scalar_orchestration_steps;
        stats->global_best_vector_batches =
            route_summary.global_best_vector_work ? 1u : 0u;
        stats->global_best_vector_lanes =
            route_summary.global_best_vector_work ? 1u : 0u;
        stats->backtrack_rank_batches = route_summary.backtrack_rank_work ? 1u : 0u;
        stats->backtrack_path_vector_batches =
            route_summary.backtrack_path_vector_work ? 1u : 0u;
        stats->backtrack_commit_vector_batches =
            route_summary.backtrack_commit_vector_work ? 1u : 0u;
        stats->backtrack_vector_lanes =
            route_summary.backtrack_path_vector_work ? 1u : 0u;
        stats->backtrack_primary_contiguous_batches =
            route_summary.backtrack_primary_contiguous_batches;
        stats->backtrack_all_starts_visited_fast_batches =
            route_summary.backtrack_all_starts_visited_fast_batches;
        if (params.simd_policy
            == simd_chaining_policy::bounded_batch_approximate)
        {
            simd_chaining_policy_stats bounded_stats{
                .bounded_score_batches =
                    route_summary.bounded_score_batches,
                .bounded_improving_batches =
                    route_summary.bounded_improving_batches,
                .bounded_nonimproving_batches =
                    route_summary.bounded_nonimproving_batches,
                .bounded_cutoff_stops =
                    route_summary.bounded_cutoff_stops,
                .bounded_candidates_skipped =
                    route_summary.bounded_candidates_skipped,
                .global_best_rescue_probes =
                    route_summary.global_best_rescue_probes,
                .global_best_rescue_wins =
                    route_summary.global_best_rescue_wins,
            };
            stats->policy.merge(bounded_stats);
        }
    } else if (stats != nullptr) {
        chaining_simd_detail::run_simd_chaining<true>(
            d,
            anchors,
            params,
            chains_out,
            reordered_anchors_out,
            stats,
            nullptr);
    } else {
        chaining_simd_detail::run_simd_chaining<false>(
            d,
            anchors,
            params,
            chains_out,
            reordered_anchors_out,
            nullptr,
            nullptr);
    }
    bioseq_log<logging_level::debug>(
        "[chaining_simd] {} SIMD chaining complete for {} anchors",
        bioseq::chaining::simd_chaining_policy_name(
            params.simd_policy),
        anchors.size());
}

}  // namespace target
}  // namespace bioseq::simd

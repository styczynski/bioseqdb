/**
 * @file chaining_simd_candidate_scoring.cppm
 * @brief Packs anchor fields for SIMD loads and scores possible chain links.
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

export module bioseq.highway.sources.chaining_candidate_scoring;

import std;
import bioseq.common.context;
import bioseq.common.logging;
import bioseq.instrumentation;
import bioseq.chaining.simd;
import bioseq.chaining.core;
import bioseq.highway.sources.simd_utils;

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

/// @brief Collects a small execution summary alongside optional detailed counters.
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

/// @brief Add a counter when this kernel collects detailed statistics.
/// @param stats Required when p_collect_stats is true.
/// @param member Counter to update.
/// @param value Amount to add.
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

/// @brief Record a rejection when counters are present, then throw.
/// @param status Reason carried by the exception.
/// @param detail Error details.
/// @throws simd_chaining_runtime_error Always.
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

/// @brief Borrows separate coordinate and span arrays from the chaining arena.
struct soa_anchors {
    std::uint32_t* ref_positions = nullptr;
    std::uint32_t* query_positions = nullptr;
    std::uint32_t* query_spans = nullptr;
    std::size_t count = 0;

    /// @brief Return writable reference coordinates stored in 32-bit words.
    [[nodiscard]] std::uint32_t* ref_data() noexcept {
        return ref_positions;
    }
    /// @brief Return writable query coordinates.
    [[nodiscard]] std::uint32_t* query_data() noexcept {
        return query_positions;
    }
    /// @brief Return writable query seed spans.
    [[nodiscard]] std::uint32_t* span_data() noexcept {
        return query_spans;
    }
    /// @brief Return read-only reference coordinates stored in 32-bit words.
    [[nodiscard]] const std::uint32_t* ref_data() const noexcept {
        return ref_positions;
    }
    /// @brief Return read-only query coordinates.
    [[nodiscard]] const std::uint32_t* query_data() const noexcept {
        return query_positions;
    }
    /// @brief Return read-only query seed spans.
    [[nodiscard]] const std::uint32_t* span_data() const noexcept {
        return query_spans;
    }
};

/// @brief Copy anchor fields into separate arrays for contiguous SIMD loads.
/// Allocate the arrays together in the current chaining arena. The returned
/// pointers remain valid while that arena keeps the allocation.
template<class p_descriptor_type>
[[nodiscard]] soa_anchors create_soa_anchors(
    p_descriptor_type,
    std::span<const anchor> anchors)
{
    const std::size_t count = anchors.size();
    const auto elements = bioseq::chaining::simd_chaining_soa_elements(count);
    if (!elements) {
        throw std::length_error{"SIMD chaining SoA element count overflowed"};
    }
    auto storage =
        context::current().alloc_chain_array<std::uint32_t, 64>(*elements);
    soa_anchors result{
        .ref_positions = storage.data(),
        .query_positions = storage.data() + count,
        .query_spans = storage.data() + 2 * count,
        .count = count,
    };
    for (std::size_t i = 0; i < count; ++i) {
        // Keep the low 32 bits. Subtraction preserves bounded coordinate
        // differences even when the reference crosses the 4-Gbp boundary.
        result.ref_positions[i] =
            static_cast<std::uint32_t>(anchors[i].ref_position);
        result.query_positions[i] =
            static_cast<std::uint32_t>(anchors[i].query_position);
        result.query_spans[i] =
            static_cast<std::uint32_t>(anchors[i].query_span);
    }
    return result;
}

/// @brief Score possible links from predecessor anchors to the current anchor.
/// Reward shared query/reference progress and penalize gaps and diagonal
/// deviation. Invalid candidates and inactive lanes get the minimum int32 score.
/// p_full_batch skips tail masking. p_has_skip_penalty controls the linear
/// penalty for distance along the chain.
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
    const auto linear_penalty = [&df, &gap_length, &gap_penalty, &deviation_f, &skip_penalty] {
        if constexpr (p_has_skip_penalty) {
            const auto gap_length_f = hn::ConvertTo(df, gap_length);
            // Keep the scalar formula's order when combining the two linear penalties.
            return hn::MulAdd(
                gap_penalty,
                deviation_f,
                hn::Mul(hn::Set(df, skip_penalty), gap_length_f));
        } else {
            // A zero skip penalty removes the gap-length conversion and multiply.
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

/// @brief Load one predecessor batch and calculate its link scores.
/// Full batches use ordinary loads. Tail batches mask unused lanes.
/// The result also updates detailed counters when p_collect_stats is true.
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
    const auto predecessor_ref = [&d, &predecessor_ref_ptr, &active_lanes] {
        if constexpr (p_full_batch) {
            return hn::LoadU(d, predecessor_ref_ptr);
        } else {
            return hn::MaskedLoad(
                hn::FirstN(d, active_lanes), d, predecessor_ref_ptr);
        }
    }();
    const auto predecessor_query = [&d, &predecessor_query_ptr, &active_lanes] {
        if constexpr (p_full_batch) {
            return hn::LoadU(d, predecessor_query_ptr);
        } else {
            return hn::MaskedLoad(
                hn::FirstN(d, active_lanes), d, predecessor_query_ptr);
        }
    }();
    const auto predecessor_span = [&d, &predecessor_span_ptr, &active_lanes] {
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

/// @brief Choose full or masked loads for a candidate batch.
/// Return one link score per lane, including the configured skip penalty.
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

/// @brief Score one candidate with the same SIMD arithmetic as a full batch.
/// Use one active lane and return its score, or the invalid-score sentinel.
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

/// @brief Find the highest-scoring anchor in [begin, end).
/// Equal scores choose the largest index, matching a newest-first scalar scan.
/// Return -1 when the interval is empty.
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
    // A newest-first scalar scan keeps the largest index on a score tie.
    return hn::GetLane(hn::MaxOfLanes(d, tied_indices));
}


}  // namespace chaining_simd_detail

}  // namespace target
}  // namespace bioseq::simd

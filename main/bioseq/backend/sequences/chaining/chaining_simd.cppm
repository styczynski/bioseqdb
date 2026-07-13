/**
 * @file chaining_simd.cppm
 * @brief Exact bounded SIMD implementation of minimap2-style DP chaining.
 *
 * Every biological score is evaluated by Highway vectors, including ranges
 * with one to five predecessors and final partial vectors.  Candidate results
 * are consumed in descending anchor order so the visited/max_chain_skip state
 * machine is identical to the scalar reference.  Chain extraction is local to
 * this backend: predecessor paths are gathered in lane batches and committed
 * with vector visited-prefix operations in strict score-rank order.
 */
module;

#include <hwy/aligned_allocator.h>
#include <hwy/highway.h>

#include "common.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

export module bioseq.sequences.chaining.simd;

import bioseq.common.context;
import bioseq.common.mlogging;
import bioseq.instr;
import bioseq.sequences.chaining.utils;
import bioseq.simd.utils;

export {

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

using bioseq::chaining::Anchor;
using bioseq::chaining::ArenaSlab;
using bioseq::chaining::Chain;
using bioseq::chaining::ChainingParams;

enum class SimdChainingStatus : std::uint8_t {
    ok = 0,
    unsupported_simd_target,
    invalid_parameters,
    anchor_count_overflow,
    workspace_overflow,
    cyclic_predecessors,
};

[[nodiscard]] constexpr std::string_view simd_chaining_status_name(
    SimdChainingStatus status) noexcept
{
    switch (status) {
        case SimdChainingStatus::ok: return "ok";
        case SimdChainingStatus::unsupported_simd_target:
            return "unsupported_simd_target";
        case SimdChainingStatus::invalid_parameters:
            return "invalid_parameters";
        case SimdChainingStatus::anchor_count_overflow:
            return "anchor_count_overflow";
        case SimdChainingStatus::workspace_overflow:
            return "workspace_overflow";
        case SimdChainingStatus::cyclic_predecessors:
            return "cyclic_predecessors";
    }
    return "unknown";
}

class SimdChainingRuntimeError final : public std::runtime_error {
public:
    SimdChainingRuntimeError(
        SimdChainingStatus status,
        std::string_view detail)
        : std::runtime_error(
              std::string{"Exact SIMD chaining rejected: "}
              + std::string{simd_chaining_status_name(status)}
              + ": " + std::string{detail}),
          status_(status) {}

    [[nodiscard]] SimdChainingStatus status() const noexcept {
        return status_;
    }

private:
    SimdChainingStatus status_;
};

/**
 * Algorithm-local evidence only.  This intentionally is not mapped into the
 * canonical pipeline proof yet.
 */
struct SimdChainingExecutionProof {
    std::uint64_t anchors_consumed = 0;
    std::uint64_t score_vector_batches = 0;
    std::uint64_t score_vector_lanes = 0;
    std::uint64_t score_masked_lanes = 0;
    std::uint64_t descending_candidate_steps = 0;
    std::uint64_t visited_marks = 0;
    std::uint64_t max_chain_skip_stops = 0;
    std::uint64_t global_best_vector_batches = 0;
    std::uint64_t global_best_vector_lanes = 0;
    std::uint64_t backtrack_rank_batches = 0;
    std::uint64_t backtrack_path_vector_batches = 0;
    std::uint64_t backtrack_commit_vector_batches = 0;
    std::uint64_t backtrack_vector_lanes = 0;
    std::uint64_t backtrack_overlap_stops = 0;
    std::uint64_t scalar_orchestration_steps = 0;
    std::uint64_t fail_closed_rejections = 0;

    // Forbidden paths.  They are never incremented by this implementation;
    // tests assert zero so later routing cannot silently launder scalar work.
    std::uint64_t scalar_score_branches = 0;
    std::uint64_t scalar_backtrack_branches = 0;
    std::uint64_t shared_scalar_backtrack_calls = 0;
    bool saturated = false;

    static void add_counter(
            std::uint64_t& destination,
            std::uint64_t increment,
            bool& did_saturate) noexcept {
        if (increment > (std::numeric_limits<std::uint64_t>::max)()
                - destination) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            did_saturate = true;
            return;
        }
        destination += increment;
    }

    void combine(const SimdChainingExecutionProof& other) noexcept {
        saturated = saturated || other.saturated;
#define BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(name) \
        add_counter(name, other.name, saturated)
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(anchors_consumed);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(score_vector_batches);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(score_vector_lanes);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(score_masked_lanes);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(descending_candidate_steps);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(visited_marks);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(max_chain_skip_stops);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(global_best_vector_batches);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(global_best_vector_lanes);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(backtrack_rank_batches);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(backtrack_path_vector_batches);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(backtrack_commit_vector_batches);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(backtrack_vector_lanes);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(backtrack_overlap_stops);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(scalar_orchestration_steps);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(fail_closed_rejections);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(scalar_score_branches);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(scalar_backtrack_branches);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(shared_scalar_backtrack_calls);
#undef BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER
    }

    [[nodiscard]] constexpr bool accepted(
            bool require_nonempty_vector_work = false) const noexcept {
        const bool exact_backend = !saturated
            && fail_closed_rejections == 0
            && scalar_score_branches == 0
            && scalar_backtrack_branches == 0
            && shared_scalar_backtrack_calls == 0;
        if (!exact_backend || !require_nonempty_vector_work) {
            return exact_backend;
        }
        return anchors_consumed > 1
            && score_vector_batches > 0
            && score_vector_lanes > 0
            && backtrack_rank_batches > 0
            && backtrack_path_vector_batches > 0
            && backtrack_commit_vector_batches > 0
            && backtrack_vector_lanes > 0;
    }

    [[nodiscard]] constexpr bool operator==(
        const SimdChainingExecutionProof&) const noexcept = default;
};

namespace chaining_simd_detail {

// Production attestation needs route evidence without turning the O(n*iter)
// score loop into a stream of counter stores.  These booleans live in the
// caller's frame, are set only at the actual SIMD call sites, and are folded
// into SimdChainingExecutionProof once after the kernels return.  The detailed
// proof mode used by algorithm tests remains available unchanged.
struct SimdChainingRouteSummary {
    bool score_vector_work = false;
    bool global_best_vector_work = false;
    bool backtrack_rank_work = false;
    bool backtrack_path_vector_work = false;
    bool backtrack_commit_vector_work = false;
};

template<bool TrackProof>
HWY_INLINE void proof_add(
    SimdChainingExecutionProof* proof,
    std::uint64_t SimdChainingExecutionProof::* member,
    std::uint64_t value = 1) noexcept
{
    if constexpr (TrackProof) {
        proof->*member += value;
    } else {
        (void)proof;
        (void)member;
        (void)value;
    }
}

[[noreturn]] inline void reject(
    SimdChainingExecutionProof* proof,
    SimdChainingStatus status,
    std::string_view detail)
{
    if (proof != nullptr) {
        ++proof->fail_closed_rejections;
    }
    throw SimdChainingRuntimeError(status, detail);
}

struct SoaAnchors {
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

template<class D>
[[nodiscard]] SoaAnchors create_soa_anchors(
    D,
    std::span<const Anchor> anchors)
{
    const std::size_t count = anchors.size();
    auto storage =
        context::current().alloc_chain_array<std::uint32_t, 64>(3 * count);
    SoaAnchors result{
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

template<class D, class D_F>
HWY_INLINE hn::Vec<D> vectorized_chaining_score(
    D d,
    D_F df,
    hn::Mask<D> active,
    hn::Vec<D> current_ref,
    hn::Vec<D> current_query,
    hn::Vec<D> predecessor_ref,
    hn::Vec<D> predecessor_query,
    hn::Vec<D> predecessor_span,
    hn::Vec<D> max_ref_dist,
    hn::Vec<D> max_query_dist,
    hn::Vec<D> bandwidth,
    hn::Vec<D_F> gap_penalty,
    hn::Vec<D_F> skip_penalty)
{
    const auto zero = hn::Zero(d);
    const auto invalid_score =
        hn::Set(d, std::numeric_limits<std::int32_t>::min());
    const auto query_distance = hn::Sub(current_query, predecessor_query);
    const auto ref_distance = hn::Sub(current_ref, predecessor_ref);
    const auto diagonal_deviation =
        hn::Abs(hn::Sub(ref_distance, query_distance));
    const auto gap_length = hn::Min(ref_distance, query_distance);

    const auto invalid = hn::Or(
        hn::Not(active),
        hn::Or(
            hn::Or(
                hn::Or(
                    hn::Le(query_distance, zero),
                    hn::Gt(query_distance, max_ref_dist)),
                hn::Eq(ref_distance, zero)),
            hn::Or(
                hn::Gt(query_distance, max_query_dist),
                hn::Gt(diagonal_deviation, bandwidth))));

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
    const auto gap_length_f = hn::ConvertTo(df, gap_length);
    // Same source ordering as scalar:
    // gap_penalty * diagonal_deviation + skip_penalty * gap_length.
    const auto linear_penalty = hn::MulAdd(
        gap_penalty,
        deviation_f,
        hn::Mul(skip_penalty, gap_length_f));
    const auto raw_log2 = vectorized_fast_log2(
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

template<bool TrackProof, class D>
HWY_INLINE hn::Vec<D> score_candidate_batch(
    D d,
    std::size_t active_lanes,
    std::int64_t load_start,
    std::int64_t current_idx,
    const SoaAnchors& soa,
    std::int32_t max_ref_dist,
    std::int32_t max_query_dist,
    const ChainingParams& params,
    SimdChainingExecutionProof* proof)
{
    using DF = hn::RebindToFloat<D>;
    const DF df;
    const auto active = hn::FirstN(d, active_lanes);
    const auto predecessor_ref = hn::MaskedLoad(
        active, d,
        reinterpret_cast<const std::int32_t*>(
            soa.ref_data() + load_start));
    const auto predecessor_query = hn::MaskedLoad(
        active, d,
        reinterpret_cast<const std::int32_t*>(
            soa.query_data() + load_start));
    const auto predecessor_span = hn::MaskedLoad(
        active, d,
        reinterpret_cast<const std::int32_t*>(
            soa.span_data() + load_start));
    const auto scores = vectorized_chaining_score(
        d,
        df,
        active,
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
        hn::Set(df, params.skip_penalty));
    proof_add<TrackProof>(
        proof, &SimdChainingExecutionProof::score_vector_batches);
    proof_add<TrackProof>(
        proof, &SimdChainingExecutionProof::score_vector_lanes,
        active_lanes);
    proof_add<TrackProof>(
        proof, &SimdChainingExecutionProof::score_masked_lanes,
        hn::Lanes(d) - active_lanes);
    return scores;
}

template<bool TrackProof, class D>
[[nodiscard]] std::int32_t score_one_candidate(
    D d,
    std::int64_t candidate_idx,
    std::int64_t current_idx,
    const SoaAnchors& soa,
    std::int32_t max_ref_dist,
    std::int32_t max_query_dist,
    const ChainingParams& params,
    SimdChainingExecutionProof* proof)
{
    HWY_ALIGN std::int32_t lanes[hn::MaxLanes(d)];
    const auto scores = score_candidate_batch<TrackProof>(
        d, 1, candidate_idx, current_idx, soa,
        max_ref_dist, max_query_dist, params, proof);
    hn::Store(scores, d, lanes);
    return lanes[0];
}

template<bool TrackProof, class D>
[[nodiscard]] std::int32_t exact_global_best(
    D d,
    const std::int32_t* best_scores,
    std::int32_t begin,
    std::int32_t end,
    SimdChainingExecutionProof* proof)
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
        proof_add<TrackProof>(
            proof,
            &SimdChainingExecutionProof::global_best_vector_batches);
        proof_add<TrackProof>(
            proof,
            &SimdChainingExecutionProof::global_best_vector_lanes,
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

template<bool TrackProof, class D>
void simd_dp_forward_pass(
    D d,
    std::span<const Anchor> anchors,
    const SoaAnchors& soa,
    std::span<std::int32_t> best_scores,
    std::span<std::int32_t> predecessors,
    std::span<std::int32_t> peak_scores,
    const ChainingParams& params,
    SimdChainingExecutionProof* proof,
    SimdChainingRouteSummary* route_summary)
{
    const std::int32_t anchor_count =
        static_cast<std::int32_t>(anchors.size());
    const std::int32_t lanes =
        static_cast<std::int32_t>(hn::Lanes(d));
    const std::int32_t max_ref_dist =
        std::max(params.max_gap, params.bandwidth);
    const std::int32_t max_query_dist =
        std::max(params.max_gap, params.bandwidth);

    auto visited = context::current().alloc_chain_array<std::int32_t>(
        anchors.size());
    std::memset(
        visited.data(), 0,
        anchors.size() * sizeof(std::int32_t));

    HWY_ALIGN std::int32_t score_lanes[hn::MaxLanes(d)];
    std::int32_t window_start = 0;
    std::int32_t global_best_idx = -1;
    bool route_score_vector_work = false;
    bool route_global_best_vector_work = false;

    for (std::int32_t current_idx = 0;
         current_idx < anchor_count; ++current_idx)
    {
        std::int32_t best_predecessor = -1;
        std::int32_t best_score =
            static_cast<std::int32_t>(anchors[current_idx].query_span);
        std::int32_t skip_count = 0;

        while (window_start < current_idx
               && (anchors[current_idx].ref_chromosome_key()
                       != anchors[window_start].ref_chromosome_key()
                   || anchors[current_idx].ref_bits()
                       > anchors[window_start].ref_bits()
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
            const auto scores = score_candidate_batch<TrackProof>(
                d,
                static_cast<std::size_t>(active_lanes),
                load_start,
                current_idx,
                soa,
                max_ref_dist,
                max_query_dist,
                params,
                proof);
            hn::Store(scores, d, score_lanes);

            for (std::int32_t lane = active_lanes - 1;
                 lane >= 0; --lane)
            {
                const std::int32_t candidate_idx = load_start + lane;
                const std::int32_t pair_score = score_lanes[lane];
                proof_add<TrackProof>(
                    proof,
                    &SimdChainingExecutionProof::descending_candidate_steps);
                proof_add<TrackProof>(
                    proof,
                    &SimdChainingExecutionProof::scalar_orchestration_steps);
                if (pair_score
                    == std::numeric_limits<std::int32_t>::min())
                {
                    continue;
                }
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
                } else if (
                    visited[candidate_idx] == current_idx)
                {
                    if (++skip_count > params.max_chain_skip) {
                        search_end = candidate_idx;
                        proof_add<TrackProof>(
                            proof,
                            &SimdChainingExecutionProof::
                                max_chain_skip_stops);
                        stopped = true;
                        break;
                    }
                }
                if (predecessors[candidate_idx] >= 0) {
                    visited[predecessors[candidate_idx]] = current_idx;
                    proof_add<TrackProof>(
                        proof,
                        &SimdChainingExecutionProof::visited_marks);
                }
            }
            candidate_end = load_start;
        }

        if (global_best_idx < 0
            || anchors[current_idx].ref_bits()
                    - anchors[global_best_idx].ref_bits()
                > static_cast<std::uint64_t>(max_ref_dist))
        {
            route_global_best_vector_work =
                route_global_best_vector_work || window_start < current_idx;
            global_best_idx = exact_global_best<TrackProof>(
                d, best_scores.data(), window_start, current_idx, proof);
            global_pair_score_cached = false;
        }

        if (global_best_idx >= 0 && global_best_idx < search_end) {
            route_score_vector_work =
                route_score_vector_work || !global_pair_score_cached;
            const std::int32_t global_pair_score =
                global_pair_score_cached
                ? cached_global_pair_score
                : score_one_candidate<TrackProof>(
                      d,
                      global_best_idx,
                      current_idx,
                      soa,
                      max_ref_dist,
                      max_query_dist,
                      params,
                      proof);
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
            || (anchors[current_idx].ref_bits()
                        - anchors[global_best_idx].ref_bits()
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
    }
}

struct ScoredAnchorIndex {
    std::int32_t score;
    std::int64_t anchor_index;
};

struct ScoreAscending {
    [[nodiscard]] bool operator()(
        const ScoredAnchorIndex& lhs,
        const ScoredAnchorIndex& rhs) const noexcept
    {
        return lhs.score < rhs.score;
    }
};

template<bool TrackProof, class D>
void backtrack_chains_simd(
    D d,
    std::span<const Anchor> anchors,
    std::span<const std::int32_t> best_scores,
    std::span<const std::int32_t> predecessors,
    std::int32_t min_score,
    std::int32_t min_seed_count,
    ArenaSlab<Chain>& chains_out,
    ArenaSlab<Anchor>& reordered_anchors_out,
    SimdChainingExecutionProof* proof,
    SimdChainingRouteSummary* route_summary)
{
    const std::int32_t anchor_count =
        static_cast<std::int32_t>(anchors.size());
    const std::int32_t lanes =
        static_cast<std::int32_t>(hn::Lanes(d));
    auto& ctx = context::current();

    auto scored = ctx.alloc_chain_array<ScoredAnchorIndex>(
        anchors.size());
    std::size_t scored_count = 0;
    for (std::int32_t i = 0; i < anchor_count; ++i) {
        if (best_scores[i] >= min_score) {
            scored[scored_count++] =
                ScoredAnchorIndex{best_scores[i], i};
        }
    }
    std::sort(
        scored.data(), scored.data() + scored_count,
        ScoreAscending{});

    auto visited = ctx.alloc_chain_array<std::int32_t>(
        anchors.size());
    std::memset(
        visited.data(), 0,
        anchors.size() * sizeof(std::int32_t));

    const std::size_t matrix_elements =
        anchors.size() * static_cast<std::size_t>(lanes);
    auto paths = ctx.alloc_chain_array<std::int32_t, 64>(
        matrix_elements);

    HWY_ALIGN std::int32_t start_lanes[hn::MaxLanes(d)];
    HWY_ALIGN std::int32_t length_lanes[hn::MaxLanes(d)];
    HWY_ALIGN std::int32_t terminal_lanes[hn::MaxLanes(d)];
    const auto lane_bases = hn::Mul(
        hn::Iota(d, 0), hn::Set(d, anchor_count));
    const auto one = hn::Set(d, 1);
    const auto minus_one = hn::Set(d, -1);
    bool route_rank_work = false;
    bool route_path_work = false;
    bool route_commit_work = false;

    std::int64_t highest_rank =
        static_cast<std::int64_t>(scored_count) - 1;
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

            proof_add<TrackProof>(
                proof,
                &SimdChainingExecutionProof::
                    backtrack_path_vector_batches);
            proof_add<TrackProof>(
                proof,
                &SimdChainingExecutionProof::backtrack_vector_lanes,
                hn::CountTrue(d, append));
        }
        if (!hn::AllFalse(d, active)) {
            reject(
                proof,
                SimdChainingStatus::cyclic_predecessors,
                "predecessor path exceeds anchor count");
        }

        hn::Store(lengths, d, length_lanes);
        hn::Store(terminals, d, terminal_lanes);
        proof_add<TrackProof>(
            proof,
            &SimdChainingExecutionProof::backtrack_rank_batches);
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
                proof_add<TrackProof>(
                    proof,
                    &SimdChainingExecutionProof::
                        backtrack_commit_vector_batches);
                proof_add<TrackProof>(
                    proof,
                    &SimdChainingExecutionProof::scalar_orchestration_steps);
                if (first_hit >= 0) {
                    terminal = lane_path[offset + first_hit];
                    proof_add<TrackProof>(
                        proof,
                        &SimdChainingExecutionProof::
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
                chains_out.push_back(Chain{
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
    }
}

template<bool TrackProof, class D>
void run_exact_simd_chaining(
    D d,
    std::span<const Anchor> anchors,
    const ChainingParams& params,
    ArenaSlab<Chain>& chains_out,
    ArenaSlab<Anchor>& reordered_anchors_out,
    SimdChainingExecutionProof* proof,
    SimdChainingRouteSummary* route_summary)
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

    simd_dp_forward_pass<TrackProof>(
        d,
        anchors,
        soa,
        best_scores,
        predecessors,
        peak_scores,
        params,
        proof,
        route_summary);
    {
        // Keep `backtrack_chains` attached to the rewritten local SIMD
        // backtracker. A surrounding orchestration scope would preserve the
        // label while timing unrelated work and could conceal a fallback.
        static const auto* backtrack_region_desc =
            bioseq::instr::register_region_static("backtrack_chains");
        auto backtrack_region_scope =
            bioseq::instr::region(backtrack_region_desc);
        backtrack_chains_simd<TrackProof>(
            d,
            anchors,
            best_scores,
            predecessors,
            params.min_chain_score,
            params.min_seed_count,
            chains_out,
            reordered_anchors_out,
            proof,
            route_summary);
    }
}

}  // namespace chaining_simd_detail

/**
 * Exact SIMD DP chaining.  Supplying proof is optional and does not alter
 * output.  Unsupported inputs throw; this entry point never calls a scalar
 * scoring or backtracking implementation.
 */
void chain_anchors_simd(
    std::span<const Anchor> anchors,
    const ChainingParams& params,
    ArenaSlab<Chain>& chains_out,
    ArenaSlab<Anchor>& reordered_anchors_out,
    SimdChainingExecutionProof* proof = nullptr,
    bool aggregate_proof = false)
{
    if (proof != nullptr) {
        *proof = {};
        proof->anchors_consumed = anchors.size();
    }

    hn::ScalableTag<std::int32_t> d;
    const std::size_t lanes = hn::Lanes(d);
    if (lanes < 2) {
        chaining_simd_detail::reject(
            proof,
            SimdChainingStatus::unsupported_simd_target,
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
            proof,
            SimdChainingStatus::invalid_parameters,
            "distances, iteration limits, seed count, and penalties must "
            "be finite and non-negative");
    }
    if (anchors.size()
        > static_cast<std::size_t>(
            std::numeric_limits<std::int32_t>::max()))
    {
        chaining_simd_detail::reject(
            proof,
            SimdChainingStatus::anchor_count_overflow,
            "anchor indices do not fit the exact int32 SIMD ABI");
    }
    if (!anchors.empty()
        && anchors.size()
            > static_cast<std::size_t>(
                  std::numeric_limits<std::int32_t>::max())
                  / lanes)
    {
        chaining_simd_detail::reject(
            proof,
            SimdChainingStatus::workspace_overflow,
            "lane-batched backtracking workspace exceeds int32 gather ABI");
    }
    if (anchors.empty()) {
        return;
    }

    static const auto* region_desc =
        bioseq::instr::register_region_static("mg_lchain_simd");
    auto region_scope = bioseq::instr::region(region_desc);
    auto chain_scope =
        context::current().push<context::Arena::Chain>();

    if (proof != nullptr && aggregate_proof) {
        chaining_simd_detail::SimdChainingRouteSummary route_summary{};
        chaining_simd_detail::run_exact_simd_chaining<false>(
            d,
            anchors,
            params,
            chains_out,
            reordered_anchors_out,
            nullptr,
            &route_summary);
        proof->score_vector_batches = route_summary.score_vector_work ? 1u : 0u;
        proof->score_vector_lanes = route_summary.score_vector_work ? 1u : 0u;
        proof->global_best_vector_batches =
            route_summary.global_best_vector_work ? 1u : 0u;
        proof->global_best_vector_lanes =
            route_summary.global_best_vector_work ? 1u : 0u;
        proof->backtrack_rank_batches = route_summary.backtrack_rank_work ? 1u : 0u;
        proof->backtrack_path_vector_batches =
            route_summary.backtrack_path_vector_work ? 1u : 0u;
        proof->backtrack_commit_vector_batches =
            route_summary.backtrack_commit_vector_work ? 1u : 0u;
        proof->backtrack_vector_lanes =
            route_summary.backtrack_path_vector_work ? 1u : 0u;
    } else if (proof != nullptr) {
        chaining_simd_detail::run_exact_simd_chaining<true>(
            d,
            anchors,
            params,
            chains_out,
            reordered_anchors_out,
            proof,
            nullptr);
    } else {
        chaining_simd_detail::run_exact_simd_chaining<false>(
            d,
            anchors,
            params,
            chains_out,
            reordered_anchors_out,
            nullptr,
            nullptr);
    }
    bioseq_log<DBG>(
        "[chaining_simd] exact SIMD chaining complete for {} anchors",
        anchors.size());
}

}  // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

}  // export

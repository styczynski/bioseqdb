/**
 * @file chaining_simd.cppm
 * @brief Exact SIMD implementation of minimap2-style DP chaining.
 *
 * Every biological score is evaluated by Highway vectors, including ranges
 * with one to five predecessors and final partial vectors. Batches that cannot
 * improve the current score update exact max-skip state from SIMD masks;
 * improvement-capable batches replay only scalar control in descending order.
 * Chain extraction is local to this backend: predecessor paths are gathered
 * in lane batches and committed with vector visited-prefix operations in
 * strict score-rank order.
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
using bioseq::chaining::SimdChainingPolicy;

enum class SimdChainingStatus : std::uint8_t {
    ok = 0,
    unsupported_simd_target,
    invalid_parameters,
    invalid_policy,
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
        case SimdChainingStatus::invalid_policy:
            return "invalid_policy";
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
              std::string{"SIMD chaining rejected: "}
              + std::string{simd_chaining_status_name(status)}
              + ": " + std::string{detail}),
          status_(status) {}

    [[nodiscard]] SimdChainingStatus status() const noexcept {
        return status_;
    }

private:
    SimdChainingStatus status_;
};

/// Operation-originated evidence for the named SIMD forward-search policy.
///
/// Bounded scan batches and the one-lane global-best rescue are deliberately
/// separate populations: the former must classify exactly once as improving
/// or non-improving, while the latter is a targeted SIMD probe.
struct SimdChainingPolicyEvidence {
    std::uint64_t exact_visited_calls = 0;
    std::uint64_t bounded_batch_approximate_calls = 0;
    std::uint64_t bounded_score_batches = 0;
    std::uint64_t bounded_improving_batches = 0;
    std::uint64_t bounded_nonimproving_batches = 0;
    std::uint64_t bounded_cutoff_stops = 0;
    std::uint64_t bounded_candidates_skipped = 0;
    std::uint64_t global_best_rescue_probes = 0;
    std::uint64_t global_best_rescue_wins = 0;
    std::uint64_t policy_rejections = 0;
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

    /// Saturating aggregation used by call, batch, and result collectors.
    void combine(const SimdChainingPolicyEvidence& other) noexcept {
        saturated = saturated || other.saturated;
        add_counter(
            exact_visited_calls,
            other.exact_visited_calls,
            saturated);
        add_counter(
            bounded_batch_approximate_calls,
            other.bounded_batch_approximate_calls,
            saturated);
        add_counter(
            bounded_score_batches,
            other.bounded_score_batches,
            saturated);
        add_counter(
            bounded_improving_batches,
            other.bounded_improving_batches,
            saturated);
        add_counter(
            bounded_nonimproving_batches,
            other.bounded_nonimproving_batches,
            saturated);
        add_counter(
            bounded_cutoff_stops,
            other.bounded_cutoff_stops,
            saturated);
        add_counter(
            bounded_candidates_skipped,
            other.bounded_candidates_skipped,
            saturated);
        add_counter(
            global_best_rescue_probes,
            other.global_best_rescue_probes,
            saturated);
        add_counter(
            global_best_rescue_wins,
            other.global_best_rescue_wins,
            saturated);
        add_counter(
            policy_rejections,
            other.policy_rejections,
            saturated);
    }

    /// Fail-closed consistency for the requested named policy.
    [[nodiscard]] constexpr bool coherent(
        SimdChainingPolicy selected,
        bool require_accelerated_work) const noexcept
    {
        if (saturated || policy_rejections != 0) {
            return false;
        }
        switch (selected) {
            case SimdChainingPolicy::exact_visited:
                return exact_visited_calls > 0
                    && bounded_batch_approximate_calls == 0
                    && bounded_score_batches == 0
                    && bounded_improving_batches == 0
                    && bounded_nonimproving_batches == 0
                    && bounded_cutoff_stops == 0
                    && bounded_candidates_skipped == 0
                    && global_best_rescue_probes == 0
                    && global_best_rescue_wins == 0;
            case SimdChainingPolicy::bounded_batch_approximate: {
                if (bounded_batch_approximate_calls == 0
                    || exact_visited_calls != 0
                    || bounded_improving_batches
                        > (std::numeric_limits<std::uint64_t>::max)()
                            - bounded_nonimproving_batches
                    || bounded_improving_batches
                            + bounded_nonimproving_batches
                        != bounded_score_batches
                    || bounded_cutoff_stops
                        > bounded_nonimproving_batches
                    || global_best_rescue_wins
                        > global_best_rescue_probes)
                {
                    return false;
                }
                return !require_accelerated_work
                    || (bounded_score_batches > 0
                        && bounded_cutoff_stops > 0
                        && bounded_candidates_skipped > 0);
            }
        }
        return false;
    }

    [[nodiscard]] constexpr bool operator==(
        const SimdChainingPolicyEvidence&) const noexcept = default;
};

/**
 * Algorithm-local evidence retained separately from the canonical route
 * proof. Match/pipeline/benchmark aggregation carries this object end to end.
 */
struct SimdChainingExecutionProof {
    std::uint64_t anchors_consumed = 0;
    std::uint64_t score_vector_batches = 0;
    std::uint64_t score_vector_lanes = 0;
    std::uint64_t score_masked_lanes = 0;
    std::uint64_t descending_candidate_steps = 0;
    std::uint64_t visited_marks = 0;
    std::uint64_t max_chain_skip_stops = 0;
    std::uint64_t no_improvement_fast_batches = 0;
    std::uint64_t improvement_replay_batches = 0;
    std::uint64_t improvement_replay_lanes = 0;
    std::uint64_t contiguous_predecessor_store_batches = 0;
    std::uint64_t global_best_vector_batches = 0;
    std::uint64_t global_best_vector_lanes = 0;
    std::uint64_t backtrack_rank_batches = 0;
    std::uint64_t backtrack_path_vector_batches = 0;
    std::uint64_t backtrack_commit_vector_batches = 0;
    std::uint64_t backtrack_vector_lanes = 0;
    std::uint64_t backtrack_overlap_stops = 0;
    std::uint64_t backtrack_primary_contiguous_batches = 0;
    std::uint64_t backtrack_all_starts_visited_fast_batches = 0;
    // Ordered score/skip/backtrack control over SIMD-produced values. This is
    // not a scalar biological scoring or backtracking fallback.
    std::uint64_t scalar_orchestration_steps = 0;
    std::uint64_t fail_closed_rejections = 0;

    // Forbidden paths.  They are never incremented by this implementation;
    // tests assert zero so later routing cannot silently launder scalar work.
    std::uint64_t scalar_score_branches = 0;
    std::uint64_t scalar_backtrack_branches = 0;
    std::uint64_t shared_scalar_backtrack_calls = 0;
    SimdChainingPolicyEvidence policy{};
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
        policy.combine(other.policy);
#define BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(name) \
        add_counter(name, other.name, saturated)
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(anchors_consumed);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(score_vector_batches);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(score_vector_lanes);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(score_masked_lanes);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(descending_candidate_steps);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(visited_marks);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(max_chain_skip_stops);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(no_improvement_fast_batches);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(improvement_replay_batches);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(improvement_replay_lanes);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(
            contiguous_predecessor_store_batches);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(global_best_vector_batches);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(global_best_vector_lanes);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(backtrack_rank_batches);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(backtrack_path_vector_batches);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(backtrack_commit_vector_batches);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(backtrack_vector_lanes);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(backtrack_overlap_stops);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(
            backtrack_primary_contiguous_batches);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(
            backtrack_all_starts_visited_fast_batches);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(scalar_orchestration_steps);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(fail_closed_rejections);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(scalar_score_branches);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(scalar_backtrack_branches);
        BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER(shared_scalar_backtrack_calls);
#undef BIOSEQ_COMBINE_SIMD_CHAIN_COUNTER
    }

    [[nodiscard]] constexpr bool accepted(
            SimdChainingPolicy selected,
            bool require_nonempty_vector_work = false) const noexcept {
        const bool authentic_backend = !saturated
            && fail_closed_rejections == 0
            && scalar_score_branches == 0
            && scalar_backtrack_branches == 0
            && shared_scalar_backtrack_calls == 0
            && policy_evidence_coherent(
                selected, /*require_accelerated_work=*/false);
        if (!authentic_backend || !require_nonempty_vector_work) {
            return authentic_backend;
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

    /// Selected-policy evidence plus the policy-specific forward invariants.
    [[nodiscard]] constexpr bool policy_evidence_coherent(
        SimdChainingPolicy selected,
        bool require_accelerated_work = false) const noexcept
    {
        if (!policy.coherent(selected, require_accelerated_work)) {
            return false;
        }
        switch (selected) {
            case SimdChainingPolicy::exact_visited:
                return exact_hybrid_evidence_coherent();
            case SimdChainingPolicy::bounded_batch_approximate:
                return true;
        }
        return false;
    }

    [[nodiscard]] constexpr bool exact_hybrid_evidence_coherent()
            const noexcept {
        if (no_improvement_fast_batches
            > (std::numeric_limits<std::uint64_t>::max)()
                - improvement_replay_batches) {
            return false;
        }
        const std::uint64_t classified_batches =
            no_improvement_fast_batches + improvement_replay_batches;
        if (score_vector_batches == 0) {
            return classified_batches == 0
                && improvement_replay_lanes == 0;
        }
        if (classified_batches == 0
            || classified_batches > score_vector_batches)
        {
            return false;
        }
        if (improvement_replay_batches == 0) {
            return improvement_replay_lanes == 0;
        }
        return improvement_replay_lanes
                    >= improvement_replay_batches
            && improvement_replay_lanes <= score_vector_lanes
            && scalar_orchestration_steps
                    >= improvement_replay_batches;
    }
};

namespace chaining_simd_detail {

// Production attestation needs route evidence without turning the O(n*iter)
// score loop into a stream of writes through a proof pointer. These local
// counters and flags live in the caller's frame, are updated only at the actual
// SIMD/control call sites, and are folded into SimdChainingExecutionProof once
// after the kernels return. Detailed proof mode remains available to tests.
struct SimdChainingRouteSummary {
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
        if (status == SimdChainingStatus::invalid_policy) {
            ++proof->policy.policy_rejections;
        }
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

template<bool FullBatch, bool HasSkipPenalty, class D, class D_F>
HWY_INLINE hn::Vec<D> vectorized_chaining_score(
    D d,
    D_F df,
    std::size_t active_lanes,
    hn::Vec<D> current_ref,
    hn::Vec<D> current_query,
    hn::Vec<D> predecessor_ref,
    hn::Vec<D> predecessor_query,
    hn::Vec<D> predecessor_span,
    hn::Vec<D> max_ref_dist,
    hn::Vec<D> max_query_dist,
    hn::Vec<D> bandwidth,
    hn::Vec<D_F> gap_penalty,
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
    if constexpr (!FullBatch) {
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
        if constexpr (HasSkipPenalty) {
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

template<bool TrackProof, bool FullBatch, bool HasSkipPenalty, class D>
HWY_INLINE hn::Vec<D> score_candidate_batch_specialized(
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
        if constexpr (FullBatch) {
            return hn::LoadU(d, predecessor_ref_ptr);
        } else {
            return hn::MaskedLoad(
                hn::FirstN(d, active_lanes), d, predecessor_ref_ptr);
        }
    }();
    const auto predecessor_query = [&] {
        if constexpr (FullBatch) {
            return hn::LoadU(d, predecessor_query_ptr);
        } else {
            return hn::MaskedLoad(
                hn::FirstN(d, active_lanes), d, predecessor_query_ptr);
        }
    }();
    const auto predecessor_span = [&] {
        if constexpr (FullBatch) {
            return hn::LoadU(d, predecessor_span_ptr);
        } else {
            return hn::MaskedLoad(
                hn::FirstN(d, active_lanes), d, predecessor_span_ptr);
        }
    }();
    const auto scores =
        vectorized_chaining_score<FullBatch, HasSkipPenalty>(
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
    if (active_lanes == hn::Lanes(d)) {
        return score_candidate_batch_specialized<
            TrackProof, true, true>(
            d,
            active_lanes,
            load_start,
            current_idx,
            soa,
            max_ref_dist,
            max_query_dist,
            params,
            proof);
    }
    return score_candidate_batch_specialized<
        TrackProof, false, true>(
        d,
        active_lanes,
        load_start,
        current_idx,
        soa,
        max_ref_dist,
        max_query_dist,
        params,
        proof);
}

template<bool TrackProof, bool HasSkipPenalty = true, class D>
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
    const auto scores = score_candidate_batch_specialized<
        TrackProof, false, HasSkipPenalty>(
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
void simd_dp_forward_pass_exact(
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
            ++route_score_vector_batches;
            route_score_vector_lanes +=
                static_cast<std::uint64_t>(active_lanes);
            route_score_masked_lanes +=
                static_cast<std::uint64_t>(lanes - active_lanes);
            const auto active = hn::FirstN(
                d, static_cast<std::size_t>(active_lanes));
            const bool full_batch = active_lanes == lanes;
            const auto predecessor_scores = full_batch
                ? hn::LoadU(d, best_scores.data() + load_start)
                : hn::MaskedLoad(
                      active, d, best_scores.data() + load_start);
            const auto predecessor_indices = full_batch
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
                proof_add<TrackProof>(
                    proof,
                    &SimdChainingExecutionProof::visited_marks,
                    static_cast<std::uint64_t>(active_lanes));
                proof_add<TrackProof>(
                    proof,
                    &SimdChainingExecutionProof::
                        contiguous_predecessor_store_batches);
                ++route_contiguous_predecessor_store_batches;
            } else {
                hn::MaskedScatterIndex(
                    hn::Set(d, current_idx),
                    predecessor_marks,
                    d,
                    visited_generation.data(),
                    predecessor_indices);
                if constexpr (TrackProof) {
                    proof_add<true>(
                        proof,
                        &SimdChainingExecutionProof::visited_marks,
                        hn::CountTrue(d, predecessor_marks));
                }
            }

            const auto generations = full_batch
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
            proof_add<TrackProof>(
                proof,
                &SimdChainingExecutionProof::descending_candidate_steps,
                static_cast<std::uint64_t>(active_lanes));

            if (hn::AllFalse(d, can_improve)) {
                // best_score can only increase, so if no lane beats it at
                // batch entry no lane can improve later in this batch. Exact
                // max-skip state is then just the number of visited hits.
                proof_add<TrackProof>(
                    proof,
                    &SimdChainingExecutionProof::
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
                    proof_add<TrackProof>(
                        proof,
                        &SimdChainingExecutionProof::
                            scalar_orchestration_steps,
                        processed_lanes);
                    proof_add<TrackProof>(
                        proof,
                        &SimdChainingExecutionProof::
                            max_chain_skip_stops);
                } else {
                    skip_count += hit_count;
                }
            } else {
                // At least one vector-produced biological score may change
                // scalar state. Replay only this batch's control decisions in
                // exact descending order; no scalar score implementation is
                // called.
                proof_add<TrackProof>(
                    proof,
                    &SimdChainingExecutionProof::
                        improvement_replay_batches);
                ++route_improvement_replay_batches;
                proof_add<TrackProof>(
                    proof,
                    &SimdChainingExecutionProof::
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
                            proof_add<TrackProof>(
                                proof,
                                &SimdChainingExecutionProof::
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
                proof_add<TrackProof>(
                    proof,
                    &SimdChainingExecutionProof::
                        scalar_orchestration_steps,
                    processed_lane_count);
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
            if (!global_pair_score_cached) {
                ++route_score_vector_batches;
                ++route_score_vector_lanes;
                route_score_masked_lanes +=
                    static_cast<std::uint64_t>(lanes - 1);
            }
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
template<class D>
HWY_INLINE hn::Mask<D> update_bounded_lane_best(
    D d,
    hn::Vec<D> pair_scores,
    hn::Vec<D> predecessor_scores,
    hn::Mask<D> valid,
    std::int32_t load_start,
    hn::Vec<D>& lane_best_scores,
    hn::Vec<D>& lane_best_predecessors)
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

template<bool TrackProof, bool FullBatch, bool HasSkipPenalty, class D>
HWY_INLINE hn::Mask<D> score_bounded_candidate_batch(
    D d,
    std::size_t active_lanes,
    std::int32_t load_start,
    std::int32_t current_idx,
    const SoaAnchors& soa,
    std::span<const std::int32_t> best_scores,
    std::int32_t max_ref_dist,
    std::int32_t max_query_dist,
    const ChainingParams& params,
    hn::Vec<D>& lane_best_scores,
    hn::Vec<D>& lane_best_predecessors,
    SimdChainingExecutionProof* proof)
{
    const auto pair_scores = score_candidate_batch_specialized<
        TrackProof, FullBatch, HasSkipPenalty>(
        d,
        active_lanes,
        load_start,
        current_idx,
        soa,
        max_ref_dist,
        max_query_dist,
        params,
        proof);
    const auto invalid_score = hn::Set(
        d, std::numeric_limits<std::int32_t>::min());
    if constexpr (FullBatch) {
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

template<bool TrackProof, bool HasSkipPenalty, class D>
void simd_dp_forward_pass_bounded_impl(
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

        auto lane_best_scores = hn::Set(
            d, static_cast<std::int32_t>(
                   anchors[current_idx].query_span));
        auto lane_best_predecessors = hn::Set(d, -1);
        std::int32_t consecutive_nonimproving = 0;
        // No unscanned interval unless a cutoff replaces this with a value
        // strictly greater than window_start.
        std::int32_t unscanned_end_exclusive = window_start;

        auto record_batch = [&](hn::Mask<D> improved,
                                std::int32_t active_lanes,
                                std::int32_t load_start) {
            ++route_score_vector_batches;
            route_score_vector_lanes +=
                static_cast<std::uint64_t>(active_lanes);
            route_score_masked_lanes +=
                static_cast<std::uint64_t>(lanes - active_lanes);
            ++bounded_score_batches;
            proof_add<TrackProof>(
                proof,
                &SimdChainingExecutionProof::descending_candidate_steps,
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
                TrackProof, true, HasSkipPenalty>(
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
                proof);
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
                TrackProof, false, HasSkipPenalty>(
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
                proof);
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
                anchors[global_best_idx].ref_chromosome_key()
                    != anchors[current_idx].ref_chromosome_key()
                || anchors[current_idx].ref_position
                        - anchors[global_best_idx].ref_position
                    > static_cast<std::uint64_t>(max_ref_dist);
        }
        if (global_best_invalid) {
            route_global_best_vector_work =
                route_global_best_vector_work
                || window_start < current_idx;
            global_best_idx = exact_global_best<TrackProof>(
                d,
                best_scores.data(),
                window_start,
                current_idx,
                proof);
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
                score_one_candidate<TrackProof, HasSkipPenalty>(
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

    if constexpr (TrackProof) {
        SimdChainingPolicyEvidence bounded_evidence{
            .bounded_score_batches = bounded_score_batches,
            .bounded_improving_batches = bounded_improving_batches,
            .bounded_nonimproving_batches =
                bounded_nonimproving_batches,
            .bounded_cutoff_stops = bounded_cutoff_stops,
            .bounded_candidates_skipped = bounded_candidates_skipped,
            .global_best_rescue_probes = global_best_rescue_probes,
            .global_best_rescue_wins = global_best_rescue_wins,
        };
        proof->policy.combine(bounded_evidence);
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

template<bool TrackProof, class D>
void simd_dp_forward_pass_bounded(
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
    // Select the arithmetic policy once, outside every anchor/candidate loop.
    // The zero-penalty specialization removes gap-length float work while the
    // nonzero specialization preserves the general scoring formula.
    if (params.skip_penalty == 0.0F) {
        simd_dp_forward_pass_bounded_impl<TrackProof, false>(
            d,
            anchors,
            soa,
            best_scores,
            predecessors,
            peak_scores,
            params,
            proof,
            route_summary);
    } else {
        simd_dp_forward_pass_bounded_impl<TrackProof, true>(
            d,
            anchors,
            soa,
            best_scores,
            predecessors,
            peak_scores,
            params,
            proof,
            route_summary);
    }
}

/// Compile-time forward dispatch keeps the policy test outside the candidate
/// loop while sharing allocation, SoA conversion, and backtracking.
template<SimdChainingPolicy Policy, bool TrackProof, class D>
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
    if constexpr (Policy == SimdChainingPolicy::exact_visited) {
        simd_dp_forward_pass_exact<TrackProof>(
            d,
            anchors,
            soa,
            best_scores,
            predecessors,
            peak_scores,
            params,
            proof,
            route_summary);
    } else {
        static_assert(
            Policy
                == SimdChainingPolicy::bounded_batch_approximate);
        simd_dp_forward_pass_bounded<TrackProof>(
            d,
            anchors,
            soa,
            best_scores,
            predecessors,
            peak_scores,
            params,
            proof,
            route_summary);
    }
}

struct ScoredAnchorIndex {
    std::int32_t score;
    std::int32_t anchor_index;
};
static_assert(sizeof(ScoredAnchorIndex) == 8u);

struct ScoreAscending {
    [[nodiscard]] bool operator()(
        const ScoredAnchorIndex& lhs,
        const ScoredAnchorIndex& rhs) const noexcept
    {
        return lhs.score != rhs.score
            ? lhs.score < rhs.score
            : lhs.anchor_index < rhs.anchor_index;
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

                    proof_add<TrackProof>(
                        proof,
                        &SimdChainingExecutionProof::
                            backtrack_path_vector_batches);
                    proof_add<TrackProof>(
                        proof,
                        &SimdChainingExecutionProof::
                            backtrack_commit_vector_batches);
                    proof_add<TrackProof>(
                        proof,
                        &SimdChainingExecutionProof::
                            backtrack_vector_lanes,
                        static_cast<std::uint64_t>(lanes));
                    proof_add<TrackProof>(
                        proof,
                        &SimdChainingExecutionProof::
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

            proof_add<TrackProof>(
                proof,
                &SimdChainingExecutionProof::
                    backtrack_path_vector_batches);
            proof_add<TrackProof>(
                proof,
                &SimdChainingExecutionProof::
                    backtrack_commit_vector_batches);
            proof_add<TrackProof>(
                proof,
                &SimdChainingExecutionProof::backtrack_vector_lanes);
        }
        if (!hn::AllFalse(d, active)) {
            reject(
                proof,
                SimdChainingStatus::cyclic_predecessors,
                "primary predecessor path exceeds anchor count");
        }

        proof_add<TrackProof>(
            proof,
            &SimdChainingExecutionProof::backtrack_rank_batches);
        route_rank_work = true;
        const std::int32_t primary_score = scored[highest_rank].score;
        if (primary_score >= min_score
            && primary_length >= min_seed_count)
        {
            chains_out.push_back(Chain{
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
            proof_add<TrackProof>(
                proof,
                &SimdChainingExecutionProof::backtrack_overlap_stops,
                visited_hit_count);

            // After the primary chain is claimed, a ranked batch commonly
            // starts entirely inside that chain.  The exact path below would
            // stop all lanes at depth zero, then reject each zero-length,
            // zero-score chain.  When either threshold rejects that outcome,
            // preserve the same proof-visible rank/path work and skip the
            // otherwise empty per-lane commit loop.
            if (depth == 0
                && visited_hit_count
                    == static_cast<std::uint64_t>(batch_count)
                && (min_score > 0 || min_seed_count > 0))
            {
                proof_add<TrackProof>(
                    proof,
                    &SimdChainingExecutionProof::
                        backtrack_path_vector_batches);
                proof_add<TrackProof>(
                    proof,
                    &SimdChainingExecutionProof::backtrack_rank_batches);
                proof_add<TrackProof>(
                    proof,
                    &SimdChainingExecutionProof::
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

            proof_add<TrackProof>(
                proof,
                &SimdChainingExecutionProof::
                    backtrack_path_vector_batches);
            proof_add<TrackProof>(
                proof,
                &SimdChainingExecutionProof::backtrack_vector_lanes,
                hn::CountTrue(d, append));
        }
        if (all_starts_visited_batch_rejected) {
            continue;
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
                ++route_scalar_orchestration_steps;
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
        route_summary->backtrack_primary_contiguous_batches +=
            route_primary_contiguous_batches;
        route_summary->backtrack_all_starts_visited_fast_batches +=
            route_all_starts_visited_fast_batches;
        route_summary->scalar_orchestration_steps +=
            route_scalar_orchestration_steps;
    }
}

template<bool TrackProof, class D>
void run_simd_chaining(
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

    switch (params.simd_policy) {
        case SimdChainingPolicy::exact_visited:
            simd_dp_forward_pass<
                SimdChainingPolicy::exact_visited, TrackProof>(
                    d,
                    anchors,
                    soa,
                    best_scores,
                    predecessors,
                    peak_scores,
                    params,
                    proof,
                    route_summary);
            break;
        case SimdChainingPolicy::bounded_batch_approximate:
            simd_dp_forward_pass<
                SimdChainingPolicy::bounded_batch_approximate, TrackProof>(
                    d,
                    anchors,
                    soa,
                    best_scores,
                    predecessors,
                    peak_scores,
                    params,
                    proof,
                    route_summary);
            break;
        default:
            reject(
                proof,
                SimdChainingStatus::invalid_policy,
                "unknown SIMD chaining policy reached forward dispatch");
    }
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
 * Named SIMD DP chaining. Supplying proof is optional and does not alter the
 * requested policy. Unsupported inputs throw; this entry point never calls a
 * scalar scoring or backtracking implementation.
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
    switch (params.simd_policy) {
        case SimdChainingPolicy::exact_visited:
        case SimdChainingPolicy::bounded_batch_approximate:
            break;
        default:
            chaining_simd_detail::reject(
                proof,
                SimdChainingStatus::invalid_policy,
                "unknown SIMD chaining policy");
    }
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
    if (params.simd_policy
            == SimdChainingPolicy::bounded_batch_approximate
        && (params.max_chain_skip <= 0
            || params.max_chain_iter <= 0))
    {
        if (proof != nullptr) {
            ++proof->policy.policy_rejections;
        }
        chaining_simd_detail::reject(
            proof,
            SimdChainingStatus::invalid_parameters,
            "bounded SIMD chaining requires positive max_chain_skip and "
            "max_chain_iter");
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
    if (proof != nullptr) {
        switch (params.simd_policy) {
            case SimdChainingPolicy::exact_visited:
                ++proof->policy.exact_visited_calls;
                break;
            case SimdChainingPolicy::bounded_batch_approximate:
                ++proof->policy.bounded_batch_approximate_calls;
                break;
            default:
                // The policy was validated before all allocation/workspace
                // checks, so this branch is intentionally unreachable.
                chaining_simd_detail::reject(
                    proof,
                    SimdChainingStatus::invalid_policy,
                    "unknown SIMD chaining policy after validation");
        }
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
        chaining_simd_detail::run_simd_chaining<false>(
            d,
            anchors,
            params,
            chains_out,
            reordered_anchors_out,
            nullptr,
            &route_summary);
        proof->score_vector_batches =
            route_summary.score_vector_batches;
        proof->score_vector_lanes = route_summary.score_vector_lanes;
        proof->score_masked_lanes = route_summary.score_masked_lanes;
        proof->no_improvement_fast_batches =
            route_summary.no_improvement_fast_batches;
        proof->improvement_replay_batches =
            route_summary.improvement_replay_batches;
        proof->improvement_replay_lanes =
            route_summary.improvement_replay_lanes;
        proof->contiguous_predecessor_store_batches =
            route_summary.contiguous_predecessor_store_batches;
        proof->scalar_orchestration_steps =
            route_summary.scalar_orchestration_steps;
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
        proof->backtrack_primary_contiguous_batches =
            route_summary.backtrack_primary_contiguous_batches;
        proof->backtrack_all_starts_visited_fast_batches =
            route_summary.backtrack_all_starts_visited_fast_batches;
        if (params.simd_policy
            == SimdChainingPolicy::bounded_batch_approximate)
        {
            SimdChainingPolicyEvidence bounded_evidence{
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
            proof->policy.combine(bounded_evidence);
        }
    } else if (proof != nullptr) {
        chaining_simd_detail::run_simd_chaining<true>(
            d,
            anchors,
            params,
            chains_out,
            reordered_anchors_out,
            proof,
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
    bioseq_log<DBG>(
        "[chaining_simd] {} SIMD chaining complete for {} anchors",
        bioseq::chaining::simd_chaining_policy_name(
            params.simd_policy),
        anchors.size());
}

}  // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

}  // export

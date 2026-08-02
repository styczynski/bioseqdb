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

#include <hwy/highway.h>

#include "common.h"
#include "pg/scheduler/dispatch_processor_hwy.h"
#include "sequences/chaining/chaining_simd_abi.h"

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

import bioseq.sequences.chaining.utils;

export {

namespace bioseq::chaining {

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
    ::bioseq::chaining::SimdChainingStatus status) noexcept
{
    switch (status) {
        case ::bioseq::chaining::SimdChainingStatus::ok: return "ok";
        case ::bioseq::chaining::SimdChainingStatus::unsupported_simd_target:
            return "unsupported_simd_target";
        case ::bioseq::chaining::SimdChainingStatus::invalid_parameters:
            return "invalid_parameters";
        case ::bioseq::chaining::SimdChainingStatus::invalid_policy:
            return "invalid_policy";
        case ::bioseq::chaining::SimdChainingStatus::anchor_count_overflow:
            return "anchor_count_overflow";
        case ::bioseq::chaining::SimdChainingStatus::workspace_overflow:
            return "workspace_overflow";
        case ::bioseq::chaining::SimdChainingStatus::cyclic_predecessors:
            return "cyclic_predecessors";
    }
    return "unknown";
}

class SimdChainingRuntimeError final : public std::runtime_error {
public:
    SimdChainingRuntimeError(
        ::bioseq::chaining::SimdChainingStatus status,
        std::string_view detail)
        : std::runtime_error(
              std::string{"SIMD chaining rejected: "}
              + std::string{::bioseq::chaining::simd_chaining_status_name(status)}
              + ": " + std::string{detail}),
          status_(status) {}

    [[nodiscard]] ::bioseq::chaining::SimdChainingStatus status() const noexcept {
        return status_;
    }

private:
    ::bioseq::chaining::SimdChainingStatus status_;
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
    void combine(const ::bioseq::chaining::SimdChainingPolicyEvidence& other) noexcept {
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
        const ::bioseq::chaining::SimdChainingPolicyEvidence&) const noexcept = default;
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
    ::bioseq::chaining::SimdChainingPolicyEvidence policy{};
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

    void combine(const ::bioseq::chaining::SimdChainingExecutionProof& other) noexcept {
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
        const ::bioseq::chaining::SimdChainingExecutionProof&) const noexcept = default;

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

} // namespace bioseq::chaining

namespace bioseq::chaining {

/**
 * Neutral chaining facade. The dispatch processor installs exactly one table
 * for its selected Highway target; this facade performs no target selection
 * and forwards the complete algorithm to that table's direct function.
 */
void chain_anchors_simd(
    std::span<const Anchor> anchors,
    const ChainingParams& params,
    ArenaSlab<Chain>& chains_out,
    ArenaSlab<Anchor>& reordered_anchors_out,
    SimdChainingExecutionProof* proof = nullptr,
    bool aggregate_proof = false)
{
    if (params.highway_kernels == nullptr
        || params.highway_kernels->chain_anchors_simd == nullptr)
    {
        if (proof != nullptr) {
            *proof = {};
            proof->anchors_consumed = anchors.size();
            ++proof->fail_closed_rejections;
        }
        throw SimdChainingRuntimeError(
            SimdChainingStatus::unsupported_simd_target,
            "no selected Highway chaining kernel is installed");
    }

    bioseq::simd::ChainAnchorsSimdRequest request{
        .anchors = anchors.data(),
        .anchor_count = anchors.size(),
        .params = &params,
        .chains_out = &chains_out,
        .reordered_anchors_out = &reordered_anchors_out,
        .proof = proof,
        .aggregate_proof = aggregate_proof,
    };
    params.highway_kernels->chain_anchors_simd(&request);
}

}  // namespace bioseq::chaining

}  // export

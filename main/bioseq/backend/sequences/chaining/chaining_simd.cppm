/**
 * @file chaining_simd.cppm
 * @brief Target-neutral contracts and facade for SIMD DP chaining.
 *
 * The complete Highway implementation is generated per target from
 * chaining_simd_kernel.cppm. This module intentionally contains no Highway
 * headers, target namespaces, or runtime dispatch. Callers supply the function
 * selected once by the dispatch processor.
 */
module;

#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

export module bioseq.sequences.chaining.simd;

import bioseq.sequences.chaining.utils;

export namespace bioseq::chaining {

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

    // Forbidden paths. They are never incremented by this implementation;
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

/** Exact target-neutral ABI for a fixed-target chaining implementation. */
using ChainAnchorsSimdKernel = void (*)(
    std::span<const Anchor>,
    const ChainingParams&,
    ArenaSlab<Chain>&,
    ArenaSlab<Anchor>&,
    SimdChainingExecutionProof*,
    bool);

static_assert(std::is_pointer_v<ChainAnchorsSimdKernel>);

/**
 * Target-neutral forwarding facade for independent callers.
 *
 * This call is static with respect to Highway: the caller supplies the target
 * function that was selected at the process dispatch root (or an explicitly
 * chosen fixed-target function in a focused test).
 */
inline void chain_anchors_simd(
    ChainAnchorsSimdKernel kernel,
    std::span<const Anchor> anchors,
    const ChainingParams& params,
    ArenaSlab<Chain>& chains_out,
    ArenaSlab<Anchor>& reordered_anchors_out,
    SimdChainingExecutionProof* proof = nullptr,
    bool aggregate_proof = false)
{
    if (kernel == nullptr) {
        if (proof != nullptr) {
            *proof = {};
            proof->anchors_consumed = anchors.size();
            ++proof->fail_closed_rejections;
        }
        throw SimdChainingRuntimeError(
            SimdChainingStatus::unsupported_simd_target,
            "no fixed-target Highway chaining kernel is installed");
    }
    kernel(
        anchors,
        params,
        chains_out,
        reordered_anchors_out,
        proof,
        aggregate_proof);
}

}  // namespace bioseq::chaining

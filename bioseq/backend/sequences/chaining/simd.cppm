/**
 * @file simd.cppm
 * @brief Shared types and call interface for SIMD chaining.
 *
 * Highway generates the CPU-specific implementations from the chaining SIMD
 * modules. Callers select a function once and pass it here with their anchors,
 * settings, and output arrays.
 */
module;

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

export module bioseq.chaining.simd;

import bioseq.chaining.core;
import bioseq.common.checked_arithmetic;

export namespace bioseq::chaining {

/// Compute the total element count for three SIMD input arrays, checking overflow.
[[nodiscard]] constexpr std::optional<std::size_t>
simd_chaining_soa_elements(std::size_t count) noexcept {
    return bioseq::common::checked_mul(count, std::size_t{3u});
}

/// Size one full-length backtracking path per SIMD lane.
/// Return no size if the lane count or the 32-bit gather indices cannot support it.
[[nodiscard]] constexpr std::optional<std::size_t>
simd_chaining_path_elements(std::size_t count, std::size_t lanes) noexcept {
    if (lanes < 2u || count > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)())) {
        return std::nullopt;
    }
    const auto elements = bioseq::common::checked_mul(count, lanes);
    if (!elements ||
        *elements > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)()) ||
        !bioseq::common::checked_mul(*elements, sizeof(std::int32_t))) {
        return std::nullopt;
    }
    return elements;
}

/// SIMD scratch-array sizes. The context reserves memory for their allocations.
struct simd_chaining_storage_plan {
    chaining_output_storage_plan output;
    std::size_t soa_u32_elements;
    std::size_t forward_visited_elements;
    std::size_t logical_bytes_before_lane_paths;

    /// Compute path storage using the selected CPU target's lane count.
    [[nodiscard]] constexpr std::optional<std::size_t>
    path_elements(std::size_t lanes) const noexcept {
        return simd_chaining_path_elements(output.anchor_count, lanes);
    }

    [[nodiscard]] constexpr std::optional<std::size_t>
    logical_scratch_bytes(std::size_t lanes) const noexcept {
        const auto elements = path_elements(lanes);
        if (!elements) {
            return std::nullopt;
        }
        const auto bytes = bioseq::common::checked_mul(*elements, sizeof(std::int32_t));
        return bytes ? bioseq::common::checked_add(logical_bytes_before_lane_paths, *bytes)
                     : std::nullopt;
    }
};

[[nodiscard]] constexpr std::optional<simd_chaining_storage_plan>
derive_simd_chaining_storage_plan(std::size_t count, const chaining_parameters& params) noexcept {
    if (count > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)()) ||
        (params.simd_policy != simd_chaining_policy::exact_visited &&
         params.simd_policy != simd_chaining_policy::bounded_batch_approximate)) {
        return std::nullopt;
    }
    const auto output = derive_chaining_output_storage_plan(count, params.min_seed_count);
    const auto soa = simd_chaining_soa_elements(count);
    const bool exact = params.simd_policy == simd_chaining_policy::exact_visited;
    // Count the three input arrays, three scoring arrays, backtracking
    // scores and indices, and visited marks. Only exact_visited also needs
    // visited marks during forward scoring.
    const auto bytes = bioseq::common::checked_mul(count,
        (9u + static_cast<std::size_t>(exact)) * sizeof(std::int32_t));
    if (!output || !soa || !bytes) {
        return std::nullopt;
    }
    return simd_chaining_storage_plan{*output, *soa, exact ? count : 0u, *bytes};
}

/// Completion status, including reasons the selected SIMD chainer was rejected.
enum class simd_chaining_status : std::uint8_t {
    ok = 0,
    unsupported_simd_target,
    invalid_parameters,
    invalid_policy,
    anchor_count_overflow,
    workspace_overflow,
    cyclic_predecessors,
};

/// Return the diagnostic name of @p status, or "unknown".
[[nodiscard]] constexpr std::string_view simd_chaining_status_name(
    simd_chaining_status status) noexcept
{
    switch (status) {
        case simd_chaining_status::ok: return "ok";
        case simd_chaining_status::unsupported_simd_target:
            return "unsupported_simd_target";
        case simd_chaining_status::invalid_parameters:
            return "invalid_parameters";
        case simd_chaining_status::invalid_policy:
            return "invalid_policy";
        case simd_chaining_status::anchor_count_overflow:
            return "anchor_count_overflow";
        case simd_chaining_status::workspace_overflow:
            return "workspace_overflow";
        case simd_chaining_status::cyclic_predecessors:
            return "cyclic_predecessors";
    }
    return "unknown";
}

/// Error raised when the selected SIMD backend rejects execution.
class simd_chaining_runtime_error final : public std::runtime_error {
public:
    /// Construct an error carrying the machine-readable @p status.
    simd_chaining_runtime_error(
        simd_chaining_status status,
        std::string_view detail)
        : std::runtime_error(
              std::string{"SIMD chaining rejected: "}
              + std::string{simd_chaining_status_name(status)}
              + ": " + std::string{detail}),
          status_(status) {}

    /// Return the machine-readable rejection status.
    [[nodiscard]] simd_chaining_status status() const noexcept {
        return status_;
    }

private:
    simd_chaining_status status_;
};

/// Counters for each SIMD predecessor-search policy.
/// Count each bounded-search batch as improving or nonimproving. Keep the
/// extra probe of the cached best anchor in separate counters.
struct simd_chaining_policy_stats {
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

    /**
     * @brief Add to a counter, clamping overflow to the largest uint64_t value.
     * @param destination Counter to update.
     * @param increment Amount to add.
     * @param did_saturate Set to true if this addition overflows.
     */
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

    /// @brief Merge policy counters, clamping any overflowing totals.
    /// @param other Counters to add.
    void merge(const simd_chaining_policy_stats& other) noexcept {
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

    /// @brief Check that the counters agree with the selected search policy.
    /// @param selected Expected policy.
    /// @param require_accelerated_work For bounded search, also require batches, an early stop, and skipped candidates.
    /// @return False for rejected calls, overflow, or inconsistent counters.
    [[nodiscard]] constexpr bool is_internally_consistent(
        simd_chaining_policy selected,
        bool require_accelerated_work) const noexcept
    {
        if (saturated || policy_rejections != 0) {
            return false;
        }
        switch (selected) {
            case simd_chaining_policy::exact_visited:
                return exact_visited_calls > 0
                    && bounded_batch_approximate_calls == 0
                    && bounded_score_batches == 0
                    && bounded_improving_batches == 0
                    && bounded_nonimproving_batches == 0
                    && bounded_cutoff_stops == 0
                    && bounded_candidates_skipped == 0
                    && global_best_rescue_probes == 0
                    && global_best_rescue_wins == 0;
            case simd_chaining_policy::bounded_batch_approximate: {
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
        const simd_chaining_policy_stats&) const noexcept = default;
};

/**
 * @brief Counters for SIMD scoring, predecessor search, and backtracking.
 *
 * Callers merge these into pipeline and benchmark results. Consistency checks
 * reject overflow, rejected calls, and any recorded scalar scoring or backtracking.
 */
struct simd_chaining_execution_stats {
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
    // Sequential decisions made from SIMD results, such as applying skip
    // rules or committing a recovered chain.
    std::uint64_t scalar_orchestration_steps = 0;
    std::uint64_t fail_closed_rejections = 0;

    // Reserved for counts of scalar scoring or backtracking calls.
    // SIMD consistency checks require all three counters to be zero.
    std::uint64_t scalar_score_branches = 0;
    std::uint64_t scalar_backtrack_branches = 0;
    std::uint64_t shared_scalar_backtrack_calls = 0;
    simd_chaining_policy_stats policy{};
    bool saturated = false;

    /**
     * @brief Add to a counter, clamping overflow to the largest uint64_t value.
     * @param destination Counter to update.
     * @param increment Amount to add.
     * @param did_saturate Set to true if this addition overflows.
     */
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

    /// @brief Merge execution and policy counters, clamping overflowing totals.
    /// @param other Counters to add.
    void merge(const simd_chaining_execution_stats& other) noexcept {
        saturated = saturated || other.saturated;
        policy.merge(other.policy);
#define BIOSEQ_MERGE_SIMD_CHAIN_COUNTER(name) \
        add_counter(name, other.name, saturated)
        BIOSEQ_MERGE_SIMD_CHAIN_COUNTER(anchors_consumed);
        BIOSEQ_MERGE_SIMD_CHAIN_COUNTER(score_vector_batches);
        BIOSEQ_MERGE_SIMD_CHAIN_COUNTER(score_vector_lanes);
        BIOSEQ_MERGE_SIMD_CHAIN_COUNTER(score_masked_lanes);
        BIOSEQ_MERGE_SIMD_CHAIN_COUNTER(descending_candidate_steps);
        BIOSEQ_MERGE_SIMD_CHAIN_COUNTER(visited_marks);
        BIOSEQ_MERGE_SIMD_CHAIN_COUNTER(max_chain_skip_stops);
        BIOSEQ_MERGE_SIMD_CHAIN_COUNTER(no_improvement_fast_batches);
        BIOSEQ_MERGE_SIMD_CHAIN_COUNTER(improvement_replay_batches);
        BIOSEQ_MERGE_SIMD_CHAIN_COUNTER(improvement_replay_lanes);
        BIOSEQ_MERGE_SIMD_CHAIN_COUNTER(
            contiguous_predecessor_store_batches);
        BIOSEQ_MERGE_SIMD_CHAIN_COUNTER(global_best_vector_batches);
        BIOSEQ_MERGE_SIMD_CHAIN_COUNTER(global_best_vector_lanes);
        BIOSEQ_MERGE_SIMD_CHAIN_COUNTER(backtrack_rank_batches);
        BIOSEQ_MERGE_SIMD_CHAIN_COUNTER(backtrack_path_vector_batches);
        BIOSEQ_MERGE_SIMD_CHAIN_COUNTER(backtrack_commit_vector_batches);
        BIOSEQ_MERGE_SIMD_CHAIN_COUNTER(backtrack_vector_lanes);
        BIOSEQ_MERGE_SIMD_CHAIN_COUNTER(backtrack_overlap_stops);
        BIOSEQ_MERGE_SIMD_CHAIN_COUNTER(
            backtrack_primary_contiguous_batches);
        BIOSEQ_MERGE_SIMD_CHAIN_COUNTER(
            backtrack_all_starts_visited_fast_batches);
        BIOSEQ_MERGE_SIMD_CHAIN_COUNTER(scalar_orchestration_steps);
        BIOSEQ_MERGE_SIMD_CHAIN_COUNTER(fail_closed_rejections);
        BIOSEQ_MERGE_SIMD_CHAIN_COUNTER(scalar_score_branches);
        BIOSEQ_MERGE_SIMD_CHAIN_COUNTER(scalar_backtrack_branches);
        BIOSEQ_MERGE_SIMD_CHAIN_COUNTER(shared_scalar_backtrack_calls);
#undef BIOSEQ_MERGE_SIMD_CHAIN_COUNTER
    }

    /// @brief Check counters for the requested SIMD policy and zero scalar-work counts.
    /// @param selected Expected predecessor-search policy.
    /// @param require_nonempty_vector_work Also require recorded SIMD scoring and backtracking.
    /// @return True when the requested checks pass.
    [[nodiscard]] constexpr bool is_internally_consistent(
            simd_chaining_policy selected,
            bool require_nonempty_vector_work = false) const noexcept {
        const bool authentic_backend = !saturated
            && fail_closed_rejections == 0
            && scalar_score_branches == 0
            && scalar_backtrack_branches == 0
            && shared_scalar_backtrack_calls == 0
            && has_consistent_policy_stats(
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
        const simd_chaining_execution_stats&) const noexcept = default;

    /// @brief Check policy counters and, for exact_visited, the forward-search totals.
    /// @param selected Expected policy.
    /// @param require_accelerated_work Require an early bounded-search stop with skipped candidates.
    /// @return True when these counter relationships hold.
    [[nodiscard]] constexpr bool has_consistent_policy_stats(
        simd_chaining_policy selected,
        bool require_accelerated_work = false) const noexcept
    {
        if (!policy.is_internally_consistent(
                selected, require_accelerated_work)) {
            return false;
        }
        switch (selected) {
            case simd_chaining_policy::exact_visited:
                return has_consistent_exact_execution_stats();
            case simd_chaining_policy::bounded_batch_approximate:
                return true;
        }
        return false;
    }

    /// @brief Check counts of exact-policy batches and sequential candidate replay.
    /// @return True when classified batches and replayed lanes fit the recorded totals.
    [[nodiscard]] constexpr bool has_consistent_exact_execution_stats()
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

/** @brief Function-pointer type for a CPU-specific SIMD chaining implementation. */
using chain_anchors_simd_kernel = void (*)(
    std::span<const anchor>,
    const chaining_parameters&,
    arena_slab<chain>&,
    arena_slab<anchor>&,
    simd_chaining_execution_stats*,
    bool);

static_assert(std::is_pointer_v<chain_anchors_simd_kernel>);

/**
 * @brief Call the SIMD chaining function already selected by the caller.
 *
 * CPU feature selection happens before this call. A missing function raises
 * an error and records the rejected call when counters are supplied.
 * @param kernel Function for the selected CPU target.
 * @param anchors Anchors in canonical_anchor_less order.
 * @param params Scoring rules, distance limits, and SIMD policy.
 * @param chains_out Receives chain descriptors in arena storage.
 * @param reordered_anchors_out Receives anchors grouped in chain order.
 * @param stats Optional counters updated by the kernel.
 * @param aggregate_stats Ask the kernel for its shorter set of counters.
 * @throws simd_chaining_runtime_error If kernel is null.
 */
inline void chain_anchors_simd(
    chain_anchors_simd_kernel kernel,
    std::span<const anchor> anchors,
    const chaining_parameters& params,
    arena_slab<chain>& chains_out,
    arena_slab<anchor>& reordered_anchors_out,
    simd_chaining_execution_stats* stats = nullptr,
    bool aggregate_stats = false)
{
    if (kernel == nullptr) {
        if (stats != nullptr) {
            *stats = {};
            stats->anchors_consumed = anchors.size();
            ++stats->fail_closed_rejections;
        }
        throw simd_chaining_runtime_error(
            simd_chaining_status::unsupported_simd_target,
            "no fixed-target Highway chaining kernel is installed");
    }
    kernel(
        anchors,
        params,
        chains_out,
        reordered_anchors_out,
        stats,
        aggregate_stats);
}

}  // namespace bioseq::chaining

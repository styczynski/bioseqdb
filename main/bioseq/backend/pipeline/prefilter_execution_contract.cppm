/**
 * @file prefilter_execution_contract.cppm
 * @brief Target-neutral rank-prefilter execution evidence.
 */
module;

#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

export module bioseq.prefilter.execution_contract;

import bioseq.pipeline.runtime_contract;

export {

// Rank-prefilter proof is target-independent statement evidence. Keeping it
// outside the Highway namespace gives every compiled SIMD target one shared
// type, so PostgreSQL can aggregate operation proofs without translating or
// mirroring their counters.
namespace bioseq::prefilter_stats {

/// Algorithm resolved for one completed prefilter batch.
enum class ResolvedPrefilterAlgorithm : std::uint8_t {
    sneaky_snake = 0u,
    gatekeeper = 1u,
};

[[nodiscard]] constexpr const char* prefilter_algorithm_name(
    ResolvedPrefilterAlgorithm algorithm) noexcept
{
    switch (algorithm) {
        case ResolvedPrefilterAlgorithm::sneaky_snake: return "sneaky_snake";
        case ResolvedPrefilterAlgorithm::gatekeeper: return "gatekeeper";
    }
    return "unknown";
}

/// Operation-originated evidence for the rank-prefilter stage.
///
/// A selector never populates this value.  The stage records one dispatch only
/// after the selected backend has returned a complete result batch and the
/// per-region survivor decision has been applied.  This makes the proof useful
/// to benchmark and SQL callers that must distinguish requested policy from
/// real maze execution.
struct PrefilterExecutionProof {
    std::uint64_t dispatches = 0;
    std::uint64_t sneaky_snake_dispatches = 0;
    std::uint64_t gatekeeper_dispatches = 0;
    std::uint64_t scalar_dispatches = 0;
    std::uint64_t simd_dispatches = 0;
    std::uint64_t vulkan_dispatches = 0;
    std::uint64_t jobs = 0;
    std::uint64_t evaluated_jobs = 0;
    /// Supported equal-length 64-symbol jobs that returned success.
    std::uint64_t length_64_jobs = 0;
    std::uint64_t length_difference_jobs = 0;
    std::uint64_t unsupported_jobs = 0;
    std::uint64_t rejected_jobs = 0;
    std::uint64_t rejected_regions = 0;
    std::uint64_t surviving_regions = 0;
    std::uint64_t mask_rejection_candidates = 0;
    std::uint64_t exact_guard_activations = 0;
    std::uint64_t guard_confirmed_rejections = 0;
    std::uint64_t hamming_exact_rejections = 0;
    bool saturated = false;

    static constexpr void add(
            std::uint64_t& destination,
            const std::uint64_t increment,
            bool& saturated_flag) noexcept {
        if (increment > (std::numeric_limits<std::uint64_t>::max)()
                            - destination) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            saturated_flag = true;
            return;
        }
        destination += increment;
    }

    constexpr void record_batch(
            const ResolvedPrefilterAlgorithm algorithm,
            const PrefilterBackendType backend,
            const std::uint64_t job_count,
            const std::uint64_t evaluated_count,
            const std::uint64_t length_difference_count,
            const std::uint64_t unsupported_count,
            const std::uint64_t rejected_job_count,
            const std::uint64_t rejected_region_count,
            const std::uint64_t surviving_region_count,
            const std::uint64_t mask_rejection_candidate_count = 0u,
            const std::uint64_t exact_guard_activation_count = 0u,
            const std::uint64_t guard_confirmed_rejection_count = 0u,
            const std::uint64_t hamming_exact_rejection_count = 0u,
            const std::uint64_t length_64_count = 0u) noexcept {
        add(dispatches, 1u, saturated);
        if (length_64_count > evaluated_count) saturated = true;
        switch (algorithm) {
            case ResolvedPrefilterAlgorithm::sneaky_snake:
                add(sneaky_snake_dispatches, 1u, saturated);
                if (mask_rejection_candidate_count != 0u
                    || exact_guard_activation_count != 0u
                    || guard_confirmed_rejection_count != 0u
                    || hamming_exact_rejection_count != 0u) {
                    saturated = true;
                }
                break;
            case ResolvedPrefilterAlgorithm::gatekeeper:
                add(gatekeeper_dispatches, 1u, saturated);
                if (guard_confirmed_rejection_count
                        > exact_guard_activation_count
                    || exact_guard_activation_count
                        > mask_rejection_candidate_count
                    || mask_rejection_candidate_count > evaluated_count
                    || guard_confirmed_rejection_count > rejected_job_count
                    || hamming_exact_rejection_count
                        != rejected_job_count
                            - guard_confirmed_rejection_count) {
                    saturated = true;
                }
                break;
        }
        switch (backend) {
            case PrefilterBackendType::SCALAR:
                add(scalar_dispatches, 1u, saturated);
                break;
            case PrefilterBackendType::SIMD:
                add(simd_dispatches, 1u, saturated);
                break;
            case PrefilterBackendType::VULKAN:
                add(vulkan_dispatches, 1u, saturated);
                break;
            case PrefilterBackendType::AUTO:
                // AUTO must be resolved before an operation can be attested.
                saturated = true;
                break;
        }
        add(jobs, job_count, saturated);
        add(evaluated_jobs, evaluated_count, saturated);
        add(length_64_jobs, length_64_count, saturated);
        add(length_difference_jobs, length_difference_count, saturated);
        add(unsupported_jobs, unsupported_count, saturated);
        add(rejected_jobs, rejected_job_count, saturated);
        add(rejected_regions, rejected_region_count, saturated);
        add(surviving_regions, surviving_region_count, saturated);
        add(mask_rejection_candidates,
            mask_rejection_candidate_count, saturated);
        add(exact_guard_activations,
            exact_guard_activation_count, saturated);
        add(guard_confirmed_rejections,
            guard_confirmed_rejection_count, saturated);
        add(hamming_exact_rejections,
            hamming_exact_rejection_count, saturated);
    }

    constexpr void combine(
            const PrefilterExecutionProof& other) noexcept {
        // A caller that marks a proof present is attesting a complete batch
        // or statement aggregate.  Malformed evidence must remain sticky:
        // adding its numeric zeroes to a valid aggregate must never make the
        // final proof appear valid.
        bool overflow = saturated || !other.valid();
        add(dispatches, other.dispatches, overflow);
        add(sneaky_snake_dispatches,
            other.sneaky_snake_dispatches, overflow);
        add(gatekeeper_dispatches, other.gatekeeper_dispatches, overflow);
        add(scalar_dispatches, other.scalar_dispatches, overflow);
        add(simd_dispatches, other.simd_dispatches, overflow);
        add(vulkan_dispatches, other.vulkan_dispatches, overflow);
        add(jobs, other.jobs, overflow);
        add(evaluated_jobs, other.evaluated_jobs, overflow);
        add(length_64_jobs, other.length_64_jobs, overflow);
        add(length_difference_jobs, other.length_difference_jobs, overflow);
        add(unsupported_jobs, other.unsupported_jobs, overflow);
        add(rejected_jobs, other.rejected_jobs, overflow);
        add(rejected_regions, other.rejected_regions, overflow);
        add(surviving_regions, other.surviving_regions, overflow);
        add(mask_rejection_candidates,
            other.mask_rejection_candidates, overflow);
        add(exact_guard_activations, other.exact_guard_activations, overflow);
        add(guard_confirmed_rejections,
            other.guard_confirmed_rejections, overflow);
        add(hamming_exact_rejections,
            other.hamming_exact_rejections, overflow);
        saturated = overflow;
    }

    /// Validate algorithm/backend accounting without requiring one backend.
    /// A statement aggregate may legitimately contain more than one resolved
    /// algorithm or backend across completed batches.
    [[nodiscard]] constexpr bool valid(
            const bool require_evaluated_jobs = false,
            const bool forbid_unsupported_jobs = false) const noexcept {
        return !(saturated || dispatches == 0u || jobs == 0u
            || sneaky_snake_dispatches > dispatches
            || gatekeeper_dispatches
                != dispatches - sneaky_snake_dispatches
            || evaluated_jobs > jobs
            || length_64_jobs > evaluated_jobs
            || length_difference_jobs > jobs - evaluated_jobs
            || unsupported_jobs
                != jobs - evaluated_jobs - length_difference_jobs
            || rejected_jobs > jobs
            || guard_confirmed_rejections > exact_guard_activations
            || exact_guard_activations > mask_rejection_candidates
            || mask_rejection_candidates > evaluated_jobs
            || guard_confirmed_rejections > rejected_jobs
            || hamming_exact_rejections
                > rejected_jobs - guard_confirmed_rejections
            || (gatekeeper_dispatches == 0u
                && (mask_rejection_candidates != 0u
                    || exact_guard_activations != 0u
                    || guard_confirmed_rejections != 0u
                    || hamming_exact_rejections != 0u))
            || rejected_regions > (std::numeric_limits<std::uint64_t>::max)()
                    - surviving_regions
            || rejected_regions + surviving_regions == 0u
            || scalar_dispatches > dispatches
            || simd_dispatches > dispatches - scalar_dispatches
            || vulkan_dispatches
                != dispatches - scalar_dispatches - simd_dispatches
            || (require_evaluated_jobs && evaluated_jobs == 0u)
            || (forbid_unsupported_jobs && unsupported_jobs != 0u));
    }

    [[nodiscard]] constexpr bool accepted(
            const PrefilterBackendType expected_backend,
            const bool require_evaluated_jobs = false,
            const bool forbid_unsupported_jobs = false) const noexcept {
        if (!valid(require_evaluated_jobs, forbid_unsupported_jobs)) return false;
        switch (expected_backend) {
            case PrefilterBackendType::SCALAR:
                return scalar_dispatches == dispatches
                    && simd_dispatches == 0u
                    && vulkan_dispatches == 0u;
            case PrefilterBackendType::SIMD:
                return scalar_dispatches == 0u
                    && simd_dispatches == dispatches
                    && vulkan_dispatches == 0u;
            case PrefilterBackendType::VULKAN:
                return scalar_dispatches == 0u
                    && simd_dispatches == 0u
                    && vulkan_dispatches == dispatches;
            case PrefilterBackendType::AUTO:
                return false;
        }
        return false;
    }

    /// Return the operation-proven algorithm identity of this aggregate.
    [[nodiscard]] constexpr const char* resolved_algorithm_name() const noexcept {
        if (!valid()) return "invalid";
        if (gatekeeper_dispatches == dispatches) return "gatekeeper";
        if (sneaky_snake_dispatches == dispatches) return "sneaky_snake";
        return "mixed";
    }

    /// Return the operation-proven backend identity of this aggregate.
    [[nodiscard]] constexpr const char* resolved_backend_name() const noexcept {
        if (!valid()) return "invalid";
        if (scalar_dispatches == dispatches) return "scalar";
        if (simd_dispatches == dispatches) return "highway_simd";
        if (vulkan_dispatches == dispatches) return "vulkan";
        return "mixed";
    }

    constexpr bool operator==(
        const PrefilterExecutionProof&) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<PrefilterExecutionProof>);
static_assert(std::is_standard_layout_v<PrefilterExecutionProof>);

static_assert([] {
    PrefilterExecutionProof proof{};
    proof.record_batch(
        ResolvedPrefilterAlgorithm::sneaky_snake,
        PrefilterBackendType::SIMD, 4u, 2u, 1u, 1u, 1u, 1u, 2u);
    PrefilterExecutionProof tail{};
    tail.record_batch(
        ResolvedPrefilterAlgorithm::sneaky_snake,
        PrefilterBackendType::SIMD, 2u, 2u, 0u, 0u, 0u, 0u, 1u);
    proof.combine(tail);
    return proof.dispatches == 2u
        && proof.jobs == 6u
        && proof.evaluated_jobs == 4u
        && proof.accepted(PrefilterBackendType::SIMD);
}());
static_assert([] {
    PrefilterExecutionProof proof{};
    proof.record_batch(
        ResolvedPrefilterAlgorithm::gatekeeper,
        PrefilterBackendType::VULKAN,
        3u, 3u, 0u, 0u, 1u, 1u, 2u,
        1u, 1u, 1u, 0u, 3u);
    return proof.valid(/*require_evaluated_jobs=*/true,
                       /*forbid_unsupported_jobs=*/true)
        && proof.length_64_jobs == 3u
        && std::string_view{proof.resolved_algorithm_name()} == "gatekeeper"
        && std::string_view{proof.resolved_backend_name()} == "vulkan";
}());
static_assert([] {
    PrefilterExecutionProof proof{};
    proof.record_batch(
        ResolvedPrefilterAlgorithm::sneaky_snake,
        PrefilterBackendType::SCALAR,
        (std::numeric_limits<std::uint64_t>::max)(),
        (std::numeric_limits<std::uint64_t>::max)(),
        0u, 0u, 0u, 0u, 1u);
    proof.record_batch(
        ResolvedPrefilterAlgorithm::sneaky_snake,
        PrefilterBackendType::SCALAR, 1u, 1u, 0u, 0u, 0u, 0u, 1u);
    return proof.saturated
        && proof.jobs == (std::numeric_limits<std::uint64_t>::max)()
        && !proof.accepted(PrefilterBackendType::SCALAR);
}());
static_assert([] {
    PrefilterExecutionProof proof{};
    proof.record_batch(
        ResolvedPrefilterAlgorithm::gatekeeper,
        PrefilterBackendType::VULKAN,
        1u, 1u, 0u, 0u, 0u, 0u, 1u,
        0u, 0u, 0u, 0u, 1u);
    proof.combine(PrefilterExecutionProof{});
    return proof.saturated && !proof.valid();
}());

} // namespace bioseq::prefilter_stats

}

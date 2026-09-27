/**
 * @file prefilter_execution_contract.cppm
 * @brief Counts completed prefilter work and checks the reported totals.
 */
module;

#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

export module bioseq.prefilter.execution_contract;

export import bioseq.pipeline.runtime_contract;

export {

// Keep one statistics type outside Highway's per-target namespaces so
// PostgreSQL can combine results from every compiled SIMD target.
namespace bioseq::prefilter {

/// Algorithm that ran a completed prefilter batch.
enum class resolved_prefilter_algorithm : std::uint8_t {
    /// SneakySnake edit-distance filtering.
    sneaky_snake = 0u,
    /// GateKeeper masks, with exact checks before accepting mask-based rejections.
    gatekeeper = 1u,
};

/** Return the algorithm name used in reports, or "unknown". */
[[nodiscard]] constexpr const char* prefilter_algorithm_name(
    resolved_prefilter_algorithm algorithm) noexcept
{
    switch (algorithm) {
        case resolved_prefilter_algorithm::sneaky_snake: return "sneaky_snake";
        case resolved_prefilter_algorithm::gatekeeper: return "gatekeeper";
    }
    return "unknown";
}

/// Counts work completed by the prefilter stage.
///
/// Record a batch  after its backend returns all results and the stage decides
/// which regions survive. Reports can then show the algorithm that ran and
/// the regions it rejected, even when the request allowed autmatic selection.
struct prefilter_execution_stats {
    /// Completed prefilter batches.
    std::uint64_t dispatches = 0;
    /// Completed SneakySnake batches.
    std::uint64_t sneaky_snake_dispatches = 0;
    /// Completed GateKeeper batches.
    std::uint64_t gatekeeper_dispatches = 0;
    /// Batches completed on the scalar backend.
    std::uint64_t scalar_dispatches = 0;
    /// Batches completed by a Highway SIMD kernel.
    std::uint64_t simd_dispatches = 0;
    /// Batches completed on the Vulkan backend.
    std::uint64_t vulkan_dispatches = 0;
    /// Sequence-gap comparisons submitted in completed batches.
    std::uint64_t jobs = 0;
    /// Jobs for which the selected prefilter returned a supported result
    std::uint64_t evaluated_jobs = 0;
    /// Supported jobs with 64 symbols on each side, accepted or rejected.
    std::uint64_t length_64_jobs = 0;
    /// Unequal-length jobs checked using only the length-difference lower bound.
    std::uint64_t length_difference_jobs = 0;
    /// Jobs accepted  without evaluation because their input shape is unsupported.
    std::uint64_t unsupported_jobs = 0;
    /// Jobs rejected by the prefilter.
    std::uint64_t rejected_jobs = 0;
    /// Regions rejected by a gap result or by their combined lower bound.
    std::uint64_t rejected_regions = 0;
    /// Regions kept for alignment after combining their gap results.
    std::uint64_t surviving_regions = 0;
    /// Jobs whose GateKeeper mask suggests rejecting the sequence pair.
    std::uint64_t mask_rejection_candidates = 0;
    /// Exact edit-distance checks made, to confirm mask-based rejections.
    std::uint64_t exact_guard_activations = 0;
    /// Mask-based rejections confirmed by an exact edit-distance check.
    std::uint64_t guard_confirmed_rejections = 0;
    /// Zero-edit jobs rejected because the sequences differ at a position.
    std::uint64_t hamming_exact_rejections = 0;
    /// A counter overflowed or the reported counts failed a consistency check.
    bool is_saturated = false;

    /** Add to a counter, clamping overflow  and setting saturated_flag. */
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

    /** Add a completed batch and check its algorithm-specific counts. */
    constexpr void record_batch(
            const resolved_prefilter_algorithm algorithm,
            const prefilter_backend backend,
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
        add(dispatches, 1u, is_saturated);
        if (length_64_count > evaluated_count) {
            is_saturated = true;
        }
        switch (algorithm) {
            case resolved_prefilter_algorithm::sneaky_snake:
                add(sneaky_snake_dispatches, 1u, is_saturated);
                if (mask_rejection_candidate_count != 0u
                    || exact_guard_activation_count != 0u
                    || guard_confirmed_rejection_count != 0u
                    || hamming_exact_rejection_count != 0u) {
                    is_saturated = true;
                }
                break;
            case resolved_prefilter_algorithm::gatekeeper:
                add(gatekeeper_dispatches, 1u, is_saturated);
                if (guard_confirmed_rejection_count
                        > exact_guard_activation_count
                    || exact_guard_activation_count
                        > mask_rejection_candidate_count
                    || mask_rejection_candidate_count > evaluated_count
                    || guard_confirmed_rejection_count > rejected_job_count
                    || hamming_exact_rejection_count
                        != rejected_job_count
                            - guard_confirmed_rejection_count) {
                    is_saturated = true;
                }
                break;
        }
        switch (backend) {
            case prefilter_backend::scalar:
                add(scalar_dispatches, 1u, is_saturated);
                break;
            case prefilter_backend::simd:
                add(simd_dispatches, 1u, is_saturated);
                break;
            case prefilter_backend::vulkan:
                add(vulkan_dispatches, 1u, is_saturated);
                break;
            case prefilter_backend::automatic:
                // Resolve AUTO to the backend that ran before recording the batch.
                is_saturated = true;
                break;
        }
        add(jobs, job_count, is_saturated);
        add(evaluated_jobs, evaluated_count, is_saturated);
        add(length_64_jobs, length_64_count, is_saturated);
        add(length_difference_jobs, length_difference_count, is_saturated);
        add(unsupported_jobs, unsupported_count, is_saturated);
        add(rejected_jobs, rejected_job_count, is_saturated);
        add(rejected_regions, rejected_region_count, is_saturated);
        add(surviving_regions, surviving_region_count, is_saturated);
        add(mask_rejection_candidates,
            mask_rejection_candidate_count, is_saturated);
        add(exact_guard_activations,
            exact_guard_activation_count, is_saturated);
        add(guard_confirmed_rejections,
            guard_confirmed_rejection_count, is_saturated);
        add(hamming_exact_rejections,
            hamming_exact_rejection_count, is_saturated);
    }

    /** Add another batch or statement's counters and keep any invalid status. */
    constexpr void merge(
            const prefilter_execution_stats& other) noexcept {
        // An invalid input must leave the merged result invalid, even when all
        // its counters are ero. Otherwise a failed batch could disappear
        // inside an aggregate with valid work.
        bool overflow = is_saturated || !other.is_internally_consistent();
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
        is_saturated = overflow;
    }

    /// Check totals across completed batches. Several algorithms or backends
    /// may contribute to one statement Optional checks require evaluated jobs
    /// or  reject aggregates containing unsupported jobs.
    [[nodiscard]] constexpr bool is_internally_consistent(
            const bool require_evaluated_jobs = false,
            const bool forbid_unsupported_jobs = false) const noexcept {
        return !(is_saturated || dispatches == 0u || jobs == 0u
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

    /** Check the totals and require every batch to use the specified backend. */
    [[nodiscard]] constexpr bool is_backend_consistent(
            const prefilter_backend expected_backend,
            const bool require_evaluated_jobs = false,
            const bool forbid_unsupported_jobs = false) const noexcept {
        if (!is_internally_consistent(require_evaluated_jobs, forbid_unsupported_jobs)) {
            return false;
        }
        switch (expected_backend) {
            case prefilter_backend::scalar:
                return scalar_dispatches == dispatches
                    && simd_dispatches == 0u
                    && vulkan_dispatches == 0u;
            case prefilter_backend::simd:
                return scalar_dispatches == 0u
                    && simd_dispatches == dispatches
                    && vulkan_dispatches == 0u;
            case prefilter_backend::vulkan:
                return scalar_dispatches == 0u
                    && simd_dispatches == 0u
                    && vulkan_dispatches == dispatches;
            case prefilter_backend::automatic:
                return false;
        }
        return false;
    }

    /// Return the completed algorithm's name, "mixed", or "invalid".
    [[nodiscard]] constexpr const char* resolved_algorithm_name() const noexcept {
        if (!is_internally_consistent()) {
            return "invalid";
        }
        if (gatekeeper_dispatches == dispatches) {
            return "gatekeeper";
        }
        if (sneaky_snake_dispatches == dispatches) {
            return "sneaky_snake";
        }
        return "mixed";
    }

    /// Return the completed backend's name, "mixed", or "invalid".
    [[nodiscard]] constexpr const char* resolved_backend_name() const noexcept {
        if (!is_internally_consistent()) {
            return "invalid";
        }
        if (scalar_dispatches == dispatches) {
            return "scalar";
        }
        if (simd_dispatches == dispatches) {
            return "highway_simd";
        }
        if (vulkan_dispatches == dispatches) {
            return "vulkan";
        }
        return "mixed";
    }

    /// Compare every execution counter and validation flag.
    constexpr bool operator==(
        const prefilter_execution_stats&) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<prefilter_execution_stats>);
static_assert(std::is_standard_layout_v<prefilter_execution_stats>);

static_assert([] {
    prefilter_execution_stats stats{};
    stats.record_batch(
        resolved_prefilter_algorithm::sneaky_snake,
        prefilter_backend::simd, 4u, 2u, 1u, 1u, 1u, 1u, 2u);
    prefilter_execution_stats tail{};
    tail.record_batch(
        resolved_prefilter_algorithm::sneaky_snake,
        prefilter_backend::simd, 2u, 2u, 0u, 0u, 0u, 0u, 1u);
    stats.merge(tail);
    return stats.dispatches == 2u
        && stats.jobs == 6u
        && stats.evaluated_jobs == 4u
        && stats.is_backend_consistent(prefilter_backend::simd);
}());
static_assert([] {
    prefilter_execution_stats stats{};
    stats.record_batch(
        resolved_prefilter_algorithm::gatekeeper,
        prefilter_backend::vulkan,
        3u, 3u, 0u, 0u, 1u, 1u, 2u,
        1u, 1u, 1u, 0u, 3u);
    return stats.is_internally_consistent(/*require_evaluated_jobs=*/true,
                       /*forbid_unsupported_jobs=*/true)
        && stats.length_64_jobs == 3u
        && std::string_view{stats.resolved_algorithm_name()} == "gatekeeper"
        && std::string_view{stats.resolved_backend_name()} == "vulkan";
}());
static_assert([] {
    prefilter_execution_stats stats{};
    stats.record_batch(
        resolved_prefilter_algorithm::sneaky_snake,
        prefilter_backend::scalar,
        (std::numeric_limits<std::uint64_t>::max)(),
        (std::numeric_limits<std::uint64_t>::max)(),
        0u, 0u, 0u, 0u, 1u);
    stats.record_batch(
        resolved_prefilter_algorithm::sneaky_snake,
        prefilter_backend::scalar, 1u, 1u, 0u, 0u, 0u, 0u, 1u);
    return stats.is_saturated
        && stats.jobs == (std::numeric_limits<std::uint64_t>::max)()
        && !stats.is_backend_consistent(prefilter_backend::scalar);
}());
static_assert([] {
    prefilter_execution_stats stats{};
    stats.record_batch(
        resolved_prefilter_algorithm::gatekeeper,
        prefilter_backend::vulkan,
        1u, 1u, 0u, 0u, 0u, 0u, 1u,
        0u, 0u, 0u, 0u, 1u);
    stats.merge(prefilter_execution_stats{});
    return stats.is_saturated && !stats.is_internally_consistent();
}());

} // namespace bioseq::prefilter

}

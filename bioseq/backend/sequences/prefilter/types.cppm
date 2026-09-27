/**
 * @file types.cppm
 * @brief Shared CPU and Vulkan prefilter job and result layouts.
 *
 * Jobs select slices from rank-byte buffers. Results use integer fields so CPU
 * code and Vulkan shaders can exchange the same records.
 */
module;

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

export module bioseq.prefilter.types;

export namespace bioseq::prefilter {

/**
 * @brief Records how an edit-distance prefilter handled a job.
 *
 * Unsupported inputs remain candidates for exact alignment.
 */
enum class prefilter_status : std::uint32_t {
    success = 0u,                  ///< The selected prefilter evaluated the job.
    conservative_unequal = 1u,     ///< Only the length lower bound was evaluated.
    unsupported_conservative = 2u, ///< The job was accepted without evaluation.
};

/**
 * @brief Selects a query slice and target slice for one prefilter comparison.
 *
 * Offsets and lengths count symbols in unpacked rank-byte buffers.
 */
struct prefilter_job {
    std::uint32_t query_offset = 0u;  ///< First query-rank index.
    std::uint32_t target_offset = 0u; ///< First target-rank index.
    std::uint32_t query_length = 0u;  ///< Query slice length in symbols.
    std::uint32_t target_length = 0u; ///< Target slice length in symbols.
    std::uint32_t max_edits = 0u;     ///< Inclusive edit-distance threshold.
};

/**
 * @brief Stores one prefilter decision in a format shared with Vulkan.
 *
 * A nonzero is_accepted keeps the candidate for exact alignment. A passing
 * prefilter alone does not establish a match.
 */
struct prefilter_result {
    std::uint32_t lower_bound = 0u; ///< Conservative edit-distance lower bound.
    std::uint32_t status = static_cast<std::uint32_t>(
        prefilter_status::success); ///< Encoded @ref prefilter_status.
    std::uint32_t is_accepted = 1u; ///< Zero only for a proven rejection.
    std::uint32_t diagnostics = 0u; ///< GateKeeper estimate and decision flags.
};

/**
 * @brief Borrows rank buffers and the jobs to evaluate against them.
 *
 * Each job selects its query and target independently. Result order follows jobs.
 */
struct prefilter_batch_input {
    std::span<const std::uint8_t> query_ranks{};  ///< Concatenated query ranks.
    std::span<const std::uint8_t> target_ranks{}; ///< Concatenated target ranks.
    std::span<const prefilter_job> jobs{};         ///< Jobs in result order.
};

static_assert(std::is_standard_layout_v<prefilter_job>);
static_assert(std::is_trivially_copyable_v<prefilter_job>);
static_assert(sizeof(prefilter_job) == 5u * sizeof(std::uint32_t));
static_assert(alignof(prefilter_job) == alignof(std::uint32_t));
static_assert(offsetof(prefilter_job, query_offset) == 0u);
static_assert(offsetof(prefilter_job, target_offset) == 4u);
static_assert(offsetof(prefilter_job, query_length) == 8u);
static_assert(offsetof(prefilter_job, target_length) == 12u);
static_assert(offsetof(prefilter_job, max_edits) == 16u);

static_assert(std::is_standard_layout_v<prefilter_result>);
static_assert(std::is_trivially_copyable_v<prefilter_result>);
static_assert(sizeof(prefilter_result) == 4u * sizeof(std::uint32_t));
static_assert(alignof(prefilter_result) == alignof(std::uint32_t));
static_assert(offsetof(prefilter_result, lower_bound) == 0u);
static_assert(offsetof(prefilter_result, status) == 4u);
static_assert(offsetof(prefilter_result, is_accepted) == 8u);
static_assert(offsetof(prefilter_result, diagnostics) == 12u);

/** Low bits containing GateKeeper's capped mask estimate. */
inline constexpr std::uint32_t k_gatekeeper_estimate_mask = 0x0000ffffu;
/** Candidate rejection produced by GateKeeper's mask estimator. */
inline constexpr std::uint32_t k_gatekeeper_mask_rejection_candidate_mask =
    1u << 16u;
/** Exact edit-distance validation ran after a mask-based rejection candidate. */
inline constexpr std::uint32_t k_gatekeeper_exact_guard_activated_mask =
    1u << 17u;
/** An exact edit-distance check confirmed that the candidate exceeds the allowed edits. */
inline constexpr std::uint32_t k_gatekeeper_guard_confirmed_rejection_mask =
    1u << 18u;
/** A zero-threshold Hamming comparison proved the rejection. */
inline constexpr std::uint32_t k_gatekeeper_hamming_exact_rejection_mask =
    1u << 19u;
/** All GateKeeper decision flags stored in @ref prefilter_result::diagnostics. */
inline constexpr std::uint32_t k_gatekeeper_diagnostic_flags_mask =
    k_gatekeeper_mask_rejection_candidate_mask
    | k_gatekeeper_exact_guard_activated_mask
    | k_gatekeeper_guard_confirmed_rejection_mask
    | k_gatekeeper_hamming_exact_rejection_mask;
/** Every currently defined bit in @ref prefilter_result::diagnostics. */
inline constexpr std::uint32_t k_gatekeeper_diagnostic_valid_mask =
    k_gatekeeper_estimate_mask | k_gatekeeper_diagnostic_flags_mask;

/**
 * @brief Builds a prefilter result with integer status and decision fields.
 * @param lower_bound Safe edit-distance lower bound.
 * @param status How the job was evaluated.
 * @param is_accepted Keep the candidate for exact alignment.
 * @param diagnostics Algorithm-specific diagnostic bits.
 * @return Result record.
 */
[[nodiscard]] constexpr prefilter_result make_prefilter_result(
    std::uint32_t lower_bound,
    prefilter_status status,
    bool is_accepted,
    std::uint32_t diagnostics = 0u) noexcept
{
    return prefilter_result{
        .lower_bound = lower_bound,
        .status = static_cast<std::uint32_t>(status),
        .is_accepted = is_accepted ? 1u : 0u,
        .diagnostics = diagnostics,
    };
}

/**
 * @brief Keeps an unsupported input as a candidate without evaluating it.
 * @return Accepted result with a zero lower bound and unsupported status.
 */
[[nodiscard]] constexpr prefilter_result
unsupported_prefilter_result() noexcept
{
    return make_prefilter_result(
        0u, prefilter_status::unsupported_conservative, true);
}

/**
 * @brief Checks whether the selected prefilter algorithm evaluated the job.
 * @return True only for success status.
 */
[[nodiscard]] constexpr bool is_prefilter_evaluated(
    const prefilter_result& result) noexcept
{
    return result.status == static_cast<std::uint32_t>(
        prefilter_status::success);
}

/**
 * @brief Reports whether the candidate should proceed to exact alignment.
 * @return True when is_accepted is nonzero.
 */
[[nodiscard]] constexpr bool is_prefilter_accepted(
    const prefilter_result& result) noexcept
{
    return result.is_accepted != 0u;
}

/**
 * @brief Checks that an offset and length fit within a rank buffer.
 * @param rank_count Buffer length in symbols.
 * @param offset First symbol.
 * @param length Number of symbols.
 * @return True when the complete slice is in bounds. The check avoids overflow.
 */
[[nodiscard]] constexpr bool prefilter_slice_in_bounds(
    std::size_t rank_count,
    std::uint32_t offset,
    std::uint32_t length) noexcept
{
    const std::size_t offset_size = static_cast<std::size_t>(offset);
    const std::size_t length_size = static_cast<std::size_t>(length);
    return offset_size <= rank_count
        && length_size <= rank_count - offset_size;
}

/**
 * @brief Checks both rank-buffer slices referenced by a job.
 * @param input Query and target buffers.
 * @param job Requested offsets and lengths.
 * @return True when both slices are in bounds.
 */
[[nodiscard]] constexpr bool prefilter_job_in_bounds(
    const prefilter_batch_input& input,
    const prefilter_job& job) noexcept
{
    return prefilter_slice_in_bounds(
               input.query_ranks.size(), job.query_offset, job.query_length)
        && prefilter_slice_in_bounds(
               input.target_ranks.size(), job.target_offset, job.target_length);
}

/** Returns the GateKeeper estimate stored in the low diagnostic bits. */
[[nodiscard]] constexpr std::uint32_t gatekeeper_diagnostic_estimate(
    const prefilter_result& result) noexcept
{
    return result.diagnostics & k_gatekeeper_estimate_mask;
}

/** Reports whether the GateKeeper mask score proposed rejection. */
[[nodiscard]] constexpr bool is_gatekeeper_mask_rejection_candidate(
    const prefilter_result& result) noexcept
{
    return (result.diagnostics
            & k_gatekeeper_mask_rejection_candidate_mask) != 0u;
}

/** Reports whether an exact edit-distance check followed the mask estimate. */
[[nodiscard]] constexpr bool is_gatekeeper_exact_guard_activated(
    const prefilter_result& result) noexcept
{
    return (result.diagnostics
            & k_gatekeeper_exact_guard_activated_mask) != 0u;
}

/** Reports whether the exact edit-distance check confirmed rejection. */
[[nodiscard]] constexpr bool is_gatekeeper_guard_confirmed_rejection(
    const prefilter_result& result) noexcept
{
    return (result.diagnostics
            & k_gatekeeper_guard_confirmed_rejection_mask) != 0u;
}

/** Reports whether a mismatch rejected a job allowing zero edits. */
[[nodiscard]] constexpr bool is_gatekeeper_hamming_exact_rejection(
    const prefilter_result& result) noexcept
{
    return (result.diagnostics
            & k_gatekeeper_hamming_exact_rejection_mask) != 0u;
}

} // namespace bioseq::prefilter

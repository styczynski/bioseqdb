/**
 * @file types.cppm
 * @brief Canonical rank-prefilter host/device batch ABI and neutral helpers.
 *
 * Jobs address flat alphabet-rank buffers in logical symbols. Results retain
 * integral status and decision fields so their byte layout is shared unchanged
 * by scalar code and Vulkan storage buffers. The helpers below construct,
 * validate, and decode that representation without selecting a backend.
 */
module;

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

export module bioseq.prefilter.types;

export namespace bioseq::prefilter {

/**
 * @brief Outcome class for a conservative edit-distance prefilter.
 *
 * Unsupported inputs remain accepted so a caller can continue with the exact
 * aligner. Prefilters never turn an unsupported geometry into a rejection.
 */
enum class prefilter_status : std::uint32_t {
    success = 0u,                  ///< The selected prefilter evaluated the job.
    conservative_unequal = 1u,     ///< Only the length lower bound was evaluated.
    unsupported_conservative = 2u, ///< The job was accepted without evaluation.
};

/**
 * @brief One offset-and-length job over a pair of flat rank buffers.
 *
 * Offsets and lengths are measured in encoded symbols, not bytes of an
 * alphabet-specific packed representation.
 */
struct prefilter_job {
    std::uint32_t query_offset = 0u;  ///< First query-rank index.
    std::uint32_t target_offset = 0u; ///< First target-rank index.
    std::uint32_t query_length = 0u;  ///< Query slice length in symbols.
    std::uint32_t target_length = 0u; ///< Target slice length in symbols.
    std::uint32_t max_edits = 0u;     ///< Inclusive edit-distance threshold.
};

/**
 * @brief Stable host/device result record for one prefilter job.
 *
 * The integral decision fields keep this record trivially copyable for Vulkan
 * storage buffers. A nonzero @ref is_accepted value means the exact aligner must
 * retain the candidate.
 */
struct prefilter_result {
    std::uint32_t lower_bound = 0u; ///< Conservative edit-distance lower bound.
    std::uint32_t status = static_cast<std::uint32_t>(
        prefilter_status::success); ///< Encoded @ref prefilter_status.
    std::uint32_t is_accepted = 1u; ///< Zero only for a proven rejection.
    std::uint32_t diagnostics = 0u; ///< GateKeeper estimate and decision flags.
};

/**
 * @brief Non-owning flat buffers and ordered jobs for one batch evaluation.
 *
 * Each job indexes the two rank spans independently. Results are emitted in
 * the same order as @ref jobs.
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
/** The exact guard confirmed the candidate rejection. */
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
 * @brief Construct the stable integral result representation.
 * @param lower_bound Conservative lower bound reported by the algorithm.
 * @param status Evaluation outcome.
 * @param is_accepted Whether the candidate must continue to exact alignment.
 * @param diagnostics Algorithm-specific diagnostic bits.
 * @return Trivially copyable result with the enum and decision encoded as integers.
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
 * @brief Construct the fail-open result for an unsupported geometry.
 * @return An unevaluated result which retains the candidate for exact alignment.
 */
[[nodiscard]] constexpr prefilter_result
unsupported_prefilter_result() noexcept
{
    return make_prefilter_result(
        0u, prefilter_status::unsupported_conservative, true);
}

/**
 * @brief Report whether a prefilter algorithm evaluated the job.
 * @param result Integral result record to inspect.
 * @return `true` only for @ref prefilter_status::success.
 */
[[nodiscard]] constexpr bool is_prefilter_evaluated(
    const prefilter_result& result) noexcept
{
    return result.status == static_cast<std::uint32_t>(
        prefilter_status::success);
}

/**
 * @brief Decode the conservative keep-or-reject decision.
 * @param result Integral result record to inspect.
 * @return `true` when exact alignment must retain the candidate.
 */
[[nodiscard]] constexpr bool is_prefilter_accepted(
    const prefilter_result& result) noexcept
{
    return result.is_accepted != 0u;
}

/**
 * @brief Validate one unsigned offset-length pair without overflow.
 * @param rank_count Number of symbols in the addressed flat rank buffer.
 * @param offset First symbol in the requested slice.
 * @param length Number of symbols in the requested slice.
 * @return `true` when the complete half-open slice lies within the buffer.
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
 * @brief Validate both rank slices referenced by a batch job.
 * @param input Flat query and target buffers containing the job's slices.
 * @param job Offset-and-length record to validate.
 * @return `true` when both half-open slices lie within their respective buffers.
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

/**
 * @brief Decode GateKeeper's capped mask estimate.
 * @param result Result whose diagnostic word contains the estimate.
 * @return Low diagnostic bits interpreted as the capped estimate.
 */
[[nodiscard]] constexpr std::uint32_t gatekeeper_diagnostic_estimate(
    const prefilter_result& result) noexcept
{
    return result.diagnostics & k_gatekeeper_estimate_mask;
}

/**
 * @brief Report whether the mask estimator proposed a rejection.
 * @param result Result whose diagnostic flags are inspected.
 * @return `true` when the rejection-candidate flag is set.
 */
[[nodiscard]] constexpr bool is_gatekeeper_mask_rejection_candidate(
    const prefilter_result& result) noexcept
{
    return (result.diagnostics
            & k_gatekeeper_mask_rejection_candidate_mask) != 0u;
}

/**
 * @brief Report whether exact edit-distance validation ran.
 * @param result Result whose diagnostic flags are inspected.
 * @return `true` when the exact-guard-activated flag is set.
 */
[[nodiscard]] constexpr bool is_gatekeeper_exact_guard_activated(
    const prefilter_result& result) noexcept
{
    return (result.diagnostics
            & k_gatekeeper_exact_guard_activated_mask) != 0u;
}

/**
 * @brief Report whether exact validation confirmed a rejection.
 * @param result Result whose diagnostic flags are inspected.
 * @return `true` when the guard-confirmed-rejection flag is set.
 */
[[nodiscard]] constexpr bool is_gatekeeper_guard_confirmed_rejection(
    const prefilter_result& result) noexcept
{
    return (result.diagnostics
            & k_gatekeeper_guard_confirmed_rejection_mask) != 0u;
}

/**
 * @brief Report whether a zero-threshold Hamming comparison rejected the job.
 * @param result Result whose diagnostic flags are inspected.
 * @return `true` when the Hamming-rejection flag is set.
 */
[[nodiscard]] constexpr bool is_gatekeeper_hamming_exact_rejection(
    const prefilter_result& result) noexcept
{
    return (result.diagnostics
            & k_gatekeeper_hamming_exact_rejection_mask) != 0u;
}

} // namespace bioseq::prefilter

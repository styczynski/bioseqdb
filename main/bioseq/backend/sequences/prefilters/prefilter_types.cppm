/**
 * @file prefilter_types.cppm
 * @brief Canonical rank-prefilter host/device batch ABI and neutral helpers.
 */
module;

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

export module bioseq.sequences.prefilters.prefilter_types;

export namespace bioseq::sequences::prefilters {

enum class PrefilterStatus : std::uint32_t {
    success = 0u,
    conservative_unequal = 1u,
    unsupported_conservative = 2u,
};

struct PrefilterJob {
    std::uint32_t query_offset = 0u;
    std::uint32_t target_offset = 0u;
    std::uint32_t query_length = 0u;
    std::uint32_t target_length = 0u;
    std::uint32_t max_edits = 0u;
};

struct PrefilterResult {
    std::uint32_t lower_bound = 0u;
    std::uint32_t status = static_cast<std::uint32_t>(
        PrefilterStatus::success);
    std::uint32_t accepted = 1u;
    std::uint32_t reserved = 0u;
};

struct PrefilterBatchInput {
    std::span<const std::uint8_t> query_ranks{};
    std::span<const std::uint8_t> target_ranks{};
    std::span<const PrefilterJob> jobs{};
};

static_assert(std::is_standard_layout_v<PrefilterJob>);
static_assert(std::is_trivially_copyable_v<PrefilterJob>);
static_assert(sizeof(PrefilterJob) == 5u * sizeof(std::uint32_t));
static_assert(alignof(PrefilterJob) == alignof(std::uint32_t));
static_assert(offsetof(PrefilterJob, query_offset) == 0u);
static_assert(offsetof(PrefilterJob, target_offset) == 4u);
static_assert(offsetof(PrefilterJob, query_length) == 8u);
static_assert(offsetof(PrefilterJob, target_length) == 12u);
static_assert(offsetof(PrefilterJob, max_edits) == 16u);

static_assert(std::is_standard_layout_v<PrefilterResult>);
static_assert(std::is_trivially_copyable_v<PrefilterResult>);
static_assert(sizeof(PrefilterResult) == 4u * sizeof(std::uint32_t));
static_assert(alignof(PrefilterResult) == alignof(std::uint32_t));
static_assert(offsetof(PrefilterResult, lower_bound) == 0u);
static_assert(offsetof(PrefilterResult, status) == 4u);
static_assert(offsetof(PrefilterResult, accepted) == 8u);
static_assert(offsetof(PrefilterResult, reserved) == 12u);

inline constexpr std::uint32_t gatekeeper_estimate_mask = 0x0000ffffu;
inline constexpr std::uint32_t gatekeeper_mask_rejection_candidate_mask =
    1u << 16u;
inline constexpr std::uint32_t gatekeeper_exact_guard_activated_mask =
    1u << 17u;
inline constexpr std::uint32_t gatekeeper_guard_confirmed_rejection_mask =
    1u << 18u;
inline constexpr std::uint32_t gatekeeper_hamming_exact_rejection_mask =
    1u << 19u;
inline constexpr std::uint32_t gatekeeper_diagnostic_flags_mask =
    gatekeeper_mask_rejection_candidate_mask
    | gatekeeper_exact_guard_activated_mask
    | gatekeeper_guard_confirmed_rejection_mask
    | gatekeeper_hamming_exact_rejection_mask;
inline constexpr std::uint32_t gatekeeper_diagnostic_valid_mask =
    gatekeeper_estimate_mask | gatekeeper_diagnostic_flags_mask;

[[nodiscard]] constexpr PrefilterResult make_prefilter_result(
    std::uint32_t lower_bound,
    PrefilterStatus status,
    bool accepted,
    std::uint32_t reserved = 0u) noexcept
{
    return PrefilterResult{
        .lower_bound = lower_bound,
        .status = static_cast<std::uint32_t>(status),
        .accepted = accepted ? 1u : 0u,
        .reserved = reserved,
    };
}

[[nodiscard]] constexpr bool prefilter_evaluated(
    const PrefilterResult& result) noexcept
{
    return result.status == static_cast<std::uint32_t>(
        PrefilterStatus::success);
}

[[nodiscard]] constexpr bool prefilter_accepted(
    const PrefilterResult& result) noexcept
{
    return result.accepted != 0u;
}

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

[[nodiscard]] constexpr bool prefilter_job_in_bounds(
    const PrefilterBatchInput& input,
    const PrefilterJob& job) noexcept
{
    return prefilter_slice_in_bounds(
               input.query_ranks.size(), job.query_offset, job.query_length)
        && prefilter_slice_in_bounds(
               input.target_ranks.size(), job.target_offset, job.target_length);
}

[[nodiscard]] constexpr std::uint32_t gatekeeper_diagnostic_estimate(
    const PrefilterResult& result) noexcept
{
    return result.reserved & gatekeeper_estimate_mask;
}

[[nodiscard]] constexpr bool gatekeeper_mask_rejection_candidate(
    const PrefilterResult& result) noexcept
{
    return (result.reserved & gatekeeper_mask_rejection_candidate_mask) != 0u;
}

[[nodiscard]] constexpr bool gatekeeper_exact_guard_activated(
    const PrefilterResult& result) noexcept
{
    return (result.reserved & gatekeeper_exact_guard_activated_mask) != 0u;
}

[[nodiscard]] constexpr bool gatekeeper_guard_confirmed_rejection(
    const PrefilterResult& result) noexcept
{
    return (result.reserved & gatekeeper_guard_confirmed_rejection_mask) != 0u;
}

[[nodiscard]] constexpr bool gatekeeper_hamming_exact_rejection(
    const PrefilterResult& result) noexcept
{
    return (result.reserved & gatekeeper_hamming_exact_rejection_mask) != 0u;
}

} // namespace bioseq::sequences::prefilters

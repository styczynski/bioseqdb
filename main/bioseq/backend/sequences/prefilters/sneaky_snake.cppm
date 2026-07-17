/**
 * @file sneaky_snake.cppm
 * @brief Alphabet-generic SneakySnake edit-distance lower-bound prefilter.
 *
 * The primitive consumes already-normalized uint8 rank buffers.  Equal-length
 * jobs run the source SneakySnake maze walk; unequal-length jobs use only the
 * always-safe length-difference lower bound.  The batch ABI intentionally
 * matches the Vulkan job/result layout.
 */
module;

#include <hwy/highway.h>

#include "common.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

export module bioseq.sequences.prefilters.sneaky_snake;

export {

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

/// Production maze geometry shared by the CPU and Vulkan implementations.
inline constexpr std::uint32_t sneaky_snake_default_submaze_width = 512u;
inline constexpr std::uint32_t sneaky_snake_default_max_escape_segments = 512u;

/// Stable wire ordinals shared with the Vulkan result buffer.
enum class SneakySnakeStatus : std::uint32_t {
    success = 0u,
    conservative_unequal = 1u,
    unsupported_conservative = 2u,
};

/// Stable CPU/GPU job ABI. Offsets address the corresponding flat rank buffer.
struct SneakySnakeJob {
    std::uint32_t query_offset = 0u;
    std::uint32_t target_offset = 0u;
    std::uint32_t query_length = 0u;
    std::uint32_t target_length = 0u;
    std::uint32_t max_edits = 0u;
};

/// Stable CPU/GPU result ABI. accepted is encoded as 0 or 1.
struct SneakySnakeResult {
    std::uint32_t lower_bound = 0u;
    std::uint32_t status = static_cast<std::uint32_t>(
        SneakySnakeStatus::success);
    std::uint32_t accepted = 1u;
    std::uint32_t reserved = 0u;
};

static_assert(std::is_standard_layout_v<SneakySnakeJob>);
static_assert(std::is_trivially_copyable_v<SneakySnakeJob>);
static_assert(sizeof(SneakySnakeJob) == 5u * sizeof(std::uint32_t));
static_assert(offsetof(SneakySnakeJob, query_offset) == 0u);
static_assert(offsetof(SneakySnakeJob, target_offset) == 4u);
static_assert(offsetof(SneakySnakeJob, query_length) == 8u);
static_assert(offsetof(SneakySnakeJob, target_length) == 12u);
static_assert(offsetof(SneakySnakeJob, max_edits) == 16u);
static_assert(std::is_standard_layout_v<SneakySnakeResult>);
static_assert(std::is_trivially_copyable_v<SneakySnakeResult>);
static_assert(sizeof(SneakySnakeResult) == 4u * sizeof(std::uint32_t));
static_assert(offsetof(SneakySnakeResult, lower_bound) == 0u);
static_assert(offsetof(SneakySnakeResult, status) == 4u);
static_assert(offsetof(SneakySnakeResult, accepted) == 8u);
static_assert(offsetof(SneakySnakeResult, reserved) == 12u);

struct SneakySnakeOptions {
    /// A zero value selects sneaky_snake_default_submaze_width.
    std::uint32_t submaze_width = sneaky_snake_default_submaze_width;
    /// Zero keeps y equal to the effective submaze width (512 by default).
    std::uint32_t max_escape_segments = 0u;
};

struct SneakySnakeBatchInput {
    std::span<const std::uint8_t> query_ranks{};
    std::span<const std::uint8_t> target_ranks{};
    std::span<const SneakySnakeJob> jobs{};
};

[[nodiscard]] constexpr bool sneaky_snake_evaluated(
    const SneakySnakeResult& result) noexcept
{
    return result.status
        == static_cast<std::uint32_t>(SneakySnakeStatus::success);
}

[[nodiscard]] constexpr bool sneaky_snake_accepted(
    const SneakySnakeResult& result) noexcept
{
    return result.accepted != 0u;
}

namespace sneaky_snake_detail {

[[nodiscard]] constexpr SneakySnakeResult make_result(
    std::uint32_t lower_bound,
    SneakySnakeStatus status,
    bool accepted) noexcept
{
    return SneakySnakeResult{
        .lower_bound = lower_bound,
        .status = static_cast<std::uint32_t>(status),
        .accepted = accepted ? 1u : 0u,
        .reserved = 0u,
    };
}

[[nodiscard]] constexpr std::uint32_t effective_width(
    SneakySnakeOptions options) noexcept
{
    return options.submaze_width == 0u
        ? sneaky_snake_default_submaze_width
        : options.submaze_width;
}

[[nodiscard]] constexpr std::uint32_t effective_escape_limit(
    SneakySnakeOptions options,
    std::uint32_t width) noexcept
{
    return options.max_escape_segments == 0u
        ? width
        : options.max_escape_segments;
}

[[nodiscard]] inline bool slice_is_in_bounds(
    std::span<const std::uint8_t> ranks,
    std::uint32_t offset,
    std::uint32_t length) noexcept
{
    const std::size_t offset_size = static_cast<std::size_t>(offset);
    const std::size_t length_size = static_cast<std::size_t>(length);
    return offset_size <= ranks.size()
        && length_size <= ranks.size() - offset_size;
}

[[nodiscard]] inline std::uint32_t diagonal_run_scalar(
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target,
    std::uint32_t position,
    std::uint32_t submaze_end,
    std::uint32_t query_index) noexcept
{
    const std::uint32_t target_remaining = submaze_end - position;
    const std::uint32_t query_remaining = static_cast<std::uint32_t>(
        query.size() - static_cast<std::size_t>(query_index));
    const std::uint32_t run_limit = std::min(
        target_remaining,
        query_remaining);

    const std::uint8_t* const query_data = query.data() + query_index;
    const std::uint8_t* const target_data = target.data() + position;
    if (run_limit == 0u || target_data[0] != query_data[0]) {
        return 0u;
    }

    std::uint32_t run = 1u;
    constexpr std::uint32_t word_bytes = sizeof(std::uint64_t);
    while (run_limit - run >= 2u * word_bytes) {
        std::uint64_t query_word = 0u;
        std::uint64_t target_word = 0u;
        std::memcpy(&query_word, query_data + run, word_bytes);
        std::memcpy(&target_word, target_data + run, word_bytes);
        const std::uint64_t different = query_word ^ target_word;
        if (different != 0u) {
            if constexpr (std::endian::native == std::endian::little) {
                return run + static_cast<std::uint32_t>(
                    std::countr_zero(different) / 8u);
            } else if constexpr (std::endian::native == std::endian::big) {
                return run + static_cast<std::uint32_t>(
                    std::countl_zero(different) / 8u);
            } else {
                break;
            }
        }

        std::memcpy(&query_word, query_data + run + word_bytes, word_bytes);
        std::memcpy(&target_word, target_data + run + word_bytes, word_bytes);
        const std::uint64_t second_different = query_word ^ target_word;
        if (second_different != 0u) {
            if constexpr (std::endian::native == std::endian::little) {
                return run + word_bytes + static_cast<std::uint32_t>(
                    std::countr_zero(second_different) / 8u);
            } else if constexpr (std::endian::native == std::endian::big) {
                return run + word_bytes + static_cast<std::uint32_t>(
                    std::countl_zero(second_different) / 8u);
            } else {
                break;
            }
        }
        run += 2u * word_bytes;
    }
    while (run_limit - run >= word_bytes) {
        std::uint64_t query_word = 0u;
        std::uint64_t target_word = 0u;
        std::memcpy(&query_word, query_data + run, word_bytes);
        std::memcpy(&target_word, target_data + run, word_bytes);
        const std::uint64_t different = query_word ^ target_word;
        if (different != 0u) {
            if constexpr (std::endian::native == std::endian::little) {
                return run + static_cast<std::uint32_t>(
                    std::countr_zero(different) / 8u);
            } else if constexpr (std::endian::native == std::endian::big) {
                return run + static_cast<std::uint32_t>(
                    std::countl_zero(different) / 8u);
            } else {
                break;
            }
        }
        run += word_bytes;
    }
    while (run < run_limit
           && target_data[run] == query_data[run]) {
        ++run;
    }
    return run;
}

template <class D>
[[nodiscard]] inline std::uint32_t diagonal_run_highway(
    D d,
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target,
    std::uint32_t position,
    std::uint32_t submaze_end,
    std::uint32_t query_index) noexcept
{
    const hn::Repartition<std::uint8_t, D> byte_d;
    const std::uint32_t target_remaining = submaze_end - position;
    const std::uint32_t query_remaining = static_cast<std::uint32_t>(
        query.size() - static_cast<std::size_t>(query_index));
    const std::uint32_t run_limit = std::min(
        target_remaining,
        query_remaining);

    const std::uint8_t* const query_data = query.data() + query_index;
    const std::uint8_t* const target_data = target.data() + position;
    if (run_limit == 0u || target_data[0] != query_data[0]) {
        return 0u;
    }

    const std::uint32_t lanes = static_cast<std::uint32_t>(
        hn::Lanes(byte_d));
    std::uint32_t run = 1u;
    while (run_limit - run >= 2u * lanes) {
        const auto query_0 = hn::LoadU(byte_d, query_data + run);
        const auto target_0 = hn::LoadU(byte_d, target_data + run);
        const auto query_1 = hn::LoadU(byte_d, query_data + run + lanes);
        const auto target_1 = hn::LoadU(byte_d, target_data + run + lanes);
        const auto mismatch_0 = hn::Ne(query_0, target_0);
        const auto mismatch_1 = hn::Ne(query_1, target_1);
        if (!hn::AllFalse(byte_d, mismatch_0)) {
            return run + static_cast<std::uint32_t>(
                hn::FindFirstTrue(byte_d, mismatch_0));
        }
        if (!hn::AllFalse(byte_d, mismatch_1)) {
            return run + lanes + static_cast<std::uint32_t>(
                hn::FindFirstTrue(byte_d, mismatch_1));
        }
        run += 2u * lanes;
    }
    while (run_limit - run >= lanes) {
        const auto mismatch = hn::Ne(
            hn::LoadU(byte_d, query_data + run),
            hn::LoadU(byte_d, target_data + run));
        if (!hn::AllFalse(byte_d, mismatch)) {
            return run + static_cast<std::uint32_t>(
                hn::FindFirstTrue(byte_d, mismatch));
        }
        run += lanes;
    }
    while (run < run_limit
           && target_data[run] == query_data[run]) {
        ++run;
    }
    return run;
}

enum class CpuTraversal : std::uint8_t {
    scalar,
    highway,
};

template <CpuTraversal traversal, class D>
[[nodiscard]] inline std::uint32_t diagonal_run_for(
    D d,
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target,
    std::uint32_t position,
    std::uint32_t submaze_end,
    std::uint32_t query_index) noexcept
{
    if constexpr (traversal == CpuTraversal::scalar) {
        static_cast<void>(d);
        return diagonal_run_scalar(
            query, target, position, submaze_end, query_index);
    } else {
        return diagonal_run_highway(
            d, query, target, position, submaze_end, query_index);
    }
}

template <CpuTraversal traversal, class D>
[[nodiscard]] inline bool improve_diagonal_candidate(
    D d,
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target,
    std::uint32_t position,
    std::uint32_t submaze_end,
    std::uint32_t query_start,
    std::uint32_t& best) noexcept
{
    const std::uint32_t remaining = submaze_end - position;
    const std::uint32_t length = static_cast<std::uint32_t>(query.size());
    const std::uint32_t run_limit = std::min(
        remaining,
        length - query_start);
    if (run_limit <= best) return false;

    best = std::max(best, diagonal_run_for<traversal>(
        d, query, target, position, submaze_end, query_start));
    return best == remaining;
}

template <CpuTraversal traversal, class D>
[[nodiscard]] inline std::uint32_t longest_diagonal_run_for(
    D d,
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target,
    std::uint32_t position,
    std::uint32_t submaze_end,
    std::uint32_t max_edits) noexcept
{
    const std::uint32_t remaining = submaze_end - position;
    std::uint32_t best = diagonal_run_for<traversal>(
        d, query, target, position, submaze_end, position);
    if (best == remaining) {
        return best;
    }

    const std::uint32_t length = static_cast<std::uint32_t>(query.size());
    // Freeze the current best as an exact candidate filter. Any diagonal that
    // can improve the maximum must match at this offset. Scanning the clipped
    // start window contiguously is therefore order-independent and exact.
    const std::uint32_t filter_offset = best;
    const std::uint8_t filter_rank =
        target[static_cast<std::size_t>(position) + filter_offset];
    const std::uint32_t left = std::min(max_edits, position);
    const std::uint32_t right = std::min(
        max_edits,
        length - position - 1u);
    const std::uint32_t lower = position - left;
    const std::uint32_t upper = std::min(
        position + right,
        length - filter_offset - 1u);
    std::uint32_t candidate = lower;

    if constexpr (traversal == CpuTraversal::highway) {
        using ByteD = hn::Repartition<std::uint8_t, D>;
        const ByteD byte_d;
        static_assert(hn::MaxLanes(ByteD{}) <= 64u);
        const std::uint32_t lanes = static_cast<std::uint32_t>(
            hn::Lanes(byte_d));
        const auto wanted = hn::Set(byte_d, filter_rank);
        while (candidate <= upper
               && upper - candidate + 1u >= lanes) {
            std::uint64_t matches = hn::BitsFromMask(
                byte_d,
                hn::Eq(
                    hn::LoadU(
                        byte_d,
                        query.data() + candidate + filter_offset),
                    wanted));
            while (matches != 0u) {
                const std::uint32_t lane = static_cast<std::uint32_t>(
                    std::countr_zero(matches));
                const std::uint32_t query_start = candidate + lane;
                if (query_start != position
                    && improve_diagonal_candidate<traversal>(
                        d,
                        query,
                        target,
                        position,
                        submaze_end,
                        query_start,
                        best)) {
                    return remaining;
                }
                matches &= matches - 1u;
            }
            candidate += lanes;
        }
    }

    for (; candidate <= upper; ++candidate) {
        if (candidate != position
            && query[static_cast<std::size_t>(candidate) + filter_offset]
                == filter_rank
            && improve_diagonal_candidate<traversal>(
                d,
                query,
                target,
                position,
                submaze_end,
                candidate,
                best)) {
            return remaining;
        }
    }
    return best;
}

template <CpuTraversal traversal, class D>
[[nodiscard]] inline SneakySnakeResult equal_length_for(
    D d,
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target,
    std::uint32_t max_edits,
    SneakySnakeOptions options) noexcept
{
    const std::uint32_t length = static_cast<std::uint32_t>(query.size());
    if (length == 0u) {
        return make_result(0u, SneakySnakeStatus::success, true);
    }

    const std::uint32_t width = effective_width(options);
    const std::uint32_t escape_limit = effective_escape_limit(options, width);
    std::uint32_t lower_bound = 0u;

    for (std::uint64_t start_wide = 0u; start_wide < length;
         start_wide += width) {
        const std::uint32_t start = static_cast<std::uint32_t>(start_wide);
        const std::uint32_t end = static_cast<std::uint32_t>(std::min(
            static_cast<std::uint64_t>(length),
            start_wide + width));
        std::uint32_t position = start;
        std::uint32_t escapes = 0u;

        while (position < end && escapes < escape_limit) {
            const std::uint32_t best = longest_diagonal_run_for<traversal>(
                d, query, target, position, end, max_edits);
            if (best == end - position) {
                position = end;
                break;
            }

            ++lower_bound;
            if (lower_bound > max_edits) {
                return make_result(
                    lower_bound,
                    SneakySnakeStatus::success,
                    false);
            }
            position += best + 1u;
            ++escapes;
        }
    }

    return make_result(
        lower_bound,
        SneakySnakeStatus::success,
        lower_bound <= max_edits);
}

/// Evaluate one already-size-checked rank pair without repeating span limits.
template <CpuTraversal traversal, class D>
[[nodiscard]] inline SneakySnakeResult bounded_for(
    D d,
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target,
    std::uint32_t max_edits,
    SneakySnakeOptions options) noexcept
{
    const std::uint32_t query_length = static_cast<std::uint32_t>(query.size());
    const std::uint32_t target_length = static_cast<std::uint32_t>(target.size());
    if (query_length != target_length) {
        const std::uint32_t difference = query_length > target_length
            ? query_length - target_length
            : target_length - query_length;
        return make_result(
            difference,
            SneakySnakeStatus::conservative_unequal,
            difference <= max_edits);
    }

    return equal_length_for<traversal>(
        d, query, target, max_edits, options);
}

/// Scalar traversal wrapper used by both public scalar entry points.
[[nodiscard]] inline SneakySnakeResult bounded_scalar(
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target,
    std::uint32_t max_edits,
    SneakySnakeOptions options) noexcept
{
    return bounded_for<CpuTraversal::scalar>(
        nullptr, query, target, max_edits, options);
}

} // namespace sneaky_snake_detail

[[nodiscard]] inline bool sneaky_snake_job_in_bounds(
    const SneakySnakeBatchInput& input,
    const SneakySnakeJob& job) noexcept
{
    return sneaky_snake_detail::slice_is_in_bounds(
               input.query_ranks, job.query_offset, job.query_length)
        && sneaky_snake_detail::slice_is_in_bounds(
               input.target_ranks, job.target_offset, job.target_length);
}

/// Scalar single-job entry point over rank spans.
[[nodiscard]] inline SneakySnakeResult sneaky_snake_scalar(
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target,
    std::uint32_t max_edits,
    SneakySnakeOptions options = {}) noexcept
{
    constexpr std::size_t max_u32 = (std::numeric_limits<std::uint32_t>::max)();
    if (query.size() > max_u32 || target.size() > max_u32) {
        return sneaky_snake_detail::make_result(
            0u, SneakySnakeStatus::unsupported_conservative, true);
    }

    return sneaky_snake_detail::bounded_scalar(
        query, target, max_edits, options);
}

/// Scalar reference batch implementation over flat uint8 rank buffers.
[[nodiscard]] inline std::vector<SneakySnakeResult> sneaky_snake_scalar_batch(
    const SneakySnakeBatchInput& input,
    SneakySnakeOptions options = {})
{
    std::vector<SneakySnakeResult> results(input.jobs.size());
    for (std::size_t index = 0u; index < input.jobs.size(); ++index) {
        const SneakySnakeJob& job = input.jobs[index];
        if (!sneaky_snake_job_in_bounds(input, job)) {
            results[index] = sneaky_snake_detail::make_result(
                0u, SneakySnakeStatus::unsupported_conservative, true);
            continue;
        }
        results[index] = sneaky_snake_detail::bounded_scalar(
            input.query_ranks.subspan(job.query_offset, job.query_length),
            input.target_ranks.subspan(job.target_offset, job.target_length),
            job.max_edits,
            options);
    }
    return results;
}

/// Highway batch implementation with contiguous byte-vector scans per job.
template <class D>
[[nodiscard]] inline std::vector<SneakySnakeResult>
sneaky_snake_simd_batch_for(
    D d,
    const SneakySnakeBatchInput& input,
    SneakySnakeOptions options = {})
{
    std::vector<SneakySnakeResult> results(input.jobs.size());
    for (std::size_t index = 0u; index < input.jobs.size(); ++index) {
        const SneakySnakeJob& job = input.jobs[index];
        if (!sneaky_snake_job_in_bounds(input, job)) {
            results[index] = sneaky_snake_detail::make_result(
                0u, SneakySnakeStatus::unsupported_conservative, true);
            continue;
        }
        results[index] = sneaky_snake_detail::bounded_for<
            sneaky_snake_detail::CpuTraversal::highway>(
                d,
                input.query_ranks.subspan(
                    job.query_offset, job.query_length),
                input.target_ranks.subspan(
                    job.target_offset, job.target_length),
                job.max_edits,
                options);
    }
    return results;
}

/// Active-target Highway entry point used by the pipeline and benchmarks.
[[nodiscard]] inline std::vector<SneakySnakeResult> sneaky_snake_simd_batch(
    const SneakySnakeBatchInput& input,
    SneakySnakeOptions options = {})
{
    const hn::ScalableTag<std::uint8_t> d;
    return sneaky_snake_simd_batch_for(d, input, options);
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

} // export

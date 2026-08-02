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

export import bioseq.sequences.prefilters.prefilter_types;

export namespace bioseq::sequences::prefilters {

/// Production maze geometry shared by the CPU and Vulkan implementations.
inline constexpr std::uint32_t sneaky_snake_default_submaze_width = 512u;
inline constexpr std::uint32_t sneaky_snake_default_max_escape_segments = 512u;

struct SneakySnakeOptions {
    /// A zero value selects sneaky_snake_default_submaze_width.
    std::uint32_t submaze_width = sneaky_snake_default_submaze_width;
    /// Zero keeps y equal to the effective submaze width (512 by default).
    std::uint32_t max_escape_segments = 0u;
};

static_assert(std::is_standard_layout_v<SneakySnakeOptions>);
static_assert(std::is_trivially_copyable_v<SneakySnakeOptions>);
static_assert(sizeof(SneakySnakeOptions) == 2u * sizeof(std::uint32_t));
static_assert(alignof(SneakySnakeOptions) == alignof(std::uint32_t));
static_assert(offsetof(SneakySnakeOptions, submaze_width) == 0u);
static_assert(offsetof(SneakySnakeOptions, max_escape_segments) == 4u);

namespace sneaky_snake_detail {

[[nodiscard]] constexpr PrefilterResult make_result(
    std::uint32_t lower_bound,
    PrefilterStatus status,
    bool accepted) noexcept
{
    return make_prefilter_result(lower_bound, status, accepted);
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

/** Scalar primitive policy for the target-neutral maze traversal. */
struct ScalarTraversal final {
    [[nodiscard]] std::uint32_t diagonal_run(
        std::span<const std::uint8_t> query,
        std::span<const std::uint8_t> target,
        std::uint32_t position,
        std::uint32_t submaze_end,
        std::uint32_t query_index) const noexcept
    {
        return diagonal_run_scalar(
            query, target, position, submaze_end, query_index);
    }

    template <class Consumer>
    [[nodiscard]] bool visit_candidate_starts(
        std::span<const std::uint8_t> query,
        std::uint32_t filter_offset,
        std::uint8_t filter_rank,
        std::uint32_t position,
        std::uint32_t lower,
        std::uint32_t upper,
        Consumer&& consume) const noexcept
    {
        for (std::uint32_t candidate = lower;
             candidate <= upper;
             ++candidate) {
            if (candidate != position
                && query[static_cast<std::size_t>(candidate) + filter_offset]
                    == filter_rank
                && consume(candidate)) {
                return true;
            }
        }
        return false;
    }
};

/**
 * Target-neutral SneakySnake maze control shared by scalar and generated
 * fixed-target Highway policies. The policy owns only the two rank-scan
 * primitives; maze geometry, escape accounting, and result semantics have one
 * implementation in this facade.
 */
template <class Traversal>
[[nodiscard]] inline bool improve_diagonal_candidate_with(
    Traversal& traversal,
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

    best = std::max(best, traversal.diagonal_run(
        query, target, position, submaze_end, query_start));
    return best == remaining;
}

template <class Traversal>
[[nodiscard]] inline std::uint32_t longest_diagonal_run_with(
    Traversal& traversal,
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target,
    std::uint32_t position,
    std::uint32_t submaze_end,
    std::uint32_t max_edits) noexcept
{
    const std::uint32_t remaining = submaze_end - position;
    std::uint32_t best = traversal.diagonal_run(
        query, target, position, submaze_end, position);
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
    const bool completed = traversal.visit_candidate_starts(
        query,
        filter_offset,
        filter_rank,
        position,
        lower,
        upper,
        [&](const std::uint32_t candidate) noexcept {
            return improve_diagonal_candidate_with(
                traversal,
                query,
                target,
                position,
                submaze_end,
                candidate,
                best);
        });
    if (completed) {
        return remaining;
    }
    return best;
}

template <class Traversal>
[[nodiscard]] inline PrefilterResult equal_length_with(
    Traversal& traversal,
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target,
    std::uint32_t max_edits,
    SneakySnakeOptions options) noexcept
{
    const std::uint32_t length = static_cast<std::uint32_t>(query.size());
    if (length == 0u) {
        return make_result(0u, PrefilterStatus::success, true);
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
            const std::uint32_t best = longest_diagonal_run_with(
                traversal, query, target, position, end, max_edits);
            if (best == end - position) {
                position = end;
                break;
            }

            ++lower_bound;
            if (lower_bound > max_edits) {
                return make_result(
                    lower_bound,
                    PrefilterStatus::success,
                    false);
            }
            position += best + 1u;
            ++escapes;
        }
    }

    return make_result(
        lower_bound,
        PrefilterStatus::success,
        lower_bound <= max_edits);
}

/// Evaluate one already-size-checked rank pair without repeating span limits.
template <class Traversal>
[[nodiscard]] inline PrefilterResult bounded_with(
    Traversal& traversal,
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
            PrefilterStatus::conservative_unequal,
            difference <= max_edits);
    }

    return equal_length_with(
        traversal, query, target, max_edits, options);
}

/// Scalar traversal wrapper used by both public scalar entry points.
[[nodiscard]] inline PrefilterResult bounded_scalar(
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target,
    std::uint32_t max_edits,
    SneakySnakeOptions options) noexcept
{
    ScalarTraversal traversal;
    return bounded_with(traversal, query, target, max_edits, options);
}

} // namespace sneaky_snake_detail

/// Scalar single-job entry point over rank spans.
[[nodiscard]] inline PrefilterResult sneaky_snake_scalar(
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target,
    std::uint32_t max_edits,
    SneakySnakeOptions options = {}) noexcept
{
    constexpr std::size_t max_u32 = (std::numeric_limits<std::uint32_t>::max)();
    if (query.size() > max_u32 || target.size() > max_u32) {
        return sneaky_snake_detail::make_result(
            0u, PrefilterStatus::unsupported_conservative, true);
    }

    return sneaky_snake_detail::bounded_scalar(
        query, target, max_edits, options);
}

/// Scalar reference batch implementation over flat uint8 rank buffers.
[[nodiscard]] inline std::vector<PrefilterResult> sneaky_snake_scalar_batch(
    const PrefilterBatchInput& input,
    SneakySnakeOptions options = {})
{
    std::vector<PrefilterResult> results(input.jobs.size());
    for (std::size_t index = 0u; index < input.jobs.size(); ++index) {
        const PrefilterJob& job = input.jobs[index];
        if (!prefilter_job_in_bounds(input, job)) {
            results[index] = sneaky_snake_detail::make_result(
                0u, PrefilterStatus::unsupported_conservative, true);
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

} // namespace bioseq::sequences::prefilters

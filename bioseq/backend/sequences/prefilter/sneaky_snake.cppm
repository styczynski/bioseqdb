/**
 * @file sneaky_snake.cppm
 * @brief SneakySnake edit-distance filtering over rank bytes.
 *
 * For equal lengths, follows the longest matching run among nearby diagonals,
 * then charges an edit to pass the next mismatch. It repeats this within bounded
 * sequence blocks and rejects once the accumulated lower bound exceeds the threshold.
 * Unequal lengths use only the length difference.
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

export module bioseq.prefilter.sneaky_snake;

export import bioseq.prefilter.types;

export namespace bioseq::prefilter {

/// Default block width and escape limit shared by CPU and Vulkan SneakySnake.
inline constexpr std::uint32_t k_sneaky_snake_default_submaze_width = 512u;
inline constexpr std::uint32_t k_sneaky_snake_default_max_escape_segments = 512u;

struct sneaky_snake_options {
    /// A zero value selects k_sneaky_snake_default_submaze_width.
    std::uint32_t submaze_width = k_sneaky_snake_default_submaze_width;
    /// Zero allows as many mismatch escapes per block as the effective block width.
    std::uint32_t max_escape_segments = 0u;
};

static_assert(std::is_standard_layout_v<sneaky_snake_options>);
static_assert(std::is_trivially_copyable_v<sneaky_snake_options>);
static_assert(sizeof(sneaky_snake_options) == 2u * sizeof(std::uint32_t));
static_assert(alignof(sneaky_snake_options) == alignof(std::uint32_t));
static_assert(offsetof(sneaky_snake_options, submaze_width) == 0u);
static_assert(offsetof(sneaky_snake_options, max_escape_segments) == 4u);

namespace sneaky_snake_detail {

/// Returns the configured block width, using the default when it is zero.
[[nodiscard]] constexpr std::uint32_t effective_width(
    sneaky_snake_options options) noexcept
{
    return options.submaze_width == 0u
        ? k_sneaky_snake_default_submaze_width
        : options.submaze_width;
}

/// Returns the configured escape limit, using the block width when it is zero.
[[nodiscard]] constexpr std::uint32_t effective_escape_limit(
    sneaky_snake_options options,
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
    constexpr std::uint32_t k_word_bytes = sizeof(std::uint64_t);
    while (run_limit - run >= 2u * k_word_bytes) {
        std::uint64_t query_word = 0u;
        std::uint64_t target_word = 0u;
        std::memcpy(&query_word, query_data + run, k_word_bytes);
        std::memcpy(&target_word, target_data + run, k_word_bytes);
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

        std::memcpy(
            &query_word, query_data + run + k_word_bytes, k_word_bytes);
        std::memcpy(
            &target_word, target_data + run + k_word_bytes, k_word_bytes);
        const std::uint64_t second_different = query_word ^ target_word;
        if (second_different != 0u) {
            if constexpr (std::endian::native == std::endian::little) {
                return run + k_word_bytes + static_cast<std::uint32_t>(
                    std::countr_zero(second_different) / 8u);
            } else if constexpr (std::endian::native == std::endian::big) {
                return run + k_word_bytes + static_cast<std::uint32_t>(
                    std::countl_zero(second_different) / 8u);
            } else {
                break;
            }
        }
        run += 2u * k_word_bytes;
    }
    while (run_limit - run >= k_word_bytes) {
        std::uint64_t query_word = 0u;
        std::uint64_t target_word = 0u;
        std::memcpy(&query_word, query_data + run, k_word_bytes);
        std::memcpy(&target_word, target_data + run, k_word_bytes);
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
        run += k_word_bytes;
    }
    while (run < run_limit
           && target_data[run] == query_data[run]) {
        ++run;
    }
    return run;
}

/** Scalar rank comparisons and candidate scanning for the shared SneakySnake walk. */
struct scalar_traversal final {
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

    template <class p_consumer_type>
    [[nodiscard]] bool visit_candidate_starts(
        std::span<const std::uint8_t> query,
        std::uint32_t filter_offset,
        std::uint8_t filter_rank,
        std::uint32_t position,
        std::uint32_t lower,
        std::uint32_t upper,
        p_consumer_type&& consume) const noexcept
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
 * @brief Checks whether one shifted comparison improves the longest known match.
 *
 * Scalar and Highway implementations supply the rank-comparison function.
 * @return True when this candidate matches through the end of the block.
 */
template <class p_traversal_type>
[[nodiscard]] inline bool improve_diagonal_candidate_with(
    p_traversal_type& traversal,
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

template <class p_traversal_type>
[[nodiscard]] inline std::uint32_t longest_diagonal_run_with(
    p_traversal_type& traversal,
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
    // A longer match must agree at the position just beyond the current best
    // run. Use that rank to skip candidate diagonals that cannot improve it.
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
        [&traversal, &query, &target, &position, &submaze_end, &best](
            const std::uint32_t candidate) noexcept {
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

template <class p_traversal_type>
[[nodiscard]] inline prefilter_result equal_length_with(
    p_traversal_type& traversal,
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target,
    std::uint32_t max_edits,
    sneaky_snake_options options) noexcept
{
    const std::uint32_t length = static_cast<std::uint32_t>(query.size());
    if (length == 0u) {
        return make_prefilter_result(0u, prefilter_status::success, true);
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
                return make_prefilter_result(
                    lower_bound,
                    prefilter_status::success,
                    false);
            }
            position += best + 1u;
            ++escapes;
        }
    }

    return make_prefilter_result(
        lower_bound,
        prefilter_status::success,
        lower_bound <= max_edits);
}

/// Filters rank spans whose lengths have already been checked to fit uint32_t.
template <class p_traversal_type>
[[nodiscard]] inline prefilter_result bounded_with(
    p_traversal_type& traversal,
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target,
    std::uint32_t max_edits,
    sneaky_snake_options options) noexcept
{
    const std::uint32_t query_length = static_cast<std::uint32_t>(query.size());
    const std::uint32_t target_length = static_cast<std::uint32_t>(target.size());
    if (query_length != target_length) {
        const std::uint32_t difference = query_length > target_length
            ? query_length - target_length
            : target_length - query_length;
        return make_prefilter_result(
            difference,
            prefilter_status::conservative_unequal,
            difference <= max_edits);
    }

    return equal_length_with(
        traversal, query, target, max_edits, options);
}

/// Scalar traversal wrapper used by both public scalar entry points.
[[nodiscard]] inline prefilter_result bounded_scalar(
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target,
    std::uint32_t max_edits,
    sneaky_snake_options options) noexcept
{
    scalar_traversal traversal;
    return bounded_with(traversal, query, target, max_edits, options);
}

/**
 * @brief Evaluates each valid rank-buffer job with the supplied backend.
 *
 * Out-of-bounds jobs remain candidates without reading their slices. Scalar and
 * Highway batches share these bounds checks.
 * @return Results in job order.
 */
template <class p_evaluator_type>
[[nodiscard]] inline std::vector<prefilter_result> evaluate_batch_with(
    const prefilter_batch_input& input,
    p_evaluator_type&& evaluate)
{
    std::vector<prefilter_result> results(input.jobs.size());
    for (std::size_t index = 0u; index < input.jobs.size(); ++index) {
        const prefilter_job& job = input.jobs[index];
        if (!prefilter_job_in_bounds(input, job)) {
            results[index] = unsupported_prefilter_result();
            continue;
        }
        results[index] = evaluate(
            input.query_ranks.subspan(job.query_offset, job.query_length),
            input.target_ranks.subspan(job.target_offset, job.target_length),
            job);
    }
    return results;
}

} // namespace sneaky_snake_detail

/// Filters one rank pair on CPU. Lengths exceeding uint32_t are kept without evaluation.
[[nodiscard]] inline prefilter_result sneaky_snake_scalar(
    std::span<const std::uint8_t> query,
    std::span<const std::uint8_t> target,
    std::uint32_t max_edits,
    sneaky_snake_options options = {}) noexcept
{
    constexpr std::size_t k_maximum_u32 =
        (std::numeric_limits<std::uint32_t>::max)();
    if (query.size() > k_maximum_u32 || target.size() > k_maximum_u32) {
        return unsupported_prefilter_result();
    }

    return sneaky_snake_detail::bounded_scalar(
        query, target, max_edits, options);
}

/// Filters a batch of rank-buffer slices using the scalar SneakySnake walk.
[[nodiscard]] inline std::vector<prefilter_result> sneaky_snake_scalar_batch(
    const prefilter_batch_input& input,
    sneaky_snake_options options = {})
{
    return sneaky_snake_detail::evaluate_batch_with(
        input,
        [options](
            std::span<const std::uint8_t> query,
            std::span<const std::uint8_t> target,
            const prefilter_job& job) noexcept {
            return sneaky_snake_detail::bounded_scalar(
                query, target, job.max_edits, options);
        });
}

} // namespace bioseq::prefilter

/**
 * @file sneaky_snake.cppm
 * @brief SneakySnake edit-distance filtering over rank bytes.
 *
 * For equal lengths, follows the longest matching run among nearby diagonals,
 * then charges an  edit to pass the next mismatch. It repeats this within bounded
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
import bioseq.simd.utils;
import bioseq.common.checked_arithmetic;

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

/// Returns the configured escape limit, using the block width when it is zro.
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

/** Scalar native primitives. They never enter a selected Highway target. */
template<class Traits, bool Reverse>
[[nodiscard]] inline typename Traits::storage_unit packed_word(
    const void* units, std::size_t physical_total, std::size_t start, std::size_t count) noexcept {
    constexpr auto complement = bioseq::simd::packed_symbol_complement_mask<Traits, Reverse>();
    static_assert(complement.has_value());
    return bioseq::simd::load_oriented_packed_symbol_window<Traits, Reverse>(
        static_cast<const typename Traits::storage_unit*>(units), physical_total,
        start, count, *complement);
}

/** Validate all acquired ranks; the caller locates any failure with read_checked. */
template<class Traits, bool Reverse>
[[nodiscard]] inline bool validate_packed_scalar(
    const void* units, std::size_t total, std::size_t start, std::size_t length) noexcept {
    using unit_type = typename Traits::storage_unit;
    if constexpr (Traits::alphabet_size != (unit_type{1} << Traits::bits_per_char)) {
        for (std::size_t offset = 0u; offset < length;) {
            const auto count = std::min(Traits::letters_per_unit, length - offset);
            auto word = packed_word<Traits, Reverse>(units, total, start + offset, count);
            for (std::size_t symbol = 0u; symbol < count; ++symbol) {
                if ((word & Traits::char_mask) >= Traits::alphabet_size) return false;
                word >>= Traits::bits_per_char;
            }
            offset += count;
        }
    }
    return true;
}

/** Read an already validated logical rank, retaining the common orientation rule. */
template<class Traits, bool Reverse>
[[nodiscard]] inline std::uint8_t rank_packed_scalar(
    const void* units, std::size_t total, std::size_t start) noexcept {
    return static_cast<std::uint8_t>(packed_word<Traits, Reverse>(units, total, start, 1u));
}

/** Return the matching prefix, not the total number of equal symbols. */
template<class Traits, bool Reverse>
[[nodiscard]] inline std::size_t prefix_packed_scalar(
    const void* query, const void* target, std::size_t total,
    std::size_t query_start, std::size_t target_start, std::size_t length) noexcept {
    using unit_type = typename Traits::storage_unit;
    for (std::size_t offset = 0u; offset < length;) {
        const auto count = std::min(Traits::letters_per_unit, length - offset);
        const unit_type difference = packed_word<Traits, Reverse>(query, total, query_start + offset, count)
            ^ bioseq::simd::load_packed_symbol_window<Traits>(
                static_cast<const unit_type*>(target), target_start + offset, count);
        if (difference != 0u)
            return offset + static_cast<std::size_t>(std::countr_zero(difference)) / Traits::bits_per_char;
        offset += count;
    }
    return length;
}

/** Return ordered candidate bits for an already validated, at-most-64-symbol tile. */
template<class Traits, bool Reverse>
[[nodiscard]] inline std::uint64_t mask_packed_scalar(
    const void* query, std::size_t total, std::size_t start,
    std::size_t length, std::uint8_t rank) noexcept {
    using unit_type = typename Traits::storage_unit;
    const unit_type repeated = bioseq::simd::packed_symbol_low_bits<Traits>() * static_cast<unit_type>(rank);
    std::uint64_t result{};
    for (std::size_t offset = 0u; offset < length;) {
        const auto count = std::min(Traits::letters_per_unit, length - offset);
        const auto word = packed_word<Traits, Reverse>(query, total, start + offset, count);
        unit_type matches = ~bioseq::simd::packed_symbol_mismatch_bits<Traits>(
                static_cast<unit_type>(word ^ repeated))
            & bioseq::simd::packed_symbol_low_bits<Traits>() & bioseq::simd::packed_symbol_mask<Traits>(count);
        while (matches != 0u) {
            result |= std::uint64_t{1} << (offset + std::countr_zero(matches) / Traits::bits_per_char);
            matches &= matches - 1u;
        }
        offset += count;
    }
    return result;
}

/** Scalar rank comparisons and candidate  scanning for the shared SneakySnake walk. */
struct scalar_traversal final {
    /** Resolve the scalar implementation independently of any Highway table. */
    template<class Traits, bool Reverse>
    [[nodiscard]] static const prefilter_packed_operations* packed_operations() noexcept {
        static constexpr prefilter_packed_operations operations{
            &validate_packed_scalar<Traits, Reverse>, &rank_packed_scalar<Traits, Reverse>,
            &prefix_packed_scalar<Traits, Reverse>, &mask_packed_scalar<Traits, Reverse>};
        return &operations;
    }

    /** Starts bounded acquisition with the first rank and two existing word loads. */
    [[nodiscard]] static constexpr std::size_t read_block_size() noexcept {
        return 1u + 2u * sizeof(std::uint64_t);
    }

    [[nodiscard, gnu::always_inline]] inline std::uint32_t diagonal_run(
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
        p_consumer_type&& consume) const
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
template <class p_traversal_type, class p_sequence_type>
[[nodiscard]] inline bool improve_diagonal_candidate_with(
    p_traversal_type& traversal,
    const p_sequence_type& query,
    const p_sequence_type& target,
    std::uint32_t position,
    std::uint32_t submaze_end,
    std::uint32_t query_start,
    std::uint32_t& best)
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

template <class p_traversal_type, class p_sequence_type>
[[nodiscard]] inline std::uint32_t longest_diagonal_run_with(
    p_traversal_type& traversal,
    const p_sequence_type& query,
    const p_sequence_type& target,
    std::uint32_t position,
    std::uint32_t submaze_end,
    std::uint32_t max_edits)
{
    const std::uint32_t remaining = submaze_end - position;
    std::uint32_t best = traversal.diagonal_run(
        query, target, position, submaze_end, position);
    if (best == remaining) {
        return best;
    }

    const std::uint32_t length = static_cast<std::uint32_t>(query.size());
    // A longer match must agree at the position just beyond the current best
    // run. Use that rank to skip candidate diagonals that cannot improve it
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
        [&traversal, &query, &target, &position, &submaze_end, &best] [[gnu::always_inline]] (
            const std::uint32_t candidate) {
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

template <class p_traversal_type, class p_sequence_type>
[[nodiscard]] inline prefilter_result equal_length_with(
    p_traversal_type& traversal,
    const p_sequence_type& query,
    const p_sequence_type& target,
    std::uint32_t max_edits,
    sneaky_snake_options options)
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
            if constexpr (requires { query.failure; }) {
                if (query.failure->has_value()) return unsupported_prefilter_result();
            }
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

/// Filters rank spans whose lengths have already been  checked to fit uint32_t.
template <class p_traversal_type, class p_sequence_type>
[[nodiscard]] inline prefilter_result bounded_with(
    p_traversal_type& traversal,
    const p_sequence_type& query,
    const p_sequence_type& target,
    std::uint32_t max_edits,
    sneaky_snake_options options)
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
 * Applies existing scalar/Highway contiguous comparisons to bounded native windows.
 * The target window is owned scratch before any query read; candidate scans use
 * separate scratch so invoking the diagonal consumer cannot invalidate them.
 */
template <class Comparison>
struct window_traversal {
    Comparison comparison;
    prefilter_rank_window candidates;

    /** Retains one independent candidate cache for the lifetime of this job. */
    explicit window_traversal(const prefilter_rank_window& query)
        : comparison{}, candidates{query.source, query.start, query.length,
              query.cardinality, query.refill_size, query.job_index, false, *query.failure, query.packed} {}

    /**
     * Compares the first demand block before growing subsequent matched reads.
     * Growth stops at fixed scratch capacity and never changes diagonal boundaries.
     * A complete diagonal already pinned in both caches needs only one comparison.
     */
    [[nodiscard, gnu::always_inline]] inline std::uint32_t diagonal_run(
        const prefilter_rank_window& query, const prefilter_rank_window& target,
        std::uint32_t position, std::uint32_t submaze_end,
        std::uint32_t query_index) const {
        const auto limit = std::min<std::size_t>(submaze_end - position, query.size() - query_index);
        if (limit == 0u || query.failure->has_value()) return 0u;
        if (target.contains(position, limit) && query.contains(query_index, limit)) {
            return compare_acquired(query, target, query_index, position, limit);
        }
        return diagonal_run_with_refill(query, target, position, query_index, limit);
    }

    /**
     * Scans candidates through one retained cache, reusing validated query ranks.
     * Candidate scratch is independent of query refills performed by consume.
     */
    template <class Consumer>
    [[nodiscard]] bool visit_candidate_starts(
        const prefilter_rank_window& query, std::uint32_t filter_offset,
        std::uint8_t filter_rank, std::uint32_t position,
        std::uint32_t lower, std::uint32_t upper, Consumer&& consume) const {
        std::uint64_t first = lower;
        while (first <= upper) {
            const auto count = std::min<std::uint64_t>(prefilter_rank_window::chunk_size,
                static_cast<std::uint64_t>(upper) - first + 1u);
            if (!candidates.acquire(first + filter_offset, count, query)) return false;
            // Validate the entire original candidate acquisition before visiting
            // even the first matching bit or invoking a short-circuit consumer.
            if (query.packed != nullptr) {
                constexpr auto mask_capacity = std::numeric_limits<std::uint64_t>::digits;
                for (std::size_t offset = 0u; offset < count;) {
                    const auto mask_count = std::min<std::size_t>(mask_capacity, count - offset);
                    auto matches = query.packed->mask(query.source.resident->units,
                        query.physical_total(), query.physical_start(first + filter_offset + offset),
                        mask_count, filter_rank);
                    const auto excluded = static_cast<std::uint64_t>(position);
                    if (excluded >= first + offset && excluded - first - offset < mask_count)
                        matches &= ~(std::uint64_t{1} << (excluded - first - offset));
                    while (matches != 0u) {
                        const auto candidate = first + offset + std::countr_zero(matches);
                        if (consume(static_cast<std::uint32_t>(candidate))) return true;
                        if (query.failure->has_value()) return false;
                        matches &= matches - 1u;
                    }
                    offset += mask_count;
                }
                first += count;
                continue;
            }
            const auto ranks = candidates.window(first + filter_offset, count);
            if (query.failure->has_value()) return false;
            const auto excluded = position >= first && position - first < count
                ? static_cast<std::uint32_t>(position - first)
                : (std::numeric_limits<std::uint32_t>::max)();
            const bool completed = comparison.visit_candidate_starts(ranks, 0u,
                filter_rank, excluded, 0u, static_cast<std::uint32_t>(count - 1u),
                [&consume, first](std::uint32_t offset) {
                    return consume(static_cast<std::uint32_t>(first + offset));
                });
            if (completed) return true;
            if (query.failure->has_value()) return false;
            first += count;
        }
        return false;
    }

  private:
    /** Compare only intervals already acquired by the shared demand walk. */
    [[nodiscard, gnu::always_inline]] inline std::uint32_t compare_acquired(
        const prefilter_rank_window& query, const prefilter_rank_window& target,
        std::size_t query_position, std::size_t target_position, std::size_t count) const {
        if (query.packed != nullptr) {
            return static_cast<std::uint32_t>(query.packed->prefix(
                query.source.resident->units, target.source.resident->units, query.physical_total(),
                query.physical_start(query_position), target.physical_start(target_position), count));
        }
        const auto target_ranks = target.window(target_position, count);
        const auto query_ranks = query.window(query_position, count);
        return comparison.diagonal_run(query_ranks, target_ranks, 0u,
            static_cast<std::uint32_t>(count), 0u);
    }

    /**
     * Acquires missing diagonal blocks in target-before-query order, growing only
     * after a complete match. limit is the caller's admitted nonempty run length.
     * Keep refill/error handling out of the inline cache path: the linked body
     * otherwise gives even complete-cache comparisons a large call frame.
     */
    [[nodiscard, gnu::noinline]] std::uint32_t diagonal_run_with_refill(
        const prefilter_rank_window& query, const prefilter_rank_window& target,
        std::uint32_t position, std::uint32_t query_index, std::size_t limit) const {
        std::size_t matched = 0u;
        std::size_t read_size = query.refill_size;
        while (matched < limit) {
            const auto count = std::min(read_size, limit - matched);
            if (!target.acquire(position + matched, count) ||
                !query.acquire(query_index + matched, count, candidates)) return 0u;
            const auto run = compare_acquired(query, target, query_index + matched,
                position + matched, count);
            matched += run;
            if (run != count) break;
            read_size = std::min(prefilter_rank_window::chunk_size, read_size * 2u);
        }
        return static_cast<std::uint32_t>(matched);
    }
};

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

/// Filters a batch of rank-buffer slices using the, scalar SneakySnake walk.
[[nodiscard]] inline prefilter_batch_result sneaky_snake_scalar_batch(
    const prefilter_batch_input& input,
    sneaky_snake_options options = {})
{
    return evaluate_prefilter_batch(input,
        [options](const prefilter_rank_window& query, const prefilter_rank_window& target,
                  const prefilter_job& job) {
            sneaky_snake_detail::window_traversal<sneaky_snake_detail::scalar_traversal> traversal{query};
            return sneaky_snake_detail::bounded_with(
                traversal, query, target, job.max_edits, options);
        }, sneaky_snake_detail::scalar_traversal::read_block_size(),
        &resolve_prefilter_packed_operations<sneaky_snake_detail::scalar_traversal>);
}

} // namespace bioseq::prefilter

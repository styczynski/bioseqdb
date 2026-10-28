/**
 * @file edit_distance_threshold.cppm
 * @brief Check whether two sequences are within an edit-distance limit.
 *
 * Keep only the diagonal band that can contain an alignment  within the limit.
 * Thresholds up to eight use stack storage. Larger thresholds allocate, two
 * rows whose sizes grow with the threshold
 */
module;

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <expected>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

export module bioseq.sequences.edit_distance_threshold;

import bioseq.common.context;
import bioseq.common.checked_arithmetic;
import bioseq.prefilter.types;
import bioseq.sequences;
import bioseq.storage.lazy_sequence;

/** @brief Helpers for evaluating the edit-distance band. */
namespace bioseq::sequences::edit_distance_threshold_detail {

/**
 * @brief Add one edit, keeping distances above the threshold at cap.
 * @tparam p_cell_type Unsigned type used for distance cells.
 * @param value Distance before the edit.
 * @param cap Value used for all distances abive the threshold.
 * @return The increased distance, limited to cap.
 */
template <class p_cell_type>
[[nodiscard]] constexpr p_cell_type add_one_capped(
    p_cell_type value,
    p_cell_type cap) noexcept
{
    return value >= cap
        ? cap
        : static_cast<p_cell_type>(value + p_cell_type{1});
}

/**
 * @brief Compute edit-distance rows inside the allowed diagonal band.
 *
 * A path outside this band already needs too many insertions or deletions.
 * Keep only the current and previous rows, with distances above the limit
 * stored as cap.
 *
 * @tparam p_symbol_type Symbol type that supports equality comparison.
 * @tparam p_cell_type Unsigned type used for distance cells.
 * @param query Query symbols.
 * @param target Target symbols.
 * @param max_edits Largest accepted number of edits.
 * @param cap Value max_edits + 1, which must fit the cell type.
 * @param prior_row Storage for the previous row, including boundary cells.
 * @param current Equal-sized scratch storage for the next  row.
 * @param center Index of the main diagonal in either row.
 * @return Whether the complete sequences can be aligned within max_edits.
 */
template <class p_sequence_type, class p_cell_type>
[[nodiscard]] bool evaluate_rows(
    const p_sequence_type& query,
    const p_sequence_type& target,
    std::uint32_t max_edits,
    p_cell_type cap,
    std::span<p_cell_type> prior_row,
    std::span<p_cell_type> current,
    std::size_t center)
{
    const std::size_t m = query.size();
    const std::size_t n = target.size();
    const std::size_t edits = static_cast<std::size_t>(max_edits);

    std::fill(prior_row.begin(), prior_row.end(), cap);
    const std::size_t initial_high = std::min(n, edits);
    for (std::size_t j = 0u; j <= initial_high; ++j) {
        prior_row[center + j] = static_cast<p_cell_type>(j);
    }

    for (std::size_t i = 1u; i <= m; ++i) {
        std::fill(current.begin(), current.end(), cap);
        const std::size_t j_low = i > edits ? i - edits : 0u;
        const std::size_t j_high = std::min(
            n, bioseq::common::checked_add(i, edits).value_or(
                (std::numeric_limits<std::size_t>::max)()));

        for (std::size_t j = j_low;; ++j) {
            const std::size_t slot = j >= i
                ? center + (j - i)
                : center - (i - j);
            if (j == 0u) {
                current[slot] = static_cast<p_cell_type>(std::min<std::uint64_t>(
                    static_cast<std::uint64_t>(i),
                    static_cast<std::uint64_t>(cap)));
            } else {
                const p_cell_type deletion = add_one_capped(
                    prior_row[slot + 1u], cap);
                const p_cell_type insertion = add_one_capped(
                    current[slot - 1u], cap);
                const p_cell_type diagonal = query[i - 1u] == target[j - 1u]
                    ? prior_row[slot]
                    : add_one_capped(prior_row[slot], cap);
                current[slot] = std::min({
                    deletion, insertion, diagonal});
            }
            if (j == j_high) break;
        }
        if constexpr (requires { query.failure; target.failure; }) {
            if (query.failure->has_value() || target.failure->has_value()) return false;
        }
        std::swap(prior_row, current);
    }

    const std::size_t final_slot = n >= m
        ? center + (n - m)
        : center - (m - n);
    return prior_row[final_slot] <= static_cast<p_cell_type>(max_edits);
}

} // namespace bioseq::sequences::edit_distance_threshold_detail

export namespace bioseq::sequences {

/**
 * @brief Check whether transforming one complete sequence into, the other needs
 * at most max_edits substitutions, insertions, and deletions.
 *
 * Each edit costs one Compare symbols as supplied, so callers can use decoded
 * alphabet ranks or another representation wcth an equality operator.
 *
 * @tparam p_symbol_type Symbol type that supports equality comparison.
 * @param query Query symbols.
 * @param target Target symbols.
 * @param max_edits Largest accepted number of edits.
 * @return True when the minimum global edit distance is  at most max_edits.
 * @throws std::length_error If the required band width cannot fit in size_t.
 */
template <class p_sequence_type>
[[nodiscard]] bool edit_distance_within_threshold_ranks(
    const p_sequence_type& query,
    const p_sequence_type& target,
    std::uint32_t max_edits)
{
    const std::size_t m = query.size();
    const std::size_t n = target.size();
    const std::size_t difference = m >= n ? m - n : n - m;
    if (difference > static_cast<std::uint64_t>(max_edits)) {
        return false;
    }
    if (m <= static_cast<std::uint64_t>(max_edits)
        && n <= static_cast<std::uint64_t>(max_edits)) {
        return true;
    }

    constexpr std::uint32_t fixed_max_edits = 8u;
    constexpr std::size_t fixed_cells = 2u * fixed_max_edits + 3u;
    if (max_edits <= fixed_max_edits) {
        const std::uint32_t cap = max_edits + 1u;
        const std::size_t center =
            static_cast<std::size_t>(max_edits) + 1u;
        const std::size_t width =
            2u * static_cast<std::size_t>(max_edits) + 3u;
        std::array<std::uint32_t, fixed_cells> prior_row{};
        std::array<std::uint32_t, fixed_cells> current{};
        return edit_distance_threshold_detail::evaluate_rows(
            query,
            target,
            max_edits,
            cap,
            std::span<std::uint32_t>{prior_row}.first(width),
            std::span<std::uint32_t>{current}.first(width),
            center);
    }

    const std::uint64_t cap =
        static_cast<std::uint64_t>(max_edits) + 1u;
    const std::uint64_t width_wide =
        2u * static_cast<std::uint64_t>(max_edits) + 3u;
    constexpr std::size_t maximum_size =
        (std::numeric_limits<std::size_t>::max)();
    if (width_wide > static_cast<std::uint64_t>(maximum_size)) {
        throw std::length_error(
            "edit-distance threshold band does not fit size_t");
    }
    const std::size_t width = static_cast<std::size_t>(width_wide);
    const std::size_t center =
        static_cast<std::size_t>(max_edits) + 1u;
    auto& memory = ::context::current();
    auto scope = memory.push<::context::arena::scratch>();
    const auto prior_row = memory.alloc_scratch_array<std::uint64_t>(width);
    const auto current = memory.alloc_scratch_array<std::uint64_t>(width);
    return edit_distance_threshold_detail::evaluate_rows(
        query,
        target,
        max_edits,
        cap,
        std::span<std::uint64_t>{prior_row},
        std::span<std::uint64_t>{current},
        center);
}

/** Evaluates equality-comparable resident symbols through the shared banded recurrence. */
template <std::equality_comparable Symbol>
[[nodiscard]] bool edit_distance_within_threshold(
    std::span<const Symbol> query, std::span<const Symbol> target,
    std::uint32_t max_edits) {
    return edit_distance_within_threshold_ranks(query, target, max_edits);
}

/** Evaluates checked native-rank windows without expanding either complete input. */
[[nodiscard]] inline std::expected<bool, prefilter::prefilter_input_failure>
edit_distance_within_threshold(
    const prefilter::prefilter_rank_window& query,
    const prefilter::prefilter_rank_window& target,
    std::uint32_t max_edits) {
    const bool accepted = edit_distance_within_threshold_ranks(query, target, max_edits);
    if (*query.failure) return std::unexpected(**query.failure);
    if (*target.failure) return std::unexpected(**target.failure);
    return accepted;
}

/** Checks two oriented biological sources using bounded native-rank windows. */
[[nodiscard]] inline std::expected<bool, prefilter::prefilter_input_failure>
edit_distance_within_threshold(
    const storage::borrowed_rank_window_source& query,
    const storage::borrowed_rank_window_source& target,
    std::uint32_t max_edits) {
    constexpr auto maximum_length = (std::numeric_limits<std::uint32_t>::max)();
    if (query.symbol_count > maximum_length || target.symbol_count > maximum_length) {
        return std::unexpected(prefilter::prefilter_input_failure{
            prefilter::prefilter_input_failure_code::invalid_descriptor, 0u,
            target.symbol_count > maximum_length, 0u});
    }
    const std::array queries{query};
    const std::array targets{target};
    const std::array jobs{prefilter::prefilter_job{
        .query_length = static_cast<std::uint32_t>(query.symbol_count),
        .target_length = static_cast<std::uint32_t>(target.symbol_count),
        .max_edits = max_edits,
        .orientation = static_cast<std::uint32_t>(query.orientation)}};
    return prefilter::evaluate_prefilter_job({queries, targets, jobs}, 0u,
        [max_edits](const prefilter::prefilter_rank_window& query_window,
                    const prefilter::prefilter_rank_window& target_window,
                    const prefilter::prefilter_job&) {
            return edit_distance_within_threshold_ranks(query_window, target_window, max_edits);
        });
}

/** Checks resident compact operands without a whole-input rank conversion. */
template <supports_compact_sequence_range QueryRange, supports_compact_sequence_range TargetRange>
[[nodiscard]] auto edit_distance_within_threshold(
    const compact_sequence_range<QueryRange>& query,
    const compact_sequence_range<TargetRange>& target, std::uint32_t max_edits) {
    static_assert(std::same_as<typename compact_sequence_range<QueryRange>::traits::alphabet_type,
                              typename compact_sequence_range<TargetRange>::traits::alphabet_type>);
    return edit_distance_within_threshold(storage::make_borrowed_rank_source(query),
        storage::make_borrowed_rank_source(target), max_edits);
}

/** Checks lazy biological operands while preserving their backend ownership contract. */
template <sequence_alphabet Alphabet>
[[nodiscard]] auto edit_distance_within_threshold(
    const storage::lazy_sequence_view<Alphabet>& query,
    const storage::lazy_sequence_view<Alphabet>& target, std::uint32_t max_edits) {
    return edit_distance_within_threshold(query.template borrow_rank_source<false>(),
        target.template borrow_rank_source<false>(), max_edits);
}

} // namespace bioseq::sequences

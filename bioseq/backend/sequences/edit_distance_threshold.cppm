/**
 * @file edit_distance_threshold.cppm
 * @brief Banded exact global edit-distance threshold predicate.
 *
 * The implementation stores only the active diagonal band. Small thresholds
 * use fixed stack arrays. Larger thresholds allocate two `O(max_edits)` rows.
 */
module;

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

export module bioseq.sequences.edit_distance_threshold;

namespace bioseq::sequences::edit_distance_threshold_detail {

/**
 * @brief Increment a dynamic-programming cell without exceeding its sentinel.
 * @tparam p_cell_type Unsigned cell type.
 * @param value Current cell value.
 * @param cap Sentinel value representing distance beyond the threshold.
 * @return Incremented value or `cap` when already saturated.
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
 * @brief Add two sizes while saturating at `size_t` maximum.
 * @param lhs Left operand.
 * @param rhs Right operand.
 * @return Saturating sum.
 */
[[nodiscard]] constexpr std::size_t add_size_saturated(
    std::size_t lhs,
    std::size_t rhs) noexcept
{
    constexpr std::size_t maximum =
        (std::numeric_limits<std::size_t>::max)();
    return rhs > maximum - lhs ? maximum : lhs + rhs;
}

/**
 * @brief Evaluate a threshold-width global edit-distance band.
 * @tparam p_symbol_type Equality-comparable logical-symbol type.
 * @tparam p_cell_type Unsigned row-cell type.
 * @param query Query symbols.
 * @param target Target symbols.
 * @param max_edits Inclusive distance threshold.
 * @param cap Sentinel equal to `max_edits + 1`.
 * @param prior_row Dynamic-programming band row for the preceding query symbol.
 * @param current Scratch storage for the current band row.
 * @param center Slot corresponding to the main diagonal.
 * @return `true` when the exact global distance does not exceed the threshold.
 */
template <class p_symbol_type, class p_cell_type>
[[nodiscard]] bool evaluate_rows(
    std::span<const p_symbol_type> query,
    std::span<const p_symbol_type> target,
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
            n, add_size_saturated(i, edits));

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
 * @brief Test whether exact global edit distance is within a threshold.
 *
 * The predicate is alphabet-neutral: equality is evaluated on the supplied
 * symbol representation, so no rank conversion or sequence materialization
 * occurs inside this function.
 *
 * @tparam p_symbol_type Equality-comparable logical-symbol type.
 * @param query Query symbols.
 * @param target Target symbols.
 * @param max_edits Inclusive unit-cost edit-distance threshold.
 * @return `true` exactly when the global edit distance is at most `max_edits`.
 * @throws std::length_error If the dynamic band width cannot fit `size_t`.
 */
template <std::equality_comparable p_symbol_type>
[[nodiscard]] bool edit_distance_within_threshold(
    std::span<const p_symbol_type> query,
    std::span<const p_symbol_type> target,
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
    std::vector<std::uint64_t> prior_row(width);
    std::vector<std::uint64_t> current(width);
    return edit_distance_threshold_detail::evaluate_rows(
        query,
        target,
        max_edits,
        cap,
        std::span<std::uint64_t>{prior_row},
        std::span<std::uint64_t>{current},
        center);
}

} // namespace bioseq::sequences

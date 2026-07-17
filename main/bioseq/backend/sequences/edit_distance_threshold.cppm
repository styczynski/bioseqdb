/**
 * @file edit_distance_threshold.cppm
 * @brief Exact alphabet-generic global edit-distance threshold predicate.
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

template <class Cell>
[[nodiscard]] constexpr Cell add_one_capped(
    Cell value,
    Cell cap) noexcept
{
    return value >= cap ? cap : static_cast<Cell>(value + Cell{1});
}

[[nodiscard]] constexpr std::size_t add_size_saturated(
    std::size_t lhs,
    std::size_t rhs) noexcept
{
    constexpr std::size_t maximum =
        (std::numeric_limits<std::size_t>::max)();
    return rhs > maximum - lhs ? maximum : lhs + rhs;
}

template <class Symbol, class Cell>
[[nodiscard]] bool evaluate_rows(
    std::span<const Symbol> query,
    std::span<const Symbol> target,
    std::uint32_t max_edits,
    Cell cap,
    std::span<Cell> previous,
    std::span<Cell> current,
    std::size_t center)
{
    const std::size_t m = query.size();
    const std::size_t n = target.size();
    const std::size_t edits = static_cast<std::size_t>(max_edits);

    std::fill(previous.begin(), previous.end(), cap);
    const std::size_t initial_high = std::min(n, edits);
    for (std::size_t j = 0u; j <= initial_high; ++j) {
        previous[center + j] = static_cast<Cell>(j);
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
                current[slot] = static_cast<Cell>(std::min<std::uint64_t>(
                    static_cast<std::uint64_t>(i),
                    static_cast<std::uint64_t>(cap)));
            } else {
                const Cell deletion = add_one_capped(
                    previous[slot + 1u], cap);
                const Cell insertion = add_one_capped(
                    current[slot - 1u], cap);
                const Cell diagonal = query[i - 1u] == target[j - 1u]
                    ? previous[slot]
                    : add_one_capped(previous[slot], cap);
                current[slot] = std::min({
                    deletion, insertion, diagonal});
            }
            if (j == j_high) break;
        }
        std::swap(previous, current);
    }

    const std::size_t final_slot = n >= m
        ? center + (n - m)
        : center - (m - n);
    return previous[final_slot] <= static_cast<Cell>(max_edits);
}

} // namespace bioseq::sequences::edit_distance_threshold_detail

export namespace bioseq::sequences {

template <std::equality_comparable Symbol>
[[nodiscard]] bool edit_distance_within_threshold(
    std::span<const Symbol> query,
    std::span<const Symbol> target,
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
        std::array<std::uint32_t, fixed_cells> previous{};
        std::array<std::uint32_t, fixed_cells> current{};
        return edit_distance_threshold_detail::evaluate_rows(
            query,
            target,
            max_edits,
            cap,
            std::span<std::uint32_t>{previous}.first(width),
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
    std::vector<std::uint64_t> previous(width);
    std::vector<std::uint64_t> current(width);
    return edit_distance_threshold_detail::evaluate_rows(
        query,
        target,
        max_edits,
        cap,
        std::span<std::uint64_t>{previous},
        std::span<std::uint64_t>{current},
        center);
}

} // namespace bioseq::sequences

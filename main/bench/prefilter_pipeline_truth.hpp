#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bioseq::bench_prefilter_truth {

/// Exact global Levenshtein threshold predicate using only the 2E+1 band.
/// This oracle runs during benchmark evaluation, outside the timed callback.
[[nodiscard]] inline bool edit_distance_within(
    std::string_view query,
    std::string_view target,
    std::uint32_t max_edits)
{
    const std::size_t m = query.size();
    const std::size_t n = target.size();
    const std::size_t e = static_cast<std::size_t>(max_edits);
    const std::size_t difference = m >= n ? m - n : n - m;
    if (difference > e) {
        return false;
    }
    if (m == 0u || n == 0u) {
        return std::max(m, n) <= e;
    }

    const std::uint32_t inf = max_edits ==
            (std::numeric_limits<std::uint32_t>::max)()
        ? max_edits
        : max_edits + 1u;
    const auto add_capped = [inf](std::uint32_t value,
                                std::uint32_t increment) noexcept {
        if (value >= inf || increment > inf - value) {
            return inf;
        }
        return static_cast<std::uint32_t>(value + increment);
    };

    std::vector<std::uint32_t> previous(n + 1u, inf);
    std::vector<std::uint32_t> current(n + 1u, inf);
    for (std::size_t j = 0u; j <= std::min(n, e); ++j) {
        previous[j] = static_cast<std::uint32_t>(j);
    }

    for (std::size_t i = 1u; i <= m; ++i) {
        const std::size_t low = i > e ? i - e : 1u;
        const std::size_t high = std::min(n, i + e);
        current[0] = i <= e ? static_cast<std::uint32_t>(i) : inf;
        if (low > 1u) {
            current[low - 1u] = inf;
        }
        std::uint32_t row_minimum = inf;
        for (std::size_t j = low; j <= high; ++j) {
            const std::uint32_t substitution = add_capped(
                previous[j - 1u],
                static_cast<std::uint32_t>(
                    query[i - 1u] != target[j - 1u]));
            const std::uint32_t deletion = add_capped(previous[j], 1u);
            const std::uint32_t insertion = add_capped(current[j - 1u], 1u);
            current[j] = std::min({substitution, deletion, insertion, inf});
            row_minimum = std::min(row_minimum, current[j]);
        }
        if (high < n) {
            current[high + 1u] = inf;
        }
        if (row_minimum > max_edits) {
            return false;
        }
        previous.swap(current);
    }
    return previous[n] <= max_edits;
}

[[nodiscard]] inline char complement_acgt(char base)
{
    switch (base) {
        case 'A': return 'T';
        case 'C': return 'G';
        case 'G': return 'C';
        case 'T': return 'A';
        default:
            throw std::invalid_argument(
                "prefilter truth oracle received a non-ACGT base");
    }
}

/// Orient the forward reference truth interval exactly as the generated query.
[[nodiscard]] inline std::string oriented_truth_sequence(
    std::string_view forward_truth,
    bool reverse)
{
    if (!reverse) {
        return std::string{forward_truth};
    }
    std::string result(forward_truth.size(), 'A');
    for (std::size_t index = 0u; index < forward_truth.size(); ++index) {
        result[index] = complement_acgt(
            forward_truth[forward_truth.size() - 1u - index]);
    }
    return result;
}

/// Recall over independently threshold-qualified truth pairs. An empty truth
/// set is vacuously complete; an all-missed nonempty set remains exactly zero.
[[nodiscard]] inline double primary_correct_fraction(
    std::size_t correct_pairs,
    std::size_t truth_pairs)
{
    if (correct_pairs > truth_pairs) {
        throw std::logic_error(
            "prefilter pipeline produced more correct pairs than its exact "
            "threshold truth oracle");
    }
    return truth_pairs == 0u
        ? 1.0
        : static_cast<double>(correct_pairs)
            / static_cast<double>(truth_pairs);
}

struct PrimaryCorrectnessSummary {
    std::size_t truth_pairs = 0u;
    std::size_t correct_truth_pairs = 0u;
    std::size_t observed_correct_pairs = 0u;
    double fraction = 1.0;
};

/// Pair the exact-threshold oracle and observed correctness by input index so
/// an ineligible reported match cannot mask a missed eligible truth pair.
[[nodiscard]] inline PrimaryCorrectnessSummary summarize_primary_correctness(
    std::span<const std::uint8_t> truth_flags,
    std::span<const std::uint8_t> correct_flags)
{
    if (truth_flags.size() != correct_flags.size()) {
        throw std::invalid_argument(
            "prefilter truth and correctness flags are not in lock-step");
    }

    PrimaryCorrectnessSummary result;
    for (std::size_t index = 0u; index < truth_flags.size(); ++index) {
        if (truth_flags[index] > 1u || correct_flags[index] > 1u) {
            throw std::invalid_argument(
                "prefilter truth and correctness flags must be binary");
        }
        const bool in_truth = truth_flags[index] != 0u;
        const bool correct = correct_flags[index] != 0u;
        result.truth_pairs += in_truth ? 1u : 0u;
        result.correct_truth_pairs += in_truth && correct ? 1u : 0u;
        result.observed_correct_pairs += correct ? 1u : 0u;
    }
    result.fraction = primary_correct_fraction(
        result.correct_truth_pairs, result.truth_pairs);
    return result;
}

} // namespace bioseq::bench_prefilter_truth

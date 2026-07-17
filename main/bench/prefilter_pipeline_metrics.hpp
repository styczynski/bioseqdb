#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bioseq::bench_prefilter_metrics {

[[nodiscard]] inline char complement_acgt(char base)
{
    switch (base) {
        case 'A': return 'T';
        case 'C': return 'G';
        case 'G': return 'C';
        case 'T': return 'A';
        default:
            throw std::invalid_argument(
                "prefilter metric received a non-ACGT base");
    }
}

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

} // namespace bioseq::bench_prefilter_metrics

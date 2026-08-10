/**
 * @file prefilter_pipeline_metrics.hpp
 * @brief Correctness metrics shared by prefilter benchmark drivers.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

/** @brief Primary-prefilter correctness calculations for benchmark rows. */
namespace bioseq::bench::prefilter_metrics {

/**
 * @brief Returns the Watson-Crick complement of one uppercase DNA4 base.
 * @param[in] base Uppercase A, C, G, or T.
 * @return Complementary uppercase DNA4 base.
 * @throws std::invalid_argument When the input is outside DNA4.
 */
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

/**
 * @brief Materializes the truth sequence in the requested orientation.
 * @param[in] forward_truth Forward-oriented DNA4 truth sequence.
 * @param[in] reverse Whether to return the reverse complement.
 * @return Oriented DNA4 sequence.
 * @throws std::invalid_argument When reverse-complementing input outside DNA4.
 * @note Forward orientation preserves the supplied text verbatim; benchmark
 * callers provide validated DNA4 truth sequences.
 */
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

/**
 * @brief Computes the fraction of truth pairs reported correctly.
 * @param[in] correct_pairs Correctly reported truth-pair count.
 * @param[in] truth_pairs Total truth-pair count.
 * @return One for an empty truth set, otherwise correct divided by truth.
 * @throws std::logic_error When correct pairs exceed the truth population.
 */
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

/** @brief Counts supporting one primary-prefilter correctness measurement. */
struct primary_correctness_summary {
    /** @brief Number of pairs in the exact-threshold truth set. */
    std::size_t truth_pairs = 0u;

    /** @brief Truth-set pairs reported with the correct classification. */
    std::size_t correct_truth_pairs = 0u;

    /** @brief All observed pairs carrying a correct classification. */
    std::size_t observed_correct_pairs = 0u;

    /** @brief Correct truth-set fraction, with an empty-set value of one. */
    double fraction = 1.0;
};

/**
 * @brief Summarizes truth membership and observed correctness flags.
 * @param[in] truth_flags Binary membership in the exact-threshold truth set.
 * @param[in] correct_flags Binary correctness of each observed pair.
 * @return Counts and fraction for the measurement row.
 * @throws std::invalid_argument On mismatched lengths or non-binary flags.
 */
[[nodiscard]] inline primary_correctness_summary summarize_primary_correctness(
    std::span<const std::uint8_t> truth_flags,
    std::span<const std::uint8_t> correct_flags)
{
    if (truth_flags.size() != correct_flags.size()) {
        throw std::invalid_argument(
            "prefilter truth and correctness flags are not in lock-step");
    }

    primary_correctness_summary result;
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

}  // namespace bioseq::bench::prefilter_metrics

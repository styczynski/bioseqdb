/**
 * @file utils.cppm
 * @brief Canonical scoring-matrix storage shared by SIMD alignment kernels.
 */
module;

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

export module bioseq.alignment.utils;

import bioseq.common.seqan_adapter;
import bioseq.sequences;

export namespace bioseq::simd {

/**
 * @brief Stores canonical scalar scores and their Highway shuffle projection.
 *
 * @tparam p_alphabet Logical sequence alphabet.
 * @tparam p_alphabet_dimension Alphabet size including the wildcard rank.
 */
template <sequence_alphabet p_alphabet, std::size_t p_alphabet_dimension>
struct scoring_matrix_storage {
    using alphabet_type = p_alphabet;

    static constexpr std::size_t k_alphabet_dimension = p_alphabet_dimension;
    static_assert(
        p_alphabet_dimension > 1,
        "Scoring matrices require at least two ranks");

    static constexpr std::size_t k_entry_count =
        p_alphabet_dimension * p_alphabet_dimension;
    static constexpr int k_dimension =
        static_cast<int>(p_alphabet_dimension);

    /** @brief Row-major scalar scoring entries. */
    std::array<std::int8_t, k_entry_count> entries{};

    static constexpr std::size_t k_simd_rows = p_alphabet_dimension + 1;
    static constexpr std::size_t k_simd_lookup_count =
        (p_alphabet_dimension + 1 + 15) / 16;
    static constexpr std::size_t k_simd_columns = k_simd_lookup_count * 16;
    static constexpr std::uint8_t k_null_byte =
        static_cast<std::uint8_t>(p_alphabet_dimension);

    static_assert(
        p_alphabet_dimension
            < std::numeric_limits<std::uint8_t>::max(),
        "Scoring rank and null sentinel must fit in uint8_t");

    /** @brief Shuffle-table rows used by Highway score lookup. */
    alignas(64) std::array<
        std::int8_t,
        k_simd_rows * k_simd_columns> simd_scores{};
    static_assert(
        sizeof(decltype(simd_scores))
            == k_simd_rows * k_simd_columns * sizeof(std::int8_t),
        "SIMD scoring storage must not add element overhead");

    /**
     * @brief Returns a scalar score by row and column.
     * @param[in] row Zero-based alphabet row.
     * @param[in] column Zero-based alphabet column.
     * @return Stored score at the requested coordinate.
     */
    [[nodiscard]] constexpr std::int8_t at(
        int row,
        int column) const noexcept {
        return entries[
            static_cast<std::size_t>(row) * p_alphabet_dimension + column];
    }

    /**
     * @brief Returns a mutable scalar score by row and column.
     * @param[in] row Zero-based alphabet row.
     * @param[in] column Zero-based alphabet column.
     * @return Mutable stored score at the requested coordinate.
     */
    [[nodiscard]] constexpr std::int8_t& at(
        int row,
        int column) noexcept {
        return entries[
            static_cast<std::size_t>(row) * p_alphabet_dimension + column];
    }

    /**
     * @brief Returns the canonical match score.
     * @return Score at the first diagonal matrix coordinate.
     */
    [[nodiscard]] constexpr std::int8_t match_score() const noexcept {
        return entries[0];
    }

    /**
     * @brief Returns the canonical mismatch score.
     * @return Score at the first off-diagonal matrix coordinate.
     */
    [[nodiscard]] constexpr std::int8_t mismatch_score() const noexcept {
        return entries[1];
    }

    /**
     * @brief Returns the wildcard score.
     * @return Score at the final matrix coordinate.
     */
    [[nodiscard]] constexpr std::int8_t wildcard_score() const noexcept {
        return entries[k_entry_count - 1];
    }

    /**
     * @brief Returns the first byte of a SIMD lookup row.
     * @param[in] row Zero-based row including the sentinel row.
     * @return Pointer into the aligned shuffle-table storage.
     */
    [[nodiscard]] const std::int8_t* simd_row_pointer(
        std::size_t row) const noexcept {
        return simd_scores.data() + row * k_simd_columns;
    }

    /**
     * @brief Validates that shuffle rows exactly project the scalar matrix.
     * @return `true` when scores and sentinel padding match the canonical layout.
     */
    [[nodiscard]] bool is_block_layout_valid() const noexcept {
        for (std::size_t row = 0; row < p_alphabet_dimension; ++row) {
            for (std::size_t column = 0;
                 column < p_alphabet_dimension;
                 ++column) {
                if (simd_scores[row * k_simd_columns + column]
                    != entries[row * p_alphabet_dimension + column]) {
                    return false;
                }
            }
            for (std::size_t column = p_alphabet_dimension;
                 column < k_simd_columns;
                 ++column) {
                if (simd_scores[row * k_simd_columns + column] != INT8_MIN) {
                    return false;
                }
            }
        }
        for (std::size_t column = 0; column < k_simd_columns; ++column) {
            if (simd_scores[p_alphabet_dimension * k_simd_columns + column]
                != INT8_MIN) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief Computes a stable identity for every score consumed by Block Aligner.
     * @return FNV-1a identity over the dimension and row-major scalar scores.
     */
    [[nodiscard]] std::uint64_t block_semantic_identity() const noexcept {
        std::uint64_t hash = 1469598103934665603ULL;
        const auto mix = [&hash](std::uint64_t value) noexcept {
            hash ^= value;
            hash *= 1099511628211ULL;
        };
        mix(p_alphabet_dimension);
        for (const std::int8_t score : entries) {
            mix(static_cast<std::uint8_t>(score));
        }
        return hash;
    }

    /**
     * @brief Generates scalar scores and their SIMD lookup projection.
     *
     * Positive match input remains positive. Mismatch and ambiguity inputs
     * are normalized to penalties. The last rank is the wildcard rank.
     *
     * @param[in] match Match score or its negated representation.
     * @param[in] mismatch_penalty Mismatch penalty in either sign convention.
     * @param[in] ambiguity_penalty Wildcard penalty in either sign convention.
     * @return Complete scalar matrix and aligned shuffle projection.
     */
    [[nodiscard]] static scoring_matrix_storage generate(
        std::int8_t match,
        std::int8_t mismatch_penalty,
        std::int8_t ambiguity_penalty) {
        scoring_matrix_storage matrix{};
        constexpr int k_base_count = k_dimension - 1;

        const std::int8_t positive_match = match > 0
            ? match
            : static_cast<std::int8_t>(-match);
        const std::int8_t negative_mismatch = mismatch_penalty > 0
            ? static_cast<std::int8_t>(-mismatch_penalty)
            : mismatch_penalty;
        const std::int8_t negative_ambiguity = ambiguity_penalty > 0
            ? static_cast<std::int8_t>(-ambiguity_penalty)
            : ambiguity_penalty;

        for (int row = 0; row < k_base_count; ++row) {
            for (int column = 0; column < k_base_count; ++column) {
                matrix.at(row, column) = row == column
                    ? positive_match
                    : negative_mismatch;
            }
            matrix.at(row, k_base_count) = negative_ambiguity;
        }
        for (int column = 0; column <= k_base_count; ++column) {
            matrix.at(k_base_count, column) = negative_ambiguity;
        }

        for (auto& score : matrix.simd_scores) {
            score = INT8_MIN;
        }
        for (int row = 0; row < k_dimension; ++row) {
            for (int column = 0; column < k_dimension; ++column) {
                matrix.simd_scores[
                    static_cast<std::size_t>(row) * k_simd_columns + column]
                    = matrix.at(row, column);
            }
        }

        return matrix;
    }
};

/**
 * @brief Names scoring storage normalized to a source's logical alphabet.
 *
 * @tparam p_source Logical alphabet or compatible compact-sequence source.
 * @tparam p_alphabet_dimension Logical alphabet dimension.
 */
template <
    supports_compact_sequence_range p_source,
    std::size_t p_alphabet_dimension =
        compact_sequence_range<p_source>::traits::alphabet_size_n>
using scoring_matrix = scoring_matrix_storage<
    typename compact_sequence_range<p_source>::traits::alphabet_type,
    p_alphabet_dimension>;

static_assert(std::is_same_v<
    scoring_matrix<seq::dna4>,
    scoring_matrix<sequence_custom_alphabet<seq::dna4, std::uint64_t>>>);
static_assert(scoring_matrix<seq::dna4>::k_alphabet_dimension == 5u);
static_assert(scoring_matrix<seq::dna5>::k_alphabet_dimension == 6u);

}  // namespace bioseq::simd

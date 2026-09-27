/**
 * @file utils.cppm
 * @brief Substitution scores shared by scalar and SIMD alignment code.
 */
module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

export module bioseq.alignment.utils;

import bioseq.common.seqan_adapter;
import bioseq.sequences;

export namespace bioseq::simd {

/**
 * @brief Stores substitution scores and padded copies for SIMD lookup.
 *
 * @tparam p_alphabet Sequence alphabet.
 * @tparam p_alphabet_dimension Number of ranks, including the wildcard.
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

    /** @brief Substitution scores indexed by row and column rank. */
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

    /** @brief Score rows padded for byte-table SIMD lookup. */
    alignas(64) std::array<
        std::int8_t,
        k_simd_rows * k_simd_columns> simd_scores{};
    static_assert(
        sizeof(decltype(simd_scores))
            == k_simd_rows * k_simd_columns * sizeof(std::int8_t),
        "SIMD scoring storage must not add element overhead");

    /**
     * @brief Reads the score for a pair of ranks.
     * @pre Ranks must be nonnegative and below dimension. No bounds check is performed.
     */
    [[nodiscard]] constexpr std::int8_t at(
        int row,
        int column) const noexcept {
        return entries[
            static_cast<std::size_t>(row) * p_alphabet_dimension + column];
    }

    /**
     * @brief Returns a mutable score for a pair of ranks.
     * @pre Ranks must be nonnegative and below dimension. Update simd_scores separately when needed.
     */
    [[nodiscard]] constexpr std::int8_t& at(
        int row,
        int column) noexcept {
        return entries[
            static_cast<std::size_t>(row) * p_alphabet_dimension + column];
    }

    /**
     * @brief Returns the first diagonal score, used as the match score.
     */
    [[nodiscard]] constexpr std::int8_t match_score() const noexcept {
        return entries[0];
    }

    /**
     * @brief Returns the first off-diagonal score, used as the mismatch score.
     */
    [[nodiscard]] constexpr std::int8_t mismatch_score() const noexcept {
        return entries[1];
    }

    /**
     * @brief Returns the score for aligning two wildcard ranks.
     */
    [[nodiscard]] constexpr std::int8_t wildcard_score() const noexcept {
        return entries[k_entry_count - 1];
    }

    /**
     * @brief Returns a padded SIMD lookup row.
     * @param[in] row Rank to look up, including the extra sentinel rank.
     * @return Pointer into this matrix's aligned score storage.
     */
    [[nodiscard]] const std::int8_t* simd_row_pointer(
        std::size_t row) const noexcept {
        return simd_scores.data() + row * k_simd_columns;
    }

    /**
     * @brief Checks that SIMD scores match entries and all padding is INT8_MIN.
     */
    // Each Highway translation unit has a different ISA. Keep this small
    // imported template predicate in its caller so the linker cannot coalesce
    // an AVX-512 definition into an AVX2 call site with the same weak symbol.
    [[nodiscard, gnu::always_inline]] bool is_block_layout_valid() const noexcept {
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
                if (simd_scores[row * k_simd_columns + column]
                    != std::numeric_limits<std::int8_t>::min()) {
                    return false;
                }
            }
        }
        for (std::size_t column = 0; column < k_simd_columns; ++column) {
            if (simd_scores[p_alphabet_dimension * k_simd_columns + column]
                != std::numeric_limits<std::int8_t>::min()) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief Hashes the matrix dimension and scores to identify a scoring setup.
     *
     * Block Aligner uses this value when checking a prepared execution plan.
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
     * @brief Builds scalar and SIMD substitution tables from match and penalty values.
     *
     * Stores the match magnitude as a reward and mismatch and wildcard values
     * as nonpositive penalties. Any pair containing the final wildcard rank
     * receives the wildcard penalty. SIMD padding is INT8_MIN.
     *
     * @param[in] match Match score in either sign convention. Its magnitude must fit in int8_t.
     * @param[in] mismatch_penalty Mismatch penalty in either sign convention.
     * @param[in] ambiguity_penalty Wildcard penalty in either sign convention.
     * @return Matrix with matching scalar and SIMD score tables.
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
            score = std::numeric_limits<std::int8_t>::min();
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
 * @brief Selects score storage from an alphabet or compact-sequence source.
 *
 * @tparam p_source Alphabet or compatible compact-sequence source.
 * @tparam p_alphabet_dimension Number of matrix ranks.
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

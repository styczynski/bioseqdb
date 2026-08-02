/**
 * @file utils.cppm
 * @brief Shared alignment utilities: unified scoring matrix.
 *
 * Provides ScoringMatrix<Source> — a single scoring-matrix type that replaces
 * the old KswScoringMatrix (used by KSW2) and BlockSIMDMatrix (used by
 * the block aligner). The matrix dimension is deduced from the sequence
 * logical alphabet alone, independent of packed width or representation.
 * Logical alphabets and packed sequence brands canonicalize to the same type.
 */
module;

#include "common.h"

#include <cstdint>
#include <cstring>
#include <array>
#include <climits>
#include <limits>
#include <type_traits>

export module bioseq.alignment.utils;

import bioseq.common.sequan_compat;
import bioseq.sequences;

export namespace bioseq::simd {

// ============================================================================
// ScoringMatrix — unified scoring matrix for KSW2 and block aligner
// ============================================================================

/**
 * @brief Canonical scoring-matrix storage templated on the logical alphabet.
 *
 * Replaces both KswScoringMatrix<AlphabetDim> (used by KSW2) and
 * BlockSIMDMatrix<AlphabetDim> (used by the block aligner) with a single
 * type.
 *
 * Core entries: AlphabetDim x AlphabetDim array of int8_t scores, used by
 * KSW2 for match/mismatch/wildcard lookups.
 *
 * SIMD-aligned rows: (AlphabetDim+1) x 16 layout with the extra row filled
 * with INT8_MIN for NULL/padding, used by the block aligner's PSHUFB
 * score lookup.
 *
 * @tparam Alphabet    A Bioseq/SeqAn logical alphabet type.
 * @tparam AlphabetDim Alphabet dimension (including wildcard), defaults to
 *                     the alphabet size plus the existing wildcard rank.
 */
template<sequence_alphabet Alphabet, size_t AlphabetDim>
struct ScoringMatrixStorage {
    using alphabet_type = Alphabet;
    static constexpr size_t alphabet_dim = AlphabetDim;
    static_assert(AlphabetDim > 1, "Requires alphabet dimension > 1.");

    static constexpr size_t entry_count =
        static_cast<size_t>(AlphabetDim) * AlphabetDim;
    static constexpr int dim = static_cast<int>(AlphabetDim);

    // --- Core scoring entries (KSW2 layout) ---
    std::array<int8_t, entry_count> entries{};

    // --- SIMD-aligned rows (block-aligner layout) ---
    static constexpr size_t SIMD_ROWS = AlphabetDim + 1;
    // The upstream Block Aligner uses one 16-byte shuffle table for DNA and
    // two for amino-acid alphabets.  Keep that rule generic: one table per
    // 16 rank values, including the dedicated NULL/padding rank.  A fixed
    // 16-byte row silently aliased row N's high-rank scores into row N+1 for
    // aa20/aa27 and made ranks >= 16 select unrelated bytes.
    static constexpr size_t SIMD_LUT_COUNT = (AlphabetDim + 1 + 15) / 16;
    static constexpr size_t SIMD_COLS = SIMD_LUT_COUNT * 16;
    static constexpr uint8_t NULL_BYTE = static_cast<uint8_t>(AlphabetDim);

    static_assert(AlphabetDim < std::numeric_limits<std::uint8_t>::max(),
                  "ScoringMatrix rank plus NULL sentinel must fit in uint8_t");

    alignas(64) int8_t simd_scores[SIMD_ROWS * SIMD_COLS] = {};

    // --- Core access (replaces KswScoringMatrix API) ---

    [[nodiscard]] constexpr int8_t at(int row, int col) const noexcept {
        return entries[static_cast<size_t>(row) * AlphabetDim + col];
    }
    [[nodiscard]] constexpr int8_t& at(int row, int col) noexcept {
        return entries[static_cast<size_t>(row) * AlphabetDim + col];
    }
    [[nodiscard]] constexpr int8_t match_score()    const noexcept {
        return entries[0];
    }
    [[nodiscard]] constexpr int8_t mismatch_score()  const noexcept {
        return entries[1];
    }
    [[nodiscard]] constexpr int8_t wildcard_score()  const noexcept {
        return entries[entry_count - 1];
    }

    // --- SIMD access (replaces BlockSIMDMatrix API) ---

    [[nodiscard]] const int8_t* simd_row_ptr(size_t i) const noexcept {
        return simd_scores + i * SIMD_COLS;
    }

    /// Validate that the SIMD lookup bank is an exact projection of the
    /// biological score matrix, including NULL/padding sentinels. Block
    /// Aligner attestation calls this once per admitted alignment, never in
    /// the DP loop.
    [[nodiscard]] bool block_layout_valid() const noexcept {
        for (size_t row = 0; row < AlphabetDim; ++row) {
            for (size_t col = 0; col < AlphabetDim; ++col) {
                if (simd_scores[row * SIMD_COLS + col]
                    != entries[row * AlphabetDim + col]) {
                    return false;
                }
            }
            for (size_t col = AlphabetDim; col < SIMD_COLS; ++col) {
                if (simd_scores[row * SIMD_COLS + col] != INT8_MIN) {
                    return false;
                }
            }
        }
        for (size_t col = 0; col < SIMD_COLS; ++col) {
            if (simd_scores[AlphabetDim * SIMD_COLS + col] != INT8_MIN) {
                return false;
            }
        }
        return true;
    }

    /// Stable identity of every score consumed by the Block kernel. This
    /// binds proof evidence to match/mismatch/ambiguity semantics instead of
    /// merely to gap and block-size parameters.
    [[nodiscard]] std::uint64_t block_semantic_identity() const noexcept {
        std::uint64_t hash = 1469598103934665603ULL;
        const auto mix = [&hash](std::uint64_t value) noexcept {
            hash ^= value;
            hash *= 1099511628211ULL;
        };
        mix(AlphabetDim);
        for (const int8_t score : entries) {
            mix(static_cast<std::uint8_t>(score));
        }
        return hash;
    }

    // --- Factory ---

    /**
     * @brief Generate a scoring matrix from match/mismatch/ambiguity params.
     *
     * Creates an AlphabetDim x AlphabetDim matrix where:
     *   - Diagonal (i==j, i < AlphabetDim-1) = +match_score
     *   - Off-diagonal                        = -mismatch_pen
     *   - Last row/col (wildcard)             = -sc_ambi
     *
     * Also fills the SIMD-aligned rows for block-aligner use.
     *
     * @param match_score  Score for matching bases (positive value)
     * @param mismatch_pen Penalty for mismatches  (positive value, negated)
     * @param sc_ambi      Penalty for ambiguous   (positive value, negated)
     */
    static ScoringMatrixStorage generate(
        int8_t match_score, int8_t mismatch_pen, int8_t sc_ambi)
    {
        ScoringMatrixStorage matrix{};
        constexpr int base_count = dim - 1;

        const int8_t pos_match =
            match_score > 0 ? match_score
                            : static_cast<int8_t>(-match_score);
        const int8_t neg_mismatch =
            mismatch_pen > 0 ? static_cast<int8_t>(-mismatch_pen)
                             : mismatch_pen;
        const int8_t ambi_penalty =
            sc_ambi > 0 ? static_cast<int8_t>(-sc_ambi) : sc_ambi;

        // Fill core entries
        for (int r = 0; r < base_count; ++r) {
            for (int c = 0; c < base_count; ++c) {
                matrix.at(r, c) = (r == c) ? pos_match : neg_mismatch;
            }
            matrix.at(r, base_count) = ambi_penalty;
        }
        for (int c = 0; c <= base_count; ++c) {
            matrix.at(base_count, c) = ambi_penalty;
        }

        // Build SIMD-aligned rows: AlphabetDim data rows + 1 NULL row
        for (auto& s : matrix.simd_scores) s = INT8_MIN;
        for (int i = 0; i < dim; ++i) {
            for (int j = 0; j < dim; ++j) {
                matrix.simd_scores[static_cast<size_t>(i) * SIMD_COLS + j] =
                    matrix.at(i, j);
            }
        }

        return matrix;
    }
};

/**
 * @brief Unified scoring matrix normalized to a source's logical alphabet.
 *
 * `Source` may be the logical alphabet used by the canonical lazy-view APIs or
 * any established compact-sequence brand. Both spellings resolve through the
 * same `compact_sequence_range<Source>::traits` contract and therefore denote
 * the same matrix type for a given alphabet and dimension.
 */
template<supports_compact_sequence_range Source,
         size_t AlphabetDim =
             compact_sequence_range<Source>::traits::alphabet_size_n>
using ScoringMatrix = ScoringMatrixStorage<
    typename compact_sequence_range<Source>::traits::alphabet_type,
    AlphabetDim>;

static_assert(std::is_same_v<
    ScoringMatrix<seq::dna4>,
    ScoringMatrix<sequence_custom_alphabet<seq::dna4, std::uint64_t>>>);
static_assert(ScoringMatrix<seq::dna4>::alphabet_dim == 5u);
static_assert(ScoringMatrix<seq::dna5>::alphabet_dim == 6u);

}  // namespace bioseq::simd

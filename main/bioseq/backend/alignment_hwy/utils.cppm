/**
 * @file utils.cppm
 * @brief Shared alignment utilities: unified scoring matrix.
 *
 * Provides ScoringMatrix<R> — a single scoring-matrix type that replaces
 * the old KswScoringMatrix (used by KSW2) and BlockSIMDMatrix (used by
 * the block aligner). The matrix dimension is deduced from the sequence
 * range's alphabet traits.
 */
module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include "common.h"

#include <cstdint>
#include <cstring>
#include <array>
#include <climits>

export module alignment_hwy_utils;

import bioseq_sequences;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// ============================================================================
// ScoringMatrix — unified scoring matrix for KSW2 and block aligner
// ============================================================================

/**
 * @brief Unified scoring matrix templated on a compact_sequence_range type.
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
 * @tparam R           A type satisfying supports_compact_sequence_range.
 * @tparam AlphabetDim Alphabet dimension (including wildcard), defaults to
 *                     the range's alphabet_size_n trait.
 */
template<supports_compact_sequence_range R,
         size_t AlphabetDim = compact_sequence_range<R>::traits::alphabet_size_n>
struct ScoringMatrix {
    static_assert(AlphabetDim > 1, "Requires alphabet dimension > 1.");

    static constexpr size_t entry_count =
        static_cast<size_t>(AlphabetDim) * AlphabetDim;
    static constexpr int dim = static_cast<int>(AlphabetDim);

    // --- Core scoring entries (KSW2 layout) ---
    std::array<int8_t, entry_count> entries{};

    // --- SIMD-aligned rows (block-aligner layout) ---
    static constexpr size_t SIMD_ROWS = AlphabetDim + 1;
    static constexpr size_t SIMD_COLS = 16;
    static constexpr uint8_t NULL_BYTE = static_cast<uint8_t>(AlphabetDim);

    HWY_ALIGN int8_t simd_scores[SIMD_ROWS * SIMD_COLS] = {};

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

    /// Score lookup: for reference char c and L query chars at query_ptr,
    /// return L i16 scores. Uses scalar loop (PSHUFB optimisation TODO).
    template<class D>
    HWY_INLINE hn::Vec<D> get_scores(
        D d, uint8_t c, const uint8_t* query_ptr) const
    {
        const size_t L = hn::Lanes(d);
        const int8_t* row = simd_row_ptr(c);
        HWY_ALIGN int16_t result[hn::MaxLanes(d)];
        for (size_t i = 0; i < L; ++i) {
            result[i] = static_cast<int16_t>(row[query_ptr[i]]);
        }
        return hn::Load(d, result);
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
    static ScoringMatrix generate(
        int8_t match_score, int8_t mismatch_pen, int8_t sc_ambi)
    {
        ScoringMatrix matrix{};
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

}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

}  // export

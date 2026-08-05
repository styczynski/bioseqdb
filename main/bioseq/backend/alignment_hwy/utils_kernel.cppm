/**
 * @file utils_kernel.cppm
 * @brief Canonical target-sensitive score lookup helpers for alignment.
 */
module;

#include <hwy/highway.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

export module bioseq.highway.sources.alignment_utils;

import std;
import bioseq.alignment.utils;
import bioseq.sequences;

export namespace bioseq::simd {

namespace [[bioseq::simd]] target {

namespace hn = hwy::HWY_NAMESPACE;

template<sequence_alphabet Alphabet, class D>
HWY_INLINE auto load_score_lut(
    const ScoringMatrix<Alphabet>& matrix,
    D d,
    std::uint8_t rank)
{
    static_assert(std::is_same_v<hn::TFromD<D>, std::int16_t>,
                  "load_score_lut expects an int16_t SIMD tag");
    const hn::Rebind<std::uint8_t, D> du8;
    const hn::Twice<decltype(du8)> du8_twice;
    using lut_vec_t = decltype(hn::LoadDup128(
        du8_twice,
        reinterpret_cast<const std::uint8_t*>(matrix.simd_row_ptr(rank))));
    std::array<lut_vec_t, ScoringMatrix<Alphabet>::SIMD_LUT_COUNT> result{};
    const auto* row = reinterpret_cast<const std::uint8_t*>(
        matrix.simd_row_ptr(rank));
    for (std::size_t lut_index = 0;
         lut_index < ScoringMatrix<Alphabet>::SIMD_LUT_COUNT;
         ++lut_index) {
        result[lut_index] = hn::LoadDup128(
            du8_twice, row + lut_index * 16);
    }
    return result;
}

template<sequence_alphabet Alphabet, class D, class LUT>
HWY_INLINE hn::Vec<D> get_scores_with_lut(
    const ScoringMatrix<Alphabet>&,
    D d,
    const LUT& lut,
    const std::uint8_t* query_ptr)
{
    static_assert(std::is_same_v<hn::TFromD<D>, std::int16_t>,
                  "get_scores_with_lut expects an int16_t SIMD tag");
    const hn::Rebind<std::uint8_t, D> du8;
    const auto query_vec = hn::LoadU(du8, query_ptr);
    auto scores_u8 = hn::TableLookupBytes(lut[0], query_vec);
    if constexpr (ScoringMatrix<Alphabet>::SIMD_LUT_COUNT > 1) {
        const auto rank_in_lut = hn::And(
            query_vec, hn::Set(du8, static_cast<std::uint8_t>(0x0f)));
        const auto rank_lut_index = hn::ShiftRight<4>(query_vec);
        scores_u8 = hn::Set(du8, static_cast<std::uint8_t>(0x80));
        for (std::size_t lut_index = 0;
             lut_index < ScoringMatrix<Alphabet>::SIMD_LUT_COUNT;
             ++lut_index) {
            const auto candidate = hn::TableLookupBytes(
                lut[lut_index], rank_in_lut);
            const auto selected = hn::Eq(
                rank_lut_index,
                hn::Set(du8, static_cast<std::uint8_t>(lut_index)));
            scores_u8 = hn::IfThenElse(selected, candidate, scores_u8);
        }
    }
    const hn::Rebind<std::int8_t, D> di8;
    return hn::PromoteTo(d, hn::BitCast(di8, scores_u8));
}

template<sequence_alphabet Alphabet, class D>
HWY_INLINE hn::Vec<D> get_scores(
    const ScoringMatrix<Alphabet>& matrix,
    D d,
    std::uint8_t rank,
    const std::uint8_t* query_ptr)
{
    return get_scores_with_lut<Alphabet>(
        matrix, d, load_score_lut<Alphabet>(matrix, d, rank), query_ptr);
}

}  // namespace target
}  // namespace bioseq::simd

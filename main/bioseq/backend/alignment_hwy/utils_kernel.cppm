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
    const auto* row = reinterpret_cast<const std::uint8_t*>(
        matrix.simd_row_ptr(rank));
    if constexpr (ScoringMatrix<Alphabet>::SIMD_LUT_COUNT == 1) {
        return hn::LoadDup128(du8_twice, row);
    } else {
        static_assert(
            ScoringMatrix<Alphabet>::SIMD_LUT_COUNT == 2,
            "alignment score LUT supports at most two 16-byte tables");
        // Vec2 is a native sizeless tuple on SVE/RVV and a small aggregate on
        // fixed-width targets. Unlike std::array<Vec>, it is legal for every
        // attainable Highway target.
        return hn::Create2(
            du8_twice,
            hn::LoadDup128(du8_twice, row),
            hn::LoadDup128(du8_twice, row + 16));
    }
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
    if constexpr (ScoringMatrix<Alphabet>::SIMD_LUT_COUNT == 1) {
        const auto scores_u8 = hn::TableLookupBytes(lut, query_vec);
        const hn::Rebind<std::int8_t, D> di8;
        return hn::PromoteTo(d, hn::BitCast(di8, scores_u8));
    } else {
        static_assert(
            ScoringMatrix<Alphabet>::SIMD_LUT_COUNT == 2,
            "alignment score LUT supports at most two 16-byte tables");
        const auto rank_in_lut = hn::And(
            query_vec, hn::Set(du8, static_cast<std::uint8_t>(0x0f)));
        const auto rank_lut_index = hn::ShiftRight<4>(query_vec);
        const auto lower = hn::TableLookupBytes(
            hn::Get2<0>(lut), rank_in_lut);
        const auto upper = hn::TableLookupBytes(
            hn::Get2<1>(lut), rank_in_lut);
        const auto scores_u8 = hn::IfThenElse(
            hn::Eq(
                rank_lut_index,
                hn::Set(du8, static_cast<std::uint8_t>(0))),
            lower,
            hn::IfThenElse(
                hn::Eq(
                    rank_lut_index,
                    hn::Set(du8, static_cast<std::uint8_t>(1))),
                upper,
                hn::Set(du8, static_cast<std::uint8_t>(0x80))));
        const hn::Rebind<std::int8_t, D> di8;
        return hn::PromoteTo(d, hn::BitCast(di8, scores_u8));
    }
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

/**
 * @file utils_kernel.cppm
 * @brief Target-specific score lookup helpers for alignment kernels.
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

/**
 * @brief Loads SIMD shuffle tables for one scoring-matrix row.
 * @tparam p_alphabet Logical sequence alphabet.
 * @tparam p_simd_tag Highway descriptor for signed 16-bit score lanes.
 * @param[in] matrix Canonical score matrix.
 * @param[in] simd_tag Active score descriptor.
 * @param[in] rank Zero-based score-matrix row.
 * @return Fixed-size array of target-native byte lookup vectors.
 */
template <sequence_alphabet p_alphabet, class p_simd_tag>
HWY_INLINE auto load_score_lookup_tables(
    const scoring_matrix<p_alphabet>& matrix,
    p_simd_tag simd_tag,
    std::uint8_t rank) {
    static_assert(
        std::is_same_v<hn::TFromD<p_simd_tag>, std::int16_t>,
        "Score lookup requires an int16 SIMD tag");
    const hn::Rebind<std::uint8_t, p_simd_tag> byte_tag;
    const hn::Twice<decltype(byte_tag)> doubled_byte_tag;
    using lookup_vector = decltype(hn::LoadDup128(
        doubled_byte_tag,
        reinterpret_cast<const std::uint8_t*>(
            matrix.simd_row_pointer(rank))));
    std::array<
        lookup_vector,
        scoring_matrix<p_alphabet>::k_simd_lookup_count>
        lookup_tables{};
    const auto* row = reinterpret_cast<const std::uint8_t*>(
        matrix.simd_row_pointer(rank));
    for (std::size_t lookup_index = 0;
         lookup_index
            < scoring_matrix<p_alphabet>::k_simd_lookup_count;
         ++lookup_index) {
        lookup_tables[lookup_index] = hn::LoadDup128(
            doubled_byte_tag,
            row + lookup_index * 16);
    }
    return lookup_tables;
}

/**
 * @brief Looks up per-lane scores from already loaded shuffle tables.
 * @tparam p_alphabet Logical sequence alphabet.
 * @tparam p_simd_tag Highway descriptor for signed 16-bit score lanes.
 * @tparam p_lookup_tables Fixed lookup-table collection type.
 * @param[in] matrix Matrix whose alphabet determines lookup semantics.
 * @param[in] simd_tag Active score descriptor.
 * @param[in] lookup_tables Loaded byte shuffle tables.
 * @param[in] query_pointer Readable query ranks for every active lane.
 * @return Signed 16-bit score vector for the query ranks.
 */
template <
    sequence_alphabet p_alphabet,
    class p_simd_tag,
    class p_lookup_tables>
HWY_INLINE hn::Vec<p_simd_tag> get_scores_with_lookup_tables(
    [[maybe_unused]] const scoring_matrix<p_alphabet>& matrix,
    p_simd_tag simd_tag,
    const p_lookup_tables& lookup_tables,
    const std::uint8_t* query_pointer) {
    static_assert(
        std::is_same_v<hn::TFromD<p_simd_tag>, std::int16_t>,
        "Score lookup requires an int16 SIMD tag");
    const hn::Rebind<std::uint8_t, p_simd_tag> byte_tag;
    const auto query_ranks = hn::LoadU(byte_tag, query_pointer);
    auto unsigned_scores =
        hn::TableLookupBytes(lookup_tables[0], query_ranks);
    if constexpr (scoring_matrix<p_alphabet>::k_simd_lookup_count > 1) {
        const auto rank_within_table = hn::And(
            query_ranks,
            hn::Set(byte_tag, static_cast<std::uint8_t>(0x0f)));
        const auto table_index_for_rank = hn::ShiftRight<4>(query_ranks);
        unsigned_scores =
            hn::Set(byte_tag, static_cast<std::uint8_t>(0x80));
        for (std::size_t lookup_index = 0;
             lookup_index
                < scoring_matrix<p_alphabet>::k_simd_lookup_count;
             ++lookup_index) {
            const auto candidate = hn::TableLookupBytes(
                lookup_tables[lookup_index],
                rank_within_table);
            const auto is_selected_table = hn::Eq(
                table_index_for_rank,
                hn::Set(
                    byte_tag,
                    static_cast<std::uint8_t>(lookup_index)));
            unsigned_scores = hn::IfThenElse(
                is_selected_table,
                candidate,
                unsigned_scores);
        }
    }
    const hn::Rebind<std::int8_t, p_simd_tag> signed_byte_tag;
    return hn::PromoteTo(
        simd_tag,
        hn::BitCast(signed_byte_tag, unsigned_scores));
}

/**
 * @brief Loads one scoring row and returns scores for the query ranks.
 * @tparam p_alphabet Logical sequence alphabet.
 * @tparam p_simd_tag Highway descriptor for signed 16-bit score lanes.
 * @param[in] matrix Canonical score matrix.
 * @param[in] simd_tag Active score descriptor.
 * @param[in] rank Zero-based score-matrix row.
 * @param[in] query_pointer Readable query ranks for every active lane.
 * @return Signed 16-bit score vector for the query ranks.
 */
template <sequence_alphabet p_alphabet, class p_simd_tag>
HWY_INLINE hn::Vec<p_simd_tag> get_scores(
    const scoring_matrix<p_alphabet>& matrix,
    p_simd_tag simd_tag,
    std::uint8_t rank,
    const std::uint8_t* query_pointer) {
    return get_scores_with_lookup_tables<p_alphabet>(
        matrix,
        simd_tag,
        load_score_lookup_tables<p_alphabet>(matrix, simd_tag, rank),
        query_pointer);
}

}  // namespace target
}  // namespace bioseq::simd

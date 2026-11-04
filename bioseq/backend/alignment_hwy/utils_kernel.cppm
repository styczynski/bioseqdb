/**
 * @file utils_kernel.cppm
 * @brief SIMD table lookups for alignment substitution scores.
 */
module;

#include <hwy/highway.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

export module bioseq.highway.sources.alignment_utils;

import std;
import bioseq.common.seqan_adapter;
import bioseq.alignment.utils;
import bioseq.sequences;

export namespace bioseq::simd {

namespace [[bioseq::simd]] target {

namespace hn = hwy::HWY_NAMESPACE;

/**
 * Decode the selected oriented interval directly into its final DP storage.
 * Reuse the backend's write-order callback; validate native ranks before any
 * matrix lookup, fusing validation with copying when the backend borrows bytes.
 */
template<bool QueryReversed, bool WriteReversed, class Sequence,
         class D = hn::ScalableTag<std::uint8_t>>
[[nodiscard]] HWY_INLINE std::expected<void, alignment_contract_error> copy_checked_alignment_ranks(
    const Sequence& sequence, std::span<std::uint8_t> destination,
    std::size_t begin, std::size_t end, D descriptor = {}) {
    static_assert(std::same_as<hn::TFromD<D>, std::uint8_t>);
    using alphabet_type = typename Sequence::traits::alphabet_type;
    constexpr auto alphabet_size = seqan3::alphabet_size<alphabet_type>;
    if (end < begin || end > sequence.bp_size() || destination.size() != end - begin)
        return std::unexpected(alignment_contract_error::coordinates);
    const auto reader = sequence.template rank_reader<QueryReversed, WriteReversed, std::uint8_t>();
    const auto ranks = reader.read_checked(begin, destination.size(), destination);
    if (!ranks) return std::unexpected(alignment_contract_error::source_read_failed);
    const bool needs_copy = ranks->data() != destination.data();
    const auto lane_count = hn::Lanes(descriptor);
    std::size_t offset = 0;
    for (; destination.size() - offset >= lane_count; offset += lane_count) {
        const auto values = hn::LoadU(descriptor, ranks->data() + offset);
        if constexpr (alphabet_size <= std::numeric_limits<std::uint8_t>::max()) {
            if (!hn::AllTrue(descriptor, hn::Lt(values,
                    hn::Set(descriptor, static_cast<std::uint8_t>(alphabet_size)))))
                return std::unexpected(alignment_contract_error::invalid_matrix_layout);
        }
        if (needs_copy) hn::StoreU(values, descriptor, destination.data() + offset);
    }
    for (; offset < destination.size(); ++offset) {
        const auto rank = (*ranks)[offset];
        if (rank >= alphabet_size)
            return std::unexpected(alignment_contract_error::invalid_matrix_layout);
        if (needs_copy) destination[offset] = rank;
    }
    return {};
}

/**
 * Score independently varying target/query rank pairs in the current DP vector.
 * Each target row uses the same segmented Highway byte tables as Block scoring;
 * ranks beyond the first sixteen select subsequent tables without truncation.
 */
template <sequence_alphabet p_alphabet, class D>
HWY_INLINE hn::Vec<D> get_pair_scores(
    const scoring_matrix<p_alphabet>& matrix, D score_tag,
    hn::Vec<hn::RebindToUnsigned<D>> targets,
    hn::Vec<hn::RebindToUnsigned<D>> queries) {
    static_assert(std::is_same_v<hn::TFromD<D>, std::int8_t>);
    const hn::RebindToUnsigned<D> rank_tag;
    const auto low_indices = hn::And(queries, hn::Set(rank_tag, std::uint8_t{15}));
    const auto table_indices = hn::ShiftRight<4>(queries);
    auto selected = hn::Set(rank_tag, std::uint8_t{128});
    for (std::size_t row = 0; row < scoring_matrix<p_alphabet>::k_simd_rows; ++row) {
        const auto row_mask = hn::Eq(targets, hn::Set(rank_tag, static_cast<std::uint8_t>(row)));
        if (hn::AllFalse(rank_tag, row_mask)) continue;
        const auto* row_pointer = reinterpret_cast<const std::uint8_t*>(matrix.simd_row_pointer(row));
        auto row_scores = hn::Set(rank_tag, std::uint8_t{128});
        for (std::size_t table = 0; table < scoring_matrix<p_alphabet>::k_simd_lookup_count; ++table) {
            const auto lookup = hn::LoadDup128(rank_tag, row_pointer + table * 16);
            const auto scores = hn::TableLookupBytes(lookup, low_indices);
            row_scores = hn::IfThenElse(hn::Eq(table_indices,
                hn::Set(rank_tag, static_cast<std::uint8_t>(table))), scores, row_scores);
        }
        selected = hn::IfThenElse(row_mask, row_scores, selected);
    }
    return hn::BitCast(score_tag, selected);
}

/**
 * @brief Loads one score row into SIMD byte-lookup tables.
 *
 * The tables can be reused when aligning several query vectors against
 * the same reference rank. Scores are prepared  for signed 16-bit lanes.
 *
 * @param[in] matrix Score matrix with SIMD padding.
 * @param[in] simd_tag Highway tag for signed 16-bit scores.
 * @param[in] rank Matrix row for the reference symbol.
 * @return One SIMD lookup vector per 16 entries in the padded row.
 */
template <sequence_alphabet p_alphabet, class p_simd_tag>
HWY_INLINE auto load_score_lookup_tables(
    const scoring_matrix<p_alphabet>& matrix,
    p_simd_tag simd_tag,
    std::uint8_t rank, bool transposed = false) {
    static_assert(
        std::is_same_v<hn::TFromD<p_simd_tag>, std::int16_t>,
        "Score lookup requires an int16 SIMD tag");
    const hn::Rebind<std::uint8_t, p_simd_tag> byte_tag;
    const hn::Twice<decltype(byte_tag)> doubled_byte_tag;
    using lookup_vector = decltype(hn::LoadDup128(
        doubled_byte_tag,
        reinterpret_cast<const std::uint8_t*>(
            matrix.simd_row_pointer(rank, transposed))));
    std::array<
        lookup_vector,
        scoring_matrix<p_alphabet>::k_simd_lookup_count>
        lookup_tables{};
    const auto* row = reinterpret_cast<const std::uint8_t*>(
        matrix.simd_row_pointer(rank, transposed));
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
 * @brief Scores query ranks using a previously loaded matrix row.
 *
 * Each lane looks up its rank and expands the signed byte score to 16 bits.
 *
 * @param[in] matrix Supplies the alphabrt and table dimensions.
 * @param[in] simd_tag Highway tag for signed 16-bit scores.
 * @param[in] lookup_tables Tables returned by load_score_lookup_tables.
 * @param[in] query_pointer Query ranks readable for every active lane.
 * @return One signed 16-bit substitution score per query rank.
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
 * @brief Returns SIMD scores for query ranks against  one reference rank.
 *
 * Loads the matrix row, then looks up each query rank.
 *
 * @param[in] matrix Score matrix with SIMD padding
 * @param[in] simd_tag Highway tag for signed 16-bit scores.
 * @param[in] rank Matrix row for the reference symbol.
 * @param[in] query_pointer Query ranks readable for every active lane.
 * @return One signed 16-bit substitution score per query rank.
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

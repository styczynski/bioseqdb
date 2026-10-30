/**
 * @file block_aligner_certificates.cppm
 * @brief Skip dynamic programming when a sequence  scan proves the diagonal is the est alignment
 */
module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <array>
#include <algorithm>
#include <bit>
#include <limits>
#include <type_traits>
#include <concepts>
#include <tuple>
#include <string>
#include <string_view>
#include <utility>

export module bioseq.highway.sources.block_aligner_certificates;

import std;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.alignment.contracts;
import bioseq.highway.sources.alignment_utils;
import bioseq.highway.sources.block_aligner_dp;
import bioseq.highway.sources.simd_utils;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.simd.utils;

export namespace bioseq::simd {

namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

/**
 * @brief Check that matching and mismatched symbols have uniform scores.
 *
 * Matching symbols must share a nonnegative score. All mismatches share
 * another score, and the padding row and column have one constant score.
 * Every entry must be at most the match score for the diagonal bound to hold.
 *
 * @return Whether these conditions hold. The output scores are valid on success.
 */
template<sequence_alphabet p_alphabet,
         size_t p_alphabet_dimension = seqan3::alphabet_size<p_alphabet> + 1u>
[[nodiscard]] bool has_generated_diagonal_score_layout(
        const scoring_matrix<p_alphabet>& scoring_matrix,
        std::int8_t& match_score,
        std::int8_t& mismatch_score) noexcept {
    const auto& summary = scoring_matrix.summary();
    match_score = static_cast<std::int8_t>(summary.equal_score);
    mismatch_score = static_cast<std::int8_t>(summary.unequal_score);
    return scoring_matrix.is_block_layout_valid()
        && scoring_matrix.kind() == substitution_kind::uniform
        && summary.equal_score >= 0 && summary.maximum <= summary.equal_score;
}

/**
 * @brief Prove that the diagonal is the unique best alignment.
 *
 * For equal-length intervals, any gapped alignment pays for an insertion and
 * a deletion and has at least one fewer aligned pair, which bounds its score.
 * The diagonal must beat that bound. Ties need dynamic programming.
 * Intervals are half-open. Query coordinates follow the selected strand.
 *
 * @return The score, mismatch count and scan counters, or an uncetified result.
 */
template<
    bool p_is_reversed,
    sequence_alphabet p_alphabet,
    size_t p_alphabet_dimension>
[[nodiscard]] HWY_INLINE block_diagonal_certificate
try_global_diagonal_certificate(
        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
        size_t query_start,
        size_t query_end,
        size_t target_start,
        size_t target_end,
        const scoring_matrix<p_alphabet>& scoring_matrix,
        const block_aligner_parameters& parameters) {
    block_diagonal_certificate result;
    const bool are_coordinates_ordered = query_end >= query_start
        && target_end >= target_start;
    const size_t sequence_length = are_coordinates_ordered ? query_end - query_start : 0u;
    const size_t target_length = are_coordinates_ordered
        ? target_end - target_start : 0u;
    if (parameters.mode != block_alignment_mode::global
        || parameters.stage != block_alignment_stage::gap_extension
        || parameters.input_order != block_input_order::forward
        || parameters.gap_open < 0 || parameters.gap_extend <= 0
        || !are_coordinates_ordered || sequence_length == 0 || target_length != sequence_length
        || query_end > query_seq.bp_size()
        || target_end > target_seq.bp_size()
        || sequence_length > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        return result;
    }
    constexpr size_t packed_cigar_limit = static_cast<size_t>(
        std::numeric_limits<std::uint32_t>::max() >> 4u);
    if (parameters.is_trace_enabled && sequence_length > packed_cigar_limit) return result;

    // This scan scores only matches and mismatches. Check that every
    // matrix  entry agrees with those two scores and the padding rules.
    if constexpr (p_alphabet_dimension < 3) return result;
    constexpr size_t biological_ranks = p_alphabet_dimension - 1;
    std::int8_t match_score = 0;
    std::int8_t mismatch_score = 0;
    if (!has_generated_diagonal_score_layout<p_alphabet, p_alphabet_dimension>(
            scoring_matrix, match_score, mismatch_score)) {
        return result;
    }

    // Scalar-only chunk dispatch keeps vector instructions inside this target.
    const auto equality = bioseq::simd::count_equal_sequence_symbols<
        p_is_reversed, bioseq::simd::sequence_rank_validation::upper_bound>(
            query_seq, target_seq, query_start, target_start,
            sequence_length, &count_equal_rank_chunk, biological_ranks);
    if (!equality.valid_ranks) return block_diagonal_certificate{};
    const std::uint64_t equal_symbols = equality.equal_symbols;
    result.vector_batches = equality.vector_batches;

    const std::uint64_t unequal_symbols = static_cast<std::uint64_t>(sequence_length)
        - equal_symbols;
    const int64_t diagonal_score =
        static_cast<int64_t>(equal_symbols) * match_score
        + static_cast<int64_t>(unequal_symbols) * mismatch_score;
    const int64_t alternative_upper_bound =
        static_cast<int64_t>(sequence_length - 1) * match_score
        - 2 * (static_cast<int64_t>(parameters.gap_open)
            + static_cast<int64_t>(parameters.gap_extend));
    if (diagonal_score <= alternative_upper_bound
        || diagonal_score < std::numeric_limits<int32_t>::min()
        || diagonal_score > std::numeric_limits<int32_t>::max()) {
        return block_diagonal_certificate{};
    }

    result.is_certified = true;
    result.score = static_cast<int32_t>(diagonal_score);
    result.edit_distance = unequal_symbols;
    result.symbols = sequence_length;
    return result;
}

/**
 * @brief Check the  diagonal directly in resident packed symbols storage.
 *
 * Count mismatches in packed words and apply the strict score bound from the
 * byte-based check. The prepared plan supplies scoring and parameter checks.
 * Intervals are half-open. Query coordinates follow the selected strand.
 *
 * @return not_eligible when packed reading is unavailable, allowing the byte
 * check. rejected means the scan ran and, dynamic programming is needed.
 * certified supplies the diagonal score, mismathh count and scan counters.
 */
template<
    bool p_is_reversed,
    sequence_alphabet p_alphabet>
[[nodiscard]] HWY_INLINE block_packed_certificate_attempt
try_packed_global_gap_certificate(
        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
        size_t query_start,
        size_t query_end,
        size_t target_start,
        size_t target_end,
        const block_global_gap_certificate_plan& plan) noexcept {
    block_packed_certificate_attempt attempt;
    if constexpr (p_is_reversed && !seqan3::nucleotide_alphabet<p_alphabet>) {
        return attempt;
    } else {
        const bool are_coordinates_ordered = query_end >= query_start
            && target_end >= target_start;
        const size_t query_length = are_coordinates_ordered
            ? query_end - query_start : 0u;
        const size_t target_length = are_coordinates_ordered
            ? target_end - target_start : 0u;
        constexpr size_t packed_cigar_limit = static_cast<size_t>(
            std::numeric_limits<std::uint32_t>::max() >> 4u);
        if (!plan.is_contract_supported
            || !plan.is_generated_matrix_supported
            || plan.parameters.mode != block_alignment_mode::global
            || plan.parameters.stage != block_alignment_stage::gap_extension
            || plan.parameters.input_order != block_input_order::forward
            || !are_coordinates_ordered || query_length == 0u
            || target_length != query_length
            || query_end > query_seq.bp_size()
            || target_end > target_seq.bp_size()
            || query_length > static_cast<size_t>(
                std::numeric_limits<std::int32_t>::max())
            || (plan.parameters.is_trace_enabled && query_length > packed_cigar_limit)) {
            return attempt;
        }

        // Borrow each complete sequence view  so reverse-strand coordinates
        // still have the full length. Keep its leading offset because slices
        // can start inside a packed unit.
        const auto query_region = query_seq.try_resident_packed(
            0u, query_seq.bp_size(), 8u);
        const auto target_region = target_seq.try_resident_packed(
            0u, target_seq.bp_size(), 8u);
        if (!query_region.has_value() || !target_region.has_value()
            || query_seq.bp_size()
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<size_t>::max)())
            || target_seq.bp_size()
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<size_t>::max)())) {
            return attempt;
        }
        const auto* query_units =
            static_cast<const std::uint64_t*>(query_region->units);
        const auto* target_units =
            static_cast<const std::uint64_t*>(target_region->units);
        const size_t query_logical_total =
            static_cast<size_t>(query_seq.bp_size());
        const size_t target_logical_total =
            static_cast<size_t>(target_seq.bp_size());
        const size_t query_leading = query_region->leading_symbol_offset;
        const size_t target_leading = target_region->leading_symbol_offset;
        if (query_logical_total
                > (std::numeric_limits<size_t>::max)() - query_leading
            || target_logical_total
                > (std::numeric_limits<size_t>::max)() - target_leading) {
            return attempt;
        }
        const size_t query_physical_total =
            query_leading + query_logical_total;

        using packed_traits = typename compact_sequence_range<
            sequence_custom_alphabet<p_alphabet, std::uint64_t>>::traits;
        constexpr auto complement_mask =
            packed_symbol_complement_mask<packed_traits, p_is_reversed>();
        if constexpr (!complement_mask.has_value()) { return attempt; }
        else {
            const hn::Repartition<std::uint64_t, block_aligner_simd_tag> packed_word_tag;
            const auto equality = count_equal_packed_symbols<packed_traits, p_is_reversed>(
                packed_word_tag, query_units, target_units, query_physical_total,
                p_is_reversed ? query_start : query_leading + query_start,
                target_leading + target_start, query_length, *complement_mask);
            if (!equality.valid_ranks) return attempt;
            attempt.certificate.vector_batches = equality.vector_batches;
            attempt.certificate.edit_distance = query_length - equality.equal_symbols;
        }
        attempt.decision = block_packed_certificate_decision::rejected;
        const std::uint64_t mismatch_symbols = attempt.certificate.edit_distance;
        const std::uint64_t equal_symbols = query_length - mismatch_symbols;
        const std::int64_t diagonal_score =
            static_cast<std::int64_t>(equal_symbols) * plan.match_score
            + static_cast<std::int64_t>(mismatch_symbols)
                * plan.mismatch_score;
        const std::int64_t alternative_upper_bound =
            static_cast<std::int64_t>(query_length - 1u) * plan.match_score
            - 2 * (static_cast<std::int64_t>(plan.parameters.gap_open)
                + static_cast<std::int64_t>(plan.parameters.gap_extend));
        if (diagonal_score <= alternative_upper_bound
            || diagonal_score < std::numeric_limits<std::int32_t>::min()
            || diagonal_score > std::numeric_limits<std::int32_t>::max()) {
            return attempt;
        }

        attempt.decision = block_packed_certificate_decision::certified;
        attempt.certificate.is_certified = true;
        attempt.certificate.score = static_cast<std::int32_t>(diagonal_score);
        attempt.certificate.edit_distance = mismatch_symbols;
        attempt.certificate.symbols = query_length;
        return attempt;
    }
}

}  // namespace target
}  // namespace bioseq::simd

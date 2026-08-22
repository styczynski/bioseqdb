/**
 * @file block_aligner_certificates.cppm
 * @brief Implements exact adaptive-block shortcut certificates.
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
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;

export namespace bioseq::simd {

namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

/**
 * @brief Checks whether a score matrix has the generated diagonal layout.
 * @tparam p_alphabet Logical sequence alphabet.
 * @tparam p_alphabet_dimension Matrix dimension including the sentinel rank.
 * @param[in] scoring_matrix Matrix inspected for uniform biological scores.
 * @param[out] match_score Uniform biological diagonal score.
 * @param[out] mismatch_score Uniform biological off-diagonal score.
 * @return `true` when every matrix entry matches the generated layout.
 */
template<sequence_alphabet p_alphabet,
         size_t p_alphabet_dimension = seqan3::alphabet_size<p_alphabet> + 1u>
[[nodiscard]] bool has_generated_diagonal_score_layout(
        const scoring_matrix<p_alphabet>& scoring_matrix,
        std::int8_t& match_score,
        std::int8_t& mismatch_score) noexcept {
    // The final alphabet rank is the padding sentinel. All biological diagonal
    // entries and all biological off-diagonal entries must be uniform.
    if constexpr (p_alphabet_dimension < 3) return false;
    constexpr size_t biological_ranks = p_alphabet_dimension - 1;
    match_score = scoring_matrix.at(0, 0);
    mismatch_score = scoring_matrix.at(0, 1);
    const std::int8_t ambiguity_score = scoring_matrix.at(
        0, static_cast<int>(biological_ranks));
    if (match_score < 0) return false;
    for (size_t row = 0; row < p_alphabet_dimension; ++row) {
        for (size_t col = 0; col < p_alphabet_dimension; ++col) {
            const std::int8_t expected =
                row == biological_ranks || col == biological_ranks
                    ? ambiguity_score
                    : row == col ? match_score : mismatch_score;
            const std::int8_t observed = scoring_matrix.at(
                static_cast<int>(row), static_cast<int>(col));
            if (observed != expected || observed > match_score) return false;
        }
    }
    return true;
}

/**
 * @brief Returns a low-bit mask for up to 32 packed two-bit DNA4 symbols.
 * @param[in] symbol_count Number of two-bit symbols represented by the mask.
 * @return Low `2 * symbol_count` bits set.
 */
[[nodiscard]] constexpr std::uint64_t low_dna4_symbol_mask(
        size_t symbol_count) noexcept {
    return symbol_count == 32u
        ? (std::numeric_limits<std::uint64_t>::max)()
        : (std::uint64_t{1} << (symbol_count * 2u)) - 1u;
}

/**
 * @brief Loads up to 32 LSB-first DNA4 ranks from an arbitrary coordinate.
 * @param[in] packed_units Contiguous packed DNA4 storage.
 * @param[in] base_position Logical start coordinate.
 * @param[in] symbol_count Number of ranks to load.
 * @return Requested ranks in the low two-bit groups.
 *
 * The second storage unit is touched only when the window crosses the first
 * unit boundary, so the final partial unit is never over-read.
 */
[[nodiscard]] HWY_INLINE std::uint64_t load_forward_dna4_window(
        const std::uint64_t* packed_units,
        size_t base_position,
        size_t symbol_count) noexcept {
    constexpr size_t symbols_per_unit = 32u;
    const size_t unit_index = base_position / symbols_per_unit;
    const size_t symbol_offset = base_position % symbols_per_unit;
    const size_t bit_offset = symbol_offset * 2u;
    std::uint64_t packed = packed_units[unit_index] >> bit_offset;
    if (symbol_offset != 0u
        && symbol_offset + symbol_count > symbols_per_unit) {
        packed |= packed_units[unit_index + 1u] << (64u - bit_offset);
    }
    return packed & low_dna4_symbol_mask(symbol_count);
}

/**
 * @brief Reverses 32 two-bit groups while retaining each rank's bit order.
 * @param[in] packed LSB-first DNA4 ranks.
 * @return Ranks in reverse group order.
 *
 * DNA4 complementation is a subsequent XOR with two set bits per rank.
 */
[[nodiscard]] constexpr std::uint64_t reverse_dna4_groups(
        std::uint64_t packed) noexcept {
    packed = ((packed & 0x0303030303030303ULL) << 6u)
        | ((packed & 0x0c0c0c0c0c0c0c0cULL) << 2u)
        | ((packed & 0x3030303030303030ULL) >> 2u)
        | ((packed & 0xc0c0c0c0c0c0c0c0ULL) >> 6u);
    return std::byteswap(packed);
}

/**
 * @brief Loads one forward or reverse-complement packed DNA4 window.
 * @tparam p_is_reversed Whether to read reverse-complement orientation.
 * @param[in] packed_units Contiguous packed DNA4 storage.
 * @param[in] total_symbols Total symbols represented by the storage view.
 * @param[in] logical_start Start coordinate in the requested orientation.
 * @param[in] symbol_count Number of ranks to load.
 * @return Requested oriented ranks in the low two-bit groups.
 */
template<bool p_is_reversed>
[[nodiscard]] HWY_INLINE std::uint64_t load_oriented_dna4_window(
        const std::uint64_t* packed_units,
        size_t total_symbols,
        size_t logical_start,
        size_t symbol_count) noexcept {
    if constexpr (!p_is_reversed) {
        return load_forward_dna4_window(
            packed_units, logical_start, symbol_count);
    } else {
        const size_t source_end = total_symbols - logical_start;
        const size_t source_begin = source_end - symbol_count;
        const std::uint64_t forward_window = load_forward_dna4_window(
            packed_units, source_begin, symbol_count);
        const size_t inactive_symbols = 32u - symbol_count;
        const std::uint64_t reversed_window = reverse_dna4_groups(forward_window)
            >> (inactive_symbols * 2u);
        return reversed_window ^ low_dna4_symbol_mask(symbol_count);
    }
}

/**
 * @brief Certifies a unique coordinate-diagonal global alignment.
 * @tparam p_is_reversed Whether query ranks use reverse orientation.
 * @tparam p_alphabet Logical sequence alphabet.
 * @tparam p_alphabet_dimension Matrix dimension including the sentinel rank.
 * @param[in] query_seq Query sequence facade.
 * @param[in] target_seq Target sequence facade.
 * @param[in] query_start Inclusive query coordinate.
 * @param[in] query_end Exclusive query coordinate.
 * @param[in] target_start Inclusive target coordinate.
 * @param[in] target_end Exclusive target coordinate.
 * @param[in] scoring_matrix Canonical scalar and shuffle score matrix.
 * @param[in] parameters Alignment contract.
 * @return Certificate with score, edit distance, and SIMD work counts.
 *
 * The scan consumes the same encoded ranks as dynamic programming. For equal
 * lengths, any different path contains an insertion and a deletion. A
 * diagonal score above that alternative-score bound establishes a unique
 * observable CIGAR, while equality remains ineligible because ties are visible.
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
        || parameters.gap_open <= 0 || parameters.gap_extend <= 0
        || !are_coordinates_ordered || sequence_length == 0 || target_length != sequence_length
        || query_end > query_seq.bp_size()
        || target_end > target_seq.bp_size()
        || sequence_length > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        return result;
    }
    constexpr size_t packed_cigar_limit = static_cast<size_t>(
        std::numeric_limits<std::uint32_t>::max() >> 4u);
    if (parameters.is_trace_enabled && sequence_length > packed_cigar_limit) return result;

    // The scan classifies equal and unequal biological ranks, so admit only a
    // matrix layout that this classification scores exactly.
    if constexpr (p_alphabet_dimension < 3) return result;
    constexpr size_t biological_ranks = p_alphabet_dimension - 1;
    std::int8_t match_score = 0;
    std::int8_t mismatch_score = 0;
    if (!has_generated_diagonal_score_layout<p_alphabet, p_alphabet_dimension>(
            scoring_matrix, match_score, mismatch_score)) {
        return result;
    }

    // Encode bounded chunks in the same orientation as the DP. Certified calls
    // avoid arena-backed padded, scratch, and trace storage.
    constexpr size_t scan_chunk_symbols = 1024u;
    // Every inspected byte is written by the encoder. Only the inactive tail
    // of a masked hardware load is initialized below.
    alignas(64) std::array<std::uint8_t, scan_chunk_symbols> query_chunk;
    alignas(64) std::array<std::uint8_t, scan_chunk_symbols> target_chunk;
    const hn::Rebind<uint8_t, block_aligner_simd_tag> byte_tag;
    const size_t lane_count = hn::Lanes(byte_tag);
    const auto rank_limit = hn::Set(
        byte_tag, static_cast<uint8_t>(biological_ranks));
    std::uint64_t equal_symbols = 0;
    for (size_t chunk_offset = 0; chunk_offset < sequence_length;
         chunk_offset += scan_chunk_symbols) {
        const size_t chunk_size = std::min(
            scan_chunk_symbols, sequence_length - chunk_offset);
        query_seq.template copy_ranks<p_is_reversed, false>(
            std::span<std::uint8_t>{query_chunk.data(), chunk_size},
            query_start + chunk_offset,
            query_start + chunk_offset + chunk_size);
        target_seq.template copy_ranks<false, false>(
            std::span<std::uint8_t>{target_chunk.data(), chunk_size},
            target_start + chunk_offset,
            target_start + chunk_offset + chunk_size);
        // Highway's masked load is permitted to issue a full hardware load
        // (notably on NEON). Initialise only the inactive bytes of the one
        // partial vector so that load never observes indeterminate storage.
        // Full chunks are lane-aligned for every admitted lane shape.
        const size_t tail_lanes = chunk_size % lane_count;
        if (tail_lanes != 0) {
            const size_t tail_padding = lane_count - tail_lanes;
            std::memset(
                query_chunk.data() + chunk_size, 0, tail_padding);
            std::memset(
                target_chunk.data() + chunk_size, 0, tail_padding);
        }
        for (size_t offset = 0; offset < chunk_size; offset += lane_count) {
            const size_t active_lanes = std::min(
                lane_count, chunk_size - offset);
            const auto active = hn::FirstN(byte_tag, active_lanes);
            const auto query_bytes = hn::MaskedLoad(
                active, byte_tag, query_chunk.data() + offset);
            const auto reference_bytes = hn::MaskedLoad(
                active, byte_tag, target_chunk.data() + offset);
            const auto valid_ranks = hn::And(
                hn::Lt(query_bytes, rank_limit),
                hn::Lt(reference_bytes, rank_limit));
            if (hn::CountTrue(byte_tag, hn::And(active, valid_ranks))
                != active_lanes) {
                return block_diagonal_certificate{};
            }
            equal_symbols += hn::CountTrue(
                byte_tag, hn::And(active, hn::Eq(
                    query_bytes, reference_bytes)));
            ++result.vector_batches;
        }
    }

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
 * @brief Attempts resident packed DNA4 certification without byte ranks.
 * @tparam p_is_reversed Whether query ranks use reverse orientation.
 * @tparam p_alphabet Logical sequence alphabet.
 * @param[in] query_seq Query sequence facade.
 * @param[in] target_seq Target sequence facade.
 * @param[in] query_start Inclusive query coordinate.
 * @param[in] query_end Exclusive query coordinate.
 * @param[in] target_start Inclusive target coordinate.
 * @param[in] target_end Exclusive target coordinate.
 * @param[in] plan Prepared matrix and alignment contract.
 * @return Three-way attempt result with SIMD work counts.
 *
 * `not_eligible` means no packed scan ran and the byte-rank path remains
 * available. `rejected` means the packed scan ran but did not certify the
 * diagonal, so dynamic programming starts without repeating that scan.
 * `certified` carries the exact score, endpoints, edit distance, and match run.
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
    if constexpr (!std::same_as<p_alphabet, seq::dna4>) {
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

        // Borrow the complete logical facade descriptors.  Region-only probes
        // lose the logical totals required by reverse orientation and expose
        // nonzero leading offsets for ordinary unaligned slices.  The packed
        // loader already handles arbitrary symbol coordinates, so retain each
        // descriptor's physical leading offset explicitly.
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

        // Extraction handles arbitrary base offsets one word at a time.  The
        // staging arrays are completely initialized before each masked load;
        // Highway then collapses and popcounts two-bit mismatches across the
        // native packed-word lane_count.
        const hn::Repartition<std::uint64_t, block_aligner_simd_tag> packed_word_tag;
        const size_t lane_count = hn::Lanes(packed_word_tag);
        constexpr size_t max_packed_lanes = 4u;
        alignas(64) std::array<std::uint64_t, max_packed_lanes> query_words{};
        alignas(64) std::array<std::uint64_t, max_packed_lanes> target_words{};
        const auto pair_low_bits = hn::Set(packed_word_tag, 0x5555555555555555ULL);
        const size_t word_count = divide_block_extent_round_up(query_length, 32u);
        std::uint64_t mismatch_symbols = 0;
        for (size_t first_word = 0; first_word < word_count;
             first_word += lane_count) {
            const size_t active_lanes = std::min(lane_count,
                word_count - first_word);
            for (size_t lane = 0; lane < active_lanes; ++lane) {
                const size_t symbol_offset = (first_word + lane) * 32u;
                const size_t symbol_count = std::min(
                    size_t{32}, query_length - symbol_offset);
                const size_t query_logical_start =
                    query_start + symbol_offset;
                query_words[lane] = load_oriented_dna4_window<p_is_reversed>(
                    query_units,
                    query_physical_total,
                    p_is_reversed
                        ? query_logical_start
                        : query_leading + query_logical_start,
                    symbol_count);
                target_words[lane] = load_oriented_dna4_window<false>(
                    target_units, target_leading + target_logical_total,
                    target_leading + target_start + symbol_offset,
                    symbol_count);
            }
            const auto active = hn::FirstN(packed_word_tag, active_lanes);
            const auto query_packed = hn::MaskedLoad(
                active, packed_word_tag, query_words.data());
            const auto target_packed = hn::MaskedLoad(
                active, packed_word_tag, target_words.data());
            const auto different_bits = hn::Xor(query_packed, target_packed);
            const auto different_symbols = hn::And(
                hn::Or(different_bits, hn::ShiftRight<1>(different_bits)),
                pair_low_bits);
            const auto per_lane_mismatches = hn::PopulationCount(
                different_symbols);
            mismatch_symbols += hn::ReduceSum(
                packed_word_tag, hn::IfThenElseZero(active, per_lane_mismatches));
            ++attempt.certificate.vector_batches;
        }

        attempt.decision = block_packed_certificate_decision::rejected;
        const std::uint64_t equal_symbols =
            static_cast<std::uint64_t>(query_length) - mismatch_symbols;
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

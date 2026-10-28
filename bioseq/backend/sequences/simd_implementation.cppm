/**
 * @file simd_implementation.cppm
 * @brief Implements Highway SIMD lookup, packing, and decoding for DNA sequences.
 */
module;

#include <hwy/highway.h>
#include <libassert/assert.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

module bioseq.sequences.simd;

import bioseq.sequences.rank_pack_core;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

#if HWY_TARGET == HWY_SSE2 || HWY_TARGET == HWY_SSSE3 || \
    HWY_TARGET == HWY_SSE4 || HWY_TARGET == HWY_AVX2
#define BIOSEQ_SIMD_X86_PRE_AVX3_FIXED128 1
#else
#define BIOSEQ_SIMD_X86_PRE_AVX3_FIXED128 0
#endif

namespace detail {

/// @brief Expands sixteen packed dna4 bytes into sixty-four ASCII symbols.
template <class p_simd_tag>
HWY_INLINE void unpack_16_bytes_to_64_ascii(
    p_simd_tag simd_tag,
    hn::Vec<p_simd_tag> packed_bytes,
    hn::Vec<p_simd_tag> rank_to_ascii_lut,
    std::uint8_t* HWY_RESTRICT destination) noexcept {
    const auto low_two_bit_mask = hn::Set(
        simd_tag, std::uint8_t{0x03});

    const auto first_rank = hn::And(packed_bytes, low_two_bit_mask);
    const auto second_rank = hn::And(
        hn::ShiftRight<2>(packed_bytes), low_two_bit_mask);
    const auto third_rank = hn::And(
        hn::ShiftRight<4>(packed_bytes), low_two_bit_mask);
    const auto fourth_rank = hn::And(
        hn::ShiftRight<6>(packed_bytes), low_two_bit_mask);

    using uint16_simd_tag_type = hn::Repartition<std::uint16_t, p_simd_tag>;
    uint16_simd_tag_type uint16_simd_tag;

    const auto first_pair_lower = hn::InterleaveLower(
        simd_tag, first_rank, second_rank);
    const auto first_pair_upper = hn::InterleaveUpper(
        simd_tag, first_rank, second_rank);
    const auto second_pair_lower = hn::InterleaveLower(
        simd_tag, third_rank, fourth_rank);
    const auto second_pair_upper = hn::InterleaveUpper(
        simd_tag, third_rank, fourth_rank);

    const auto first_rank_chunk = hn::BitCast(
        simd_tag,
        hn::InterleaveLower(
            uint16_simd_tag,
            hn::BitCast(uint16_simd_tag, first_pair_lower),
            hn::BitCast(uint16_simd_tag, second_pair_lower)));
    const auto second_rank_chunk = hn::BitCast(
        simd_tag,
        hn::InterleaveUpper(
            uint16_simd_tag,
            hn::BitCast(uint16_simd_tag, first_pair_lower),
            hn::BitCast(uint16_simd_tag, second_pair_lower)));
    const auto third_rank_chunk = hn::BitCast(
        simd_tag,
        hn::InterleaveLower(
            uint16_simd_tag,
            hn::BitCast(uint16_simd_tag, first_pair_upper),
            hn::BitCast(uint16_simd_tag, second_pair_upper)));
    const auto fourth_rank_chunk = hn::BitCast(
        simd_tag,
        hn::InterleaveUpper(
            uint16_simd_tag,
            hn::BitCast(uint16_simd_tag, first_pair_upper),
            hn::BitCast(uint16_simd_tag, second_pair_upper)));

    const auto first_ascii_chunk = hn::TableLookupBytes(
        rank_to_ascii_lut, first_rank_chunk);
    const auto second_ascii_chunk = hn::TableLookupBytes(
        rank_to_ascii_lut, second_rank_chunk);
    const auto third_ascii_chunk = hn::TableLookupBytes(
        rank_to_ascii_lut, third_rank_chunk);
    const auto fourth_ascii_chunk = hn::TableLookupBytes(
        rank_to_ascii_lut, fourth_rank_chunk);

    constexpr std::size_t k_lanes_per_chunk = 16;
    hn::StoreU(first_ascii_chunk, simd_tag, destination);
    hn::StoreU(
        second_ascii_chunk,
        simd_tag,
        destination + k_lanes_per_chunk);
    hn::StoreU(
        third_ascii_chunk,
        simd_tag,
        destination + 2 * k_lanes_per_chunk);
    hn::StoreU(
        fourth_ascii_chunk,
        simd_tag,
        destination + 3 * k_lanes_per_chunk);
}

/// Decodes dna4 with SIMD for complete blocks and scalar reads for the tail.
void decode_dna4_words_to_ascii_impl(
    const std::uint64_t* HWY_RESTRICT words,
    std::uint8_t* HWY_RESTRICT destination,
    std::size_t total_letters) noexcept {
    using simd_tag_type = hn::FixedTag<std::uint8_t, 16>;
    constexpr simd_tag_type simd_tag;

    static constexpr auto k_rank_to_ascii_lut =
        build_rank_to_letter_lut<seq::dna4>();
    const auto rank_to_ascii_lut = hn::LoadDup128(
        simd_tag, k_rank_to_ascii_lut.data());

    constexpr std::size_t k_letters_per_iteration = 64;

    std::size_t letters_emitted = 0;
    std::size_t word_cursor = 0;
    const auto* packed_bytes_base =
        reinterpret_cast<const std::uint8_t*>(words);

    while (letters_emitted + k_letters_per_iteration <= total_letters) {
        const auto packed_bytes = hn::LoadU(
            simd_tag, packed_bytes_base + word_cursor * 8);
        unpack_16_bytes_to_64_ascii(
            simd_tag,
            packed_bytes,
            rank_to_ascii_lut,
            destination + letters_emitted);
        letters_emitted += k_letters_per_iteration;
        word_cursor += 2;
    }

    while (letters_emitted < total_letters) {
        const std::size_t word_index = letters_emitted / 32;
        const std::size_t letter_in_word = letters_emitted % 32;
        const std::uint64_t word = words[word_index];
        const std::uint8_t rank = static_cast<std::uint8_t>(
            (word >> (2 * letter_in_word)) & 0x3u);
        destination[letters_emitted] = k_rank_to_ascii_lut[rank];
        ++letters_emitted;
    }
}

/// @brief Decodes two-ranks-per-byte dna5 storage into ASCII.
void decode_dna5_uint8_words_to_ascii_impl(
    const std::uint8_t* HWY_RESTRICT words,
    std::uint8_t* HWY_RESTRICT destination,
    std::size_t total_letters) noexcept {
    using simd_tag_type = hn::FixedTag<std::uint8_t, 16>;
    constexpr simd_tag_type simd_tag;

    static constexpr auto k_rank_to_ascii_lut =
        build_rank_to_letter_lut<seq::dna5>();
    const auto rank_to_ascii_lut = hn::LoadDup128(
        simd_tag, k_rank_to_ascii_lut.data());

    constexpr std::size_t k_letters_per_iteration = 32;
    constexpr std::size_t k_bytes_per_iteration = 16;

    std::size_t letters_emitted = 0;
    std::size_t bytes_consumed = 0;
    while (letters_emitted + k_letters_per_iteration <= total_letters) {
        const auto packed = hn::LoadU(
            simd_tag, words + bytes_consumed);
        const auto low_three_bit_mask = hn::Set(
            simd_tag, std::uint8_t{0x07});
        const auto first_rank = hn::And(packed, low_three_bit_mask);
        const auto second_rank = hn::And(
            hn::ShiftRight<3>(packed), low_three_bit_mask);

        const auto interleaved_lo =
            hn::InterleaveLower(simd_tag, first_rank, second_rank);
        const auto interleaved_hi =
            hn::InterleaveUpper(simd_tag, first_rank, second_rank);
        const auto ascii_lo =
            hn::TableLookupBytes(rank_to_ascii_lut, interleaved_lo);
        const auto ascii_hi =
            hn::TableLookupBytes(rank_to_ascii_lut, interleaved_hi);
        hn::StoreU(ascii_lo, simd_tag, destination + letters_emitted);
        hn::StoreU(
            ascii_hi,
            simd_tag,
            destination + letters_emitted + 16);
        letters_emitted += k_letters_per_iteration;
        bytes_consumed += k_bytes_per_iteration;
    }

    while (letters_emitted < total_letters) {
        const std::size_t byte_index = letters_emitted / 2;
        const std::size_t letter_in_byte = letters_emitted % 2;
        const std::uint8_t byte = words[byte_index];
        const std::uint8_t rank = static_cast<std::uint8_t>(
            (byte >> (3 * letter_in_byte)) & 0x07u);
        destination[letters_emitted] = k_rank_to_ascii_lut[rank];
        ++letters_emitted;
    }
}


} // namespace detail

#undef BIOSEQ_SIMD_X86_PRE_AVX3_FIXED128

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

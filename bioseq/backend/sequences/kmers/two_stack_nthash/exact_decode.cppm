/**
 * @file exact_decode.cppm
 * @brief Decodes packed nucleotide symbols into SIMD lanes.
 */
module;

#include <hwy/highway.h>
#include <libassert/assert.hpp>

export module bioseq.highway.sources.minimizer_two_stack_nthash_exact_decode;

import std;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.highway.sources.minimizer_two_stack_nthash_resident_stream;
import bioseq.highway.sources.hasher_base;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

namespace two_stack_internal {

/**
 * @brief Decoded DNA4 ranks and a flag for crossing a packed-unit boundary.
 * @tparam p_simd_tag Sets the number of lanes.
 */
template<class p_simd_tag>
struct decoded_dna4_rank_vector {
    /** @brief Byte descriptor matching the source descriptor's lane count. */
    using byte_simd_tag_type = hn::Rebind<std::uint8_t, p_simd_tag>;
    hn::Vec<byte_simd_tag_type> ranks;
    bool crossed_word = false;
};

/**
 * @brief Decoded nucleotide codes with a mask of ambiguous positions.
 *
 * Codes 0 through 3 represent A, C, G, and T. The result also records whether
 * the read crossed a packed-unit boundary.
 */
template<class p_simd_tag>
struct decoded_nt4_rank_vector {
    /** @brief Byte descriptor matching the source descriptor's lane count. */
    using byte_simd_tag_type = hn::Rebind<std::uint8_t, p_simd_tag>;
    hn::Vec<byte_simd_tag_type> codes;
    std::uint64_t ambiguous_lanes = 0;
    bool crossed_word = false;
};

/**
 * @brief Extracts a 32-bit or 64-bit chonk from two joined 64-bit words.
 *
 * origin is the first bit of the chunk in packed_low followed by packed_high.
 * The caller must keep the requested shifts within those words.
 */
template<class p_lane_type>
[[nodiscard, gnu::always_inline]] HWY_INLINE p_lane_type
extract_packed_dna4_lane_chunk(
    std::uint64_t packed_low,
    std::uint64_t packed_high,
    std::size_t origin) noexcept
{
    static_assert(std::is_unsigned_v<p_lane_type>);
    static_assert(sizeof(p_lane_type) == 4 || sizeof(p_lane_type) == 8);
    if (origin < 64) {
        std::uint64_t chunk = packed_low >> origin;
        if (origin != 0) {
            chunk |= packed_high << (64 - origin);
        }
        return static_cast<p_lane_type>(chunk);
    }
    return static_cast<p_lane_type>(packed_high >> (origin - 64));
}

/**
 * @brief Decodes consecutive DNA4 symbols into byte lanes.
 *
 * Reads packed units into  a small pair of words, then extracts each two-bit
 * rank with SIMD shifts. Input can use 8-, 16-, 32-, or 64-bit units.
 * @param local_bp First symbol within, the packed fragment.
 * @param rank_count Active symbols to decode, at most Lanes(d). The supplied
 * span must cover them.
 * @return Ranks and a flag indicating a read across a packed-unit boundary.
 */
// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN decode_packed_dna4_rank_vector
template<class p_simd_tag, class p_alphabet_traits>
[[nodiscard]] HWY_INLINE decoded_dna4_rank_vector<p_simd_tag>
decode_packed_dna4_rank_vector(
    p_simd_tag d,
    std::span<const typename p_alphabet_traits::storage_unit> packed,
    std::size_t local_bp,
    std::size_t rank_count) noexcept
{
    using lane_type = hn::TFromD<p_simd_tag>;
    using storage_unit = typename p_alphabet_traits::storage_unit;
    using unsigned_storage = std::make_unsigned_t<storage_unit>;
    static_assert(std::is_unsigned_v<lane_type>);
    static_assert(sizeof(lane_type) == 4 || sizeof(lane_type) == 8);
    static_assert(p_alphabet_traits::bits_per_char == 2);
    static_assert(std::is_integral_v<storage_unit>);
    static_assert(sizeof(storage_unit) == 1 || sizeof(storage_unit) == 2
        || sizeof(storage_unit) == 4 || sizeof(storage_unit) == 8);
    constexpr std::size_t unit_bits = sizeof(storage_unit) * 8;
    constexpr std::size_t letters_per_unit = unit_bits / 2;
    static_assert(p_alphabet_traits::letters_per_unit == letters_per_unit);

    const std::size_t first_unit = local_bp / letters_per_unit;
    const std::size_t rank_in_unit = local_bp % letters_per_unit;
    const std::size_t bit_offset = rank_in_unit * 2;
    const std::size_t required_bits = bit_offset + rank_count * 2;
    const std::size_t required_units =
        (required_bits + unit_bits - 1) / unit_bits;

    std::uint64_t packed_low = 0;
    std::uint64_t packed_high = 0;
    // A bit_casted_view can expose u64 storage as u32 units. Read through bytes
    // with memcpy so those borrowed views do not violate C++ aliasing rules.
    const auto* packed_bytes = reinterpret_cast<const unsigned char*>(
        packed.data());
    for (std::size_t unit_offset = 0;
         unit_offset < required_units;
         ++unit_offset) {
        unsigned_storage storage_value = 0;
        std::memcpy(
            &storage_value,
            packed_bytes
                + (first_unit + unit_offset) * sizeof(storage_unit),
            sizeof(storage_unit));
        const std::uint64_t value = static_cast<std::uint64_t>(
            storage_value);
        const std::size_t destination_bit = unit_offset * unit_bits;
        if (destination_bit < 64) {
            packed_low |= value << destination_bit;
            const std::size_t low_bits_available = 64 - destination_bit;
            if (unit_bits > low_bits_available) {
                packed_high |= value >> low_bits_available;
            }
        } else {
            packed_high |= value << (destination_bit - 64);
        }
    }

    constexpr std::size_t lane_bits = sizeof(lane_type) * 8;
    const std::size_t chunk_origin = bit_offset / lane_bits * lane_bits;
    const lane_type packed_chunk_low =
        extract_packed_dna4_lane_chunk<lane_type>(
            packed_low, packed_high, chunk_origin);
    const lane_type packed_chunk_high =
        extract_packed_dna4_lane_chunk<lane_type>(
            packed_low, packed_high, chunk_origin + lane_bits);
    const lane_type relative_bit_offset = static_cast<lane_type>(
        bit_offset - chunk_origin);

    const auto shifts = hn::Mul(
        lane_indices(d),
        hn::Set(d, lane_type{2}));
    const auto source_shifts = hn::Add(
        shifts, hn::Set(d, relative_bit_offset));
    const auto shift_mod_lane = hn::And(
        source_shifts,
        hn::Set(d, static_cast<lane_type>(lane_bits - 1)));
    const auto use_high = hn::Ge(
        source_shifts, hn::Set(d, static_cast<lane_type>(lane_bits)));
    const auto extracted = hn::IfThenElse(
        use_high,
        hn::Shr(hn::Set(d, packed_chunk_high), shift_mod_lane),
        hn::Shr(hn::Set(d, packed_chunk_low), shift_mod_lane));
    const auto ranks = hn::And(
        extracted,
        hn::Set(d, lane_type{3}));
    const hn::Rebind<std::uint16_t, p_simd_tag> d_u16;
    const hn::Rebind<std::uint8_t, p_simd_tag> d_u8;
    if constexpr (sizeof(lane_type) == 8) {
        const hn::Rebind<std::uint32_t, p_simd_tag> d_u32;
        const auto ranks_u32 = hn::DemoteTo(d_u32, ranks);
        const auto ranks_u16 = hn::DemoteTo(d_u16, ranks_u32);
        return {
            .ranks = hn::DemoteTo(d_u8, ranks_u16),
            .crossed_word = required_units > 1,
        };
    } else {
        const auto ranks_u16 = hn::DemoteTo(d_u16, ranks);
        return {
            .ranks = hn::DemoteTo(d_u8, ranks_u16),
            .crossed_word = required_units > 1,
        };
    }
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END decode_packed_dna4_rank_vector

/**
 * @brief Decodes nucleotide symbols and maps them to A/C/G/T codes.
 *
 * Some alphabets leave unused bits at the end of each packed unit DNA5, for
 * example, needs three bits per symbol. Each lane therefore selects its  own
 * source unit and shift before SIMD code extracts and maps the rank.
 * @param local_bp First symbol within the packed fragment.
 * @param rank_count Active symbols to decode, at most Lanes(d). The supplied
 * span must cover them.
 * @return Byte codes, ambiguous-position bits, and the packed-unit crossing flag.
 */
// BIOSEQ_PRIVATE_PACKED_ACCESS_BEGIN decode_packed_nt4_rank_vector
template<class p_simd_tag, class p_alphabet_traits>
[[nodiscard]] HWY_INLINE decoded_nt4_rank_vector<p_simd_tag>
decode_packed_nt4_rank_vector(
    p_simd_tag d,
    std::span<const typename p_alphabet_traits::storage_unit> packed,
    std::size_t local_bp,
    std::size_t rank_count) noexcept
{
    using alphabet_type = typename p_alphabet_traits::alphabet_type;
    using storage_unit = typename p_alphabet_traits::storage_unit;
    using unsigned_storage = std::make_unsigned_t<storage_unit>;
    static_assert(seqan3::nucleotide_alphabet<alphabet_type>);
    static_assert(std::is_integral_v<storage_unit>);
    static_assert(sizeof(storage_unit) == 1 || sizeof(storage_unit) == 2
        || sizeof(storage_unit) == 4 || sizeof(storage_unit) == 8);
    static_assert(p_alphabet_traits::bits_per_char > 0
        && p_alphabet_traits::bits_per_char <= 8);
    constexpr std::size_t unit_bits = sizeof(storage_unit) * 8;
    constexpr std::size_t bits_per_char = p_alphabet_traits::bits_per_char;
    constexpr std::size_t letters_per_unit = unit_bits / bits_per_char;
    static_assert(p_alphabet_traits::letters_per_unit == letters_per_unit);
    constexpr std::size_t max_lanes = hn::MaxLanes(p_simd_tag{});

    HWY_ALIGN std::array<std::uint64_t, max_lanes> source_words{};
    HWY_ALIGN std::array<std::uint64_t, max_lanes> source_shifts{};
    for (std::size_t lane = 0; lane < rank_count; ++lane) {
        const std::size_t bp = local_bp + lane;
        const std::size_t unit = bp / letters_per_unit;
        const std::size_t rank_in_unit = bp % letters_per_unit;
        source_words[lane] = static_cast<std::uint64_t>(
            static_cast<unsigned_storage>(packed[unit]));
        source_shifts[lane] = rank_in_unit * bits_per_char;
    }

    const auto ranks = hn::And(
        hn::Shr(
            hn::LoadU(d, source_words.data()),
            hn::LoadU(d, source_shifts.data())),
        hn::Set(d, (std::uint64_t{1} << bits_per_char) - 1));
    auto nt4_codes = hn::Set(
        d, static_cast<std::uint64_t>(k_nt4_sentinel));
    for (std::size_t rank = 0;
         rank < seqan3::alphabet_size<alphabet_type>;
         ++rank) {
        nt4_codes = hn::IfThenElse(
            hn::Eq(ranks, hn::Set(d, static_cast<std::uint64_t>(rank))),
            hn::Set(d, static_cast<std::uint64_t>(
                k_nt4_lut<alphabet_type>[rank])),
            nt4_codes);
    }
    const auto active = hn::FirstN(d, rank_count);
    const auto ambiguous = hn::And(
        active,
        hn::Ge(
            nt4_codes,
            hn::Set(d, static_cast<std::uint64_t>(k_nt4_sentinel))));

    const hn::Rebind<std::uint32_t, p_simd_tag> d_u32;
    const hn::Rebind<std::uint16_t, p_simd_tag> d_u16;
    const hn::Rebind<std::uint8_t, p_simd_tag> d_u8;
    const auto codes_u32 = hn::DemoteTo(d_u32, nt4_codes);
    const auto codes_u16 = hn::DemoteTo(d_u16, codes_u32);
    const std::size_t first_unit = local_bp / letters_per_unit;
    const std::size_t last_unit = rank_count == 0
        ? first_unit
        : (local_bp + rank_count - 1) / letters_per_unit;
    return {
        .codes = hn::DemoteTo(d_u8, codes_u16),
        .ambiguous_lanes = static_cast<std::uint64_t>(
            hn::BitsFromMask(d, ambiguous)),
        .crossed_word = first_unit != last_unit,
    };
}
// BIOSEQ_PRIVATE_PACKED_ACCESS_END decode_packed_nt4_rank_vector

/**
 * @brief Loads DNA4 byte codes into u64 SIMD lanes.
 * @param codes Points to at least Lanes(d) bytes.
 */
template<class p_simd_tag>
[[nodiscard]] HWY_INLINE hn::Vec<p_simd_tag> load_promoted_dna4_codes(
    p_simd_tag d,
    const std::uint8_t* HWY_RESTRICT codes) noexcept
{
    const hn::Rebind<std::uint8_t, p_simd_tag> d_u8;
    const hn::Rebind<std::uint16_t, p_simd_tag> d_u16;
    const hn::Rebind<std::uint32_t, p_simd_tag> d_u32;
    const auto codes_u8 = hn::LoadU(d_u8, codes);
    const auto codes_u16 = hn::PromoteTo(d_u16, codes_u8);
    const auto codes_u32 = hn::PromoteTo(d_u32, codes_u16);
    return hn::PromoteTo(d, codes_u32);
}

/**
 * @brief Loads DNA4 byte codes into u32 SIMD lanes.
 * @param codes Points to at least Lanes(d) bytes.
 */
template<class p_simd_tag>
[[nodiscard]] HWY_INLINE hn::Vec<p_simd_tag> load_promoted_dna4_codes_u32(
    p_simd_tag d,
    const std::uint8_t* HWY_RESTRICT codes) noexcept
{
    static_assert(std::is_same_v<hn::TFromD<p_simd_tag>, std::uint32_t>);
    const hn::Rebind<std::uint8_t, p_simd_tag> d_u8;
    const hn::Rebind<std::uint16_t, p_simd_tag> d_u16;
    const auto codes_u8 = hn::LoadU(d_u8, codes);
    return hn::PromoteTo(d, hn::PromoteTo(d_u16, codes_u8));
}

/// @brief Lower and upper halves of a u32 mask, eaqh using u64 lanes.
template<class p_uint32_simd_tag, class p_uint64_simd_tag>
struct split_u32_mask_result {
    hn::Mask<p_uint64_simd_tag> lower;
    hn::Mask<p_uint64_simd_tag> upper;
};

/**
 * @brief Splits a u32 mask into two u64 masks while keeping lane order.
 *
 * d64 must have half as many lanes as d32. The first half goes to lower
 * and the second  half to upper.
 */

template<class p_uint32_simd_tag, class p_uint64_simd_tag>
[[nodiscard]] HWY_INLINE split_u32_mask_result<p_uint32_simd_tag, p_uint64_simd_tag> split_u32_mask(
    p_uint32_simd_tag d32,
    p_uint64_simd_tag d64,
    hn::Mask<p_uint32_simd_tag> mask) noexcept
{
    const auto bits = hn::VecFromMask(d32, mask);
    const auto zero = hn::Zero(d64);
    return {
        .lower = hn::Ne(hn::PromoteLowerTo(d64, bits), zero),
        .upper = hn::Ne(hn::PromoteUpperTo(d64, bits), zero),
    };
}

template<class p_uint32_simd_tag, class p_uint64_simd_tag>
[[nodiscard]] HWY_INLINE hn::Mask<p_uint32_simd_tag> join_u64_masks(
    p_uint32_simd_tag d32,
    p_uint64_simd_tag d64,
    hn::Mask<p_uint64_simd_tag> lower,
    hn::Mask<p_uint64_simd_tag> upper) noexcept
{
    const hn::Half<p_uint32_simd_tag> d32_half;
    return hn::CombineMasks(
        d32,
        hn::DemoteMaskTo(d32_half, d64, upper),
        hn::DemoteMaskTo(d32_half, d64, lower));
}


}  // namespace two_stack_internal

}  // namespace target
}  // namespace bioseq::simd

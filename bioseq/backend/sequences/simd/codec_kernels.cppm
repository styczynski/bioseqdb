/**
 * @file codec_kernels.cppm
 * @brief SIMD rank lookup, packing, and decoding for dna4 and dna5.
 */
module;

#include <hwy/highway.h>
#include <libassert/assert.hpp>

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <functional>

#include <boost/integer.hpp>

export module bioseq.sequences.simd:codec_kernels;

export import :streaming;
import bioseq.sequences;
import bioseq.sequences.rank_pack_core;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

#if HWY_TARGET == HWY_SSE2 || HWY_TARGET == HWY_SSSE3 || \
    HWY_TARGET == HWY_SSE4 || HWY_TARGET == HWY_AVX2
#define BIOSEQ_SIMD_X86_PRE_AVX3_FIXED128 1
#else
#define BIOSEQ_SIMD_X86_PRE_AVX3_FIXED128 0
#endif

// SIMD character lookup and rank packing.

/**
 * Looks up ranks using two 16-byte halves of a table.
 *
 * Each byte selects an entry using its low five bits, which lets DNA letters
 * shaae an entry across ASCII cases. Non-ASCII bytes select entry zero, filled
 * with the policy's default rank or skip marker. Other ASCII bytes can still
 * share an entry with a letter, so this operation does not validate input.
 */
template <class p_simd_tag>
HWY_INLINE hn::Vec<p_simd_tag> lookup_ranks_via_32byte_lut(
    p_simd_tag simd_tag,
    hn::Vec<p_simd_tag> lut_low,
    hn::Vec<p_simd_tag> lut_high,
    hn::Vec<p_simd_tag> chunk) noexcept {
    const auto high_bit =
        hn::And(chunk, hn::Set(simd_tag, std::uint8_t{0x80}));
    const auto is_non_ascii = hn::Ne(high_bit, hn::Zero(simd_tag));
    const auto safe = hn::IfThenZeroElse(is_non_ascii, chunk);

    const auto low_four_bits =
        hn::And(safe, hn::Set(simd_tag, std::uint8_t{0x0F}));
    const auto low_result = hn::TableLookupBytes(lut_low, low_four_bits);
    const auto high_result = hn::TableLookupBytes(lut_high, low_four_bits);
    const auto fifth_bit =
        hn::And(safe, hn::Set(simd_tag, std::uint8_t{0x10}));
    const auto is_high = hn::Ne(fifth_bit, hn::Zero(simd_tag));
    return hn::IfThenElse(is_high, high_result, low_result);
}

/// Looks up dna4 ranks using each byte's low  four bits.
/// Unused entries return 0xFF. Callers must account for bytes that share
/// a letter's low bits, since the lookup cannot distinguish them.
template <class p_simd_tag>
HWY_INLINE hn::Vec<p_simd_tag> dna4_lookup_ranks(
    p_simd_tag /* tag, deduction-only */,
    hn::Vec<p_simd_tag> letter_to_rank_table_vec,
    hn::Vec<p_simd_tag> low_nibble_mask,
    hn::Vec<p_simd_tag> chunk_bytes) noexcept {
    const auto letter_index = hn::And(chunk_bytes, low_nibble_mask);
    return hn::TableLookupBytes(letter_to_rank_table_vec, letter_index);
}

/**
 * Packs sixteen dna4 ranks into one 32-bit value, first letter in the low bits.
 *
 * Each rank must be between zero and three. Supported targets combine ranks
 * with vector shifts and sums. Older x86 targets pack stored ranks scalarly.
 */
template <class p_simd_tag>
HWY_INLINE std::uint32_t pack_16_dna4_to_u32(
    p_simd_tag simd_tag,
    hn::Vec<p_simd_tag> ranks) noexcept {
#if BIOSEQ_SIMD_X86_PRE_AVX3_FIXED128
    HWY_ALIGN std::uint8_t scratch[16];
    hn::Store(ranks, simd_tag, scratch);
    std::uint32_t packed = 0;
    for (std::size_t lane_index = 0; lane_index < 16; ++lane_index) {
        packed |= static_cast<std::uint32_t>(scratch[lane_index] & 0x03u)
                  << (2 * lane_index);
    }
    return packed;
#else
    HWY_ALIGN constexpr std::uint8_t k_shift_pattern[16] = {
        0, 2, 4, 6, 0, 2, 4, 6,
        0, 2, 4, 6, 0, 2, 4, 6};
    const auto shift_pattern_vector = hn::LoadU(simd_tag, k_shift_pattern);
    const auto shifted = hn::Shl(ranks, shift_pattern_vector);

    using uint32_simd_tag_type =
        hn::Repartition<std::uint32_t, p_simd_tag>;
    uint32_simd_tag_type uint32_simd_tag;
    const auto sums = hn::SumsOf4(shifted);

    HWY_ALIGN constexpr std::uint32_t k_multiplier_pattern[4] = {
        1u, 1u << 8, 1u << 16, 1u << 24};
    const auto multipliers =
        hn::LoadU(uint32_simd_tag, k_multiplier_pattern);
    const auto multiplied = hn::Mul(sums, multipliers);
    return hn::ReduceSum(uint32_simd_tag, multiplied);
#endif
}

/// Returns true if any lane contains the 0xFF skip marker.
template <class p_simd_tag>
HWY_INLINE bool has_skip_sentinel(
    p_simd_tag simd_tag,
    hn::Vec<p_simd_tag> ranks) noexcept {
    const auto sentinel_mask =
        hn::Eq(ranks, hn::Set(simd_tag, std::uint8_t{0xFF}));
    return !hn::AllFalse(simd_tag, sentinel_mask);
}

/// Appends sixteen packed dna4 ranks to a uint64_t packing state
/// Writes completed words to packed_words and keeps the remaining ranks.
/// local_partial_letters must be smaller than 32.
template <typename p_storage_unit>
HWY_INLINE void merge_packed_chunk_into_partial(
    p_storage_unit& local_partial_word,
    std::size_t& local_partial_letters,
    std::vector<p_storage_unit>* HWY_RESTRICT packed_words,
    std::uint32_t packed_chunk) noexcept {
    std::size_t appended_letters = 0u;
    // Flush one completed storage unit into the output vector.
    auto write_unit = [packed_words](p_storage_unit value) {
        packed_words->push_back(value);
        return true;
    };
    const bool appended =
        bioseq::sequences::rank_pack_detail::append_prepacked_ranks_lsb<
            p_storage_unit, 2u, 32u>(
                local_partial_word,
                local_partial_letters,
                appended_letters,
                packed_chunk,
                16u,
                write_unit);
    DEBUG_ASSERT(appended && appended_letters == 16u,
                 "growing SIMD rank chunk pack unexpectedly failed");
}

/// Appends count packed dna4 ranks to the current uint64_t word.
/// count must be at most 16, and local_partial_letters must be below 32.
template <typename p_storage_unit>
HWY_INLINE void merge_variable_chunk_into_partial(
    p_storage_unit& local_partial_word,
    std::size_t& local_partial_letters,
    std::vector<p_storage_unit>* HWY_RESTRICT packed_words,
    std::uint32_t packed_chunk,
    std::size_t count) noexcept {
    std::size_t appended_letters = 0u;
    // Flush one completed storage unit into the output vector.
    auto write_unit = [packed_words](p_storage_unit value) {
        packed_words->push_back(value);
        return true;
    };
    const bool appended =
        bioseq::sequences::rank_pack_detail::append_prepacked_ranks_lsb<
            p_storage_unit, 2u, 32u>(
                local_partial_word,
                local_partial_letters,
                appended_letters,
                packed_chunk,
                count,
                write_unit);
    DEBUG_ASSERT(appended && appended_letters == count,
                 "growing SIMD variable rank chunk pack unexpectedly failed");
}

// dna4 encoding for input with  line separators already removed.

/**
 * Appends a dna4 text span using 32 two-bit ranks per uint64_t word.
 *
 * The input should have its line separators removed. drop uses the low-nibble
 * lookup and retries chunks containing skip markers with the scalar table.
 * map_to_default uses the five-bit lookup. These fast lookups assume DNA text
 * and do not check every posscble invalid byte. Partial words stay, in state.
 */
template <typename p_storage_unit,
          std::size_t p_bits_per_character,
          std::size_t p_letters_per_unit,
          unknown_byte_policy p_policy = unknown_byte_policy::drop>
HWY_INLINE void encode_body_chunk_dna4_simd(
    encode_state<p_storage_unit, p_bits_per_character, p_letters_per_unit>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    static_assert(p_bits_per_character == 2 && p_letters_per_unit == 32 &&
                  std::is_same_v<p_storage_unit, std::uint64_t>,
                  "encode_body_chunk_dna4_simd is specialised for "
                  "(dna4, uint64_t, 2, 32)");
    if constexpr (p_policy == unknown_byte_policy::drop) {
        detail::encode_body_chunk_dna4_simd_drop_impl(
            state, body_bytes, body_size);
    } else {
        detail::encode_body_chunk_dna4_simd_map_to_default_impl(
            state, body_bytes, body_size);
    }
}
// dna4 encoding for multiline sequence bodies.

/// Appends multiline dna4 text, skipping line separators.
/// Chunks containing skip markers use scalar filtering. Other chunks use
/// the low-nibble  lookup, which assumes DNA text.
template <typename p_storage_unit,
          std::size_t p_bits_per_character,
          std::size_t p_letters_per_unit>
HWY_INLINE void encode_body_chunk_dna4_simd_multiline(
    encode_state<p_storage_unit, p_bits_per_character, p_letters_per_unit>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    static_assert(p_bits_per_character == 2 && p_letters_per_unit == 32 &&
                  std::is_same_v<p_storage_unit, std::uint64_t>,
                  "encode_body_chunk_dna4_simd_multiline is specialised for "
                  "(dna4, uint64_t, 2, 32)");
    detail::encode_body_chunk_dna4_simd_multiline_impl(
        state, body_bytes, body_size);
}
/// Appends multiline dna4 input up to the first p_boundary_character.
/// Returns bytes consumed, excluding the boundary. If supplied,
/// newline_count_out accumulates each CR and LF byte before that boundary.
template <std::uint8_t p_boundary_character,
          typename p_storage_unit,
          std::size_t p_bits_per_character,
          std::size_t p_letters_per_unit>
[[nodiscard]] HWY_INLINE std::size_t
encode_body_chunk_dna4_simd_multiline_until_byte(
    encode_state<p_storage_unit, p_bits_per_character, p_letters_per_unit>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size,
    std::size_t* HWY_RESTRICT newline_count_out) {
    static_assert(p_bits_per_character == 2 && p_letters_per_unit == 32 &&
                  std::is_same_v<p_storage_unit, std::uint64_t>,
                  "encode_body_chunk_dna4_simd_multiline_until_byte is "
                  "specialised for (dna4, uint64_t, 2, 32)");
    return detail::encode_body_chunk_dna4_simd_multiline_until_byte_impl(
        state, body_bytes, body_size, newline_count_out,
        p_boundary_character);
}
// Decoding dna4 words to ASCII.

/**
 * Decodes total_letters dna4 ranks to ASCII, omitting padding.
 *
 * Processes complete 64-letter blocks with SIMD and handles the tail scalarly.
 * @param words uint64_t words storing the first rank in the low bits.
 * @param dst Output with space for total_letters bytes.
 * @param total_letters Logical sequence length.
 */
HWY_INLINE void decode_dna4_words_to_ascii(
    const std::uint64_t* HWY_RESTRICT words,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t total_letters) noexcept {
    detail::decode_dna4_words_to_ascii_impl(words, dst, total_letters);
}
/**
 * Decodes dna4 words and adds a newline after each full output line.
 *
 * wrap_width must be positive. The initial line count must be at most that
 * width. A final full line also receives a newline.
 * @param words Packed dna4 sequence
 * @param total_letters Number of letters to decode.
 * @param wrap_width Letters per  output line.
 * @param letters_in_current_line Letters already written on this line.
 * @param scratch Temporary space for total_letters decoded bytes.
 * @param dst Space for decoded letters and inserted newlines.
 * @return Updated line count.
 */
HWY_INLINE std::size_t decode_dna4_words_to_ascii_wrapped(
    const std::uint64_t* HWY_RESTRICT words,
    std::size_t total_letters,
    std::size_t wrap_width,
    std::size_t letters_in_current_line,
    std::uint8_t* HWY_RESTRICT scratch,
    std::uint8_t* HWY_RESTRICT dst) noexcept {
    if (total_letters == 0) return letters_in_current_line;
    decode_dna4_words_to_ascii(words, scratch, total_letters);
    std::size_t in_cursor = 0;
    std::size_t out_cursor = 0;
    std::size_t line_position = letters_in_current_line;
    while (in_cursor < total_letters) {
        const std::size_t room_in_line = wrap_width - line_position;
        const std::size_t this_run = std::min(room_in_line,
                                                total_letters - in_cursor);
        std::memcpy(dst + out_cursor, scratch + in_cursor, this_run);
        out_cursor += this_run;
        in_cursor += this_run;
        line_position += this_run;
        if (line_position == wrap_width) {
            dst[out_cursor++] = static_cast<std::uint8_t>('\n');
            line_position = 0;
        }
    }
    return line_position;
}

// dna5 encoding with two ranks per byte.

/// Appends dna5 text with two three-bit ranks in each output byte.
/// Uses the five-bit SIMD lookup and keeps an unfinished pair in state.
/// The lookup assumes DNA text and cannot validate every ASCII byte.
template <unknown_byte_policy p_policy>
HWY_INLINE void encode_body_chunk_dna5_uint8_simd(
    encode_state<std::uint8_t, 3, 2>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    if constexpr (p_policy == unknown_byte_policy::drop) {
        detail::encode_body_chunk_dna5_uint8_simd_drop_impl(
            state, body_bytes, body_size);
    } else {
        detail::encode_body_chunk_dna5_uint8_simd_map_to_default_impl(
            state, body_bytes, body_size);
    }
}
// Decoding dna5 bytes to ASCII.

/**
 * Decodes dna5 storage with two thcee-bit ranks per byte.
 * @param words Packed input.
 * @param dst Output with space for total_letters bytes.
 * @param total_letters Logical sequence length, excluding padding.
 */
HWY_INLINE void decode_dna5_uint8_words_to_ascii(
    const std::uint8_t* HWY_RESTRICT words,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t total_letters) noexcept {
    detail::decode_dna5_uint8_words_to_ascii_impl(words, dst, total_letters);
}
// SIMD lookup with scalar packing for other dna4 and dna5 layouts.

/**
 * Appends dna4 or dna5 text using SIMD lookup and  scalar rank packing.
 *
 * This supports layouts such as 21 dna5 ranks per uint64_t word. Lookup uses
 * the five-bit table, so callers should supply DNA text. The packing helper
 * skips 0xFF ranks under drop and keeps partial words between calls.
 */
template <sequence_alphabet p_alphabet,
          unknown_byte_policy p_policy,
          typename p_storage_unit,
          std::size_t p_bits_per_character,
          std::size_t p_letters_per_unit>
HWY_INLINE void encode_body_chunk_lut_simd_pack_scalar(
    encode_state<p_storage_unit, p_bits_per_character, p_letters_per_unit>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    static_assert(std::is_same_v<p_alphabet, seq::dna4>
               || std::is_same_v<p_alphabet, seq::dna5>,
                  "SIMD LUT pack_scalar path is implemented for dna4/dna5");

    constexpr std::size_t k_scratch_capacity = 1024;
    std::array<std::uint8_t, k_scratch_capacity> rank_scratch{};
    std::size_t read_cursor = 0;
    while (read_cursor < body_size) {
        const std::size_t chunk_size =
            std::min(k_scratch_capacity, body_size - read_cursor);
        if constexpr (std::is_same_v<p_alphabet, seq::dna4>) {
            if constexpr (p_policy == unknown_byte_policy::drop) {
                detail::ascii_to_rank_lut_simd_dna4_drop_impl(
                    body_bytes + read_cursor, rank_scratch.data(), chunk_size);
            } else {
                detail::ascii_to_rank_lut_simd_dna4_map_to_default_impl(
                    body_bytes + read_cursor, rank_scratch.data(), chunk_size);
            }
        } else {
            if constexpr (p_policy == unknown_byte_policy::drop) {
                detail::ascii_to_rank_lut_simd_dna5_drop_impl(
                    body_bytes + read_cursor, rank_scratch.data(), chunk_size);
            } else {
                detail::ascii_to_rank_lut_simd_dna5_map_to_default_impl(
                    body_bytes + read_cursor, rank_scratch.data(), chunk_size);
            }
        }
        append_ranks<p_storage_unit, p_bits_per_character, p_letters_per_unit>(
            state, rank_scratch.data(), chunk_size);
        read_cursor += chunk_size;
    }
}
// Codec selection by alphabet, and packed layout.

/**
 * Appends a chunk using the codec for its alphabet and storage layout.
 *
 * dna4 in uint64_t words and dna5 with two ranks per byte use their specialized
 * codecs. Other dna4 and dna5 layouts combine SIMD lookup with scalar packing.
 * Remaining alphabets use the scalar codec. State keeps unfinished words for the next chunk.
 */
template <sequence_alphabet p_alphabet,
          unknown_byte_policy p_policy = unknown_byte_policy::drop,
          typename p_storage_unit,
          std::size_t p_bits_per_character,
          std::size_t p_letters_per_unit>
HWY_INLINE void encode_body_chunk(
    encode_state<p_storage_unit, p_bits_per_character, p_letters_per_unit>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    constexpr bool k_is_dna4_uint64_shape =
        std::is_same_v<p_alphabet, seq::dna4>
        && std::is_same_v<p_storage_unit, std::uint64_t>
        && p_bits_per_character == 2
        && p_letters_per_unit == 32;
    constexpr bool k_is_dna5_uint8_shape =
        std::is_same_v<p_alphabet, seq::dna5>
        && std::is_same_v<p_storage_unit, std::uint8_t>
        && p_bits_per_character == 3
        && p_letters_per_unit == 2;
    constexpr bool k_is_simd_lut_alphabet =
        std::is_same_v<p_alphabet, seq::dna4>
        || std::is_same_v<p_alphabet, seq::dna5>;

    if constexpr (k_is_dna4_uint64_shape) {
        encode_body_chunk_dna4_simd<p_storage_unit, p_bits_per_character,
                                    p_letters_per_unit, p_policy>(
            state, body_bytes, body_size);
    } else if constexpr (k_is_dna5_uint8_shape) {
        encode_body_chunk_dna5_uint8_simd<p_policy>(
            state, body_bytes, body_size);
    } else if constexpr (k_is_simd_lut_alphabet) {
        encode_body_chunk_lut_simd_pack_scalar<p_alphabet, p_policy,
                                               p_storage_unit,
                                               p_bits_per_character,
                                               p_letters_per_unit>(
            state, body_bytes, body_size);
    } else {
        encode_body_chunk_scalar<p_alphabet, p_policy,
                                 p_storage_unit, p_bits_per_character,
                                 p_letters_per_unit>(
            state, body_bytes, body_size);
    }
}

/**
 * Decodes packed ranks to ASCII using the matching storage layout.
 *
 * dna4 with 32 ranks per uint64_t and dna5 with  two ranks per byte use SIMD.
 * Other layouts use the scalar decoder.
 * @param words Packed sequence storage
 * @param dst Space for total_letters bytes.
 * @param total_letters Logical letters to decode, excluding padding.
 */
template <sequence_alphabet p_alphabet,
          typename p_storage_unit,
          std::size_t p_bits_per_character,
          std::size_t p_letters_per_unit>
HWY_INLINE void decode_words_to_ascii(
    const p_storage_unit* words,
    std::uint8_t* dst,
    std::size_t total_letters) {
    if constexpr (is_dna4_simd_decode_shape<p_alphabet, p_storage_unit,
                                             p_bits_per_character,
                                             p_letters_per_unit>()) {
        decode_dna4_words_to_ascii(
            reinterpret_cast<const std::uint64_t*>(words),
            dst, total_letters);
    } else if constexpr (is_dna5_uint8_simd_decode_shape<
                             p_alphabet,
                             p_storage_unit,
                             p_bits_per_character,
                             p_letters_per_unit>()) {
        decode_dna5_uint8_words_to_ascii(
            reinterpret_cast<const std::uint8_t*>(words),
            dst, total_letters);
    } else {
        decode_words_to_ascii_scalar<p_alphabet, p_storage_unit,
                                     p_bits_per_character,
                                     p_letters_per_unit>(
            words, dst, total_letters);
    }
}

/**
 * Decodes packed ranks into wrapped ASCII lines.
 *
 * wrap_width must be positive. Start with a line count smaller than that width.
 * The SIMD paths write a newline after a final full line. The scalar path leaves
 * that newline to the caler and can return wrap_width, which must be handled
 * before continuing the output.
 * @param words Packed sequence storage.
 * @param total_letters Logical letters to decode.
 * @param wrap_width Letters per output line.
 * @param letters_in_current_line Letters already on this  line.
 * @param scratch Temporary space for total_letters bytes on SIMD paths.
 * @param dst Space for letters and inserted newlines.
 * @return Letters on the last output line.
 */
template <sequence_alphabet p_alphabet,
          typename p_storage_unit,
          std::size_t p_bits_per_character,
          std::size_t p_letters_per_unit>
[[nodiscard]] HWY_INLINE std::size_t decode_words_to_ascii_wrapped(
    const p_storage_unit* words,
    std::size_t total_letters,
    std::size_t wrap_width,
    std::size_t letters_in_current_line,
    std::uint8_t* scratch,
    std::uint8_t* dst) {
    if constexpr (is_dna4_simd_decode_shape<p_alphabet, p_storage_unit,
                                             p_bits_per_character,
                                             p_letters_per_unit>()) {
        return decode_dna4_words_to_ascii_wrapped(
            reinterpret_cast<const std::uint64_t*>(words),
            total_letters, wrap_width,
            letters_in_current_line, scratch, dst);
    } else if constexpr (is_dna5_uint8_simd_decode_shape<
                             p_alphabet,
                             p_storage_unit,
                             p_bits_per_character,
                             p_letters_per_unit>()) {
        // Decode into scratch, then copy each output line with its newline.
        if (total_letters == 0) return letters_in_current_line;
        decode_dna5_uint8_words_to_ascii(
            reinterpret_cast<const std::uint8_t*>(words),
            scratch, total_letters);
        std::size_t in_cursor = 0;
        std::size_t out_cursor = 0;
        std::size_t line_position = letters_in_current_line;
        while (in_cursor < total_letters) {
            const std::size_t room_in_line = wrap_width - line_position;
            const std::size_t this_run = std::min(room_in_line,
                                                    total_letters - in_cursor);
            std::memcpy(dst + out_cursor, scratch + in_cursor, this_run);
            out_cursor += this_run;
            in_cursor += this_run;
            line_position += this_run;
            if (line_position == wrap_width) {
                dst[out_cursor++] = static_cast<std::uint8_t>('\n');
                line_position = 0;
            }
        }
        return line_position;
    } else {
        (void)scratch;
        return decode_words_to_ascii_scalar_wrapped<
            p_alphabet,
            p_storage_unit,
            p_bits_per_character,
            p_letters_per_unit>(
            words, total_letters, wrap_width,
            letters_in_current_line, dst);
    }
}


}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

#undef BIOSEQ_SIMD_X86_PRE_AVX3_FIXED128

}  // export

/**
 * @file codec_kernels.cppm
 * @brief Highway codec primitives and alphabet-specific SIMD dispatch.
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

// ============================================================================
// SIMD primitives — char→rank lookup, dna4 pack, dna5 pack, decode spreads.
// ============================================================================

/// SIMD 32-byte LUT lookup. `lut_low` / `lut_high` are 128-bit-broadcast
/// vectors holding the two halves of a 32-byte table indexed by
/// `chunk[i] & 0x1F`. Result: `lut[chunk & 0x1F]` per lane. Used by
/// the dna4/dna5 SIMD encoders for both `drop` (with 0xFF sentinels)
/// and `map_to_default` (with default-rank fill) lookups.
///
/// Non-ASCII bytes (bit 7 set) are routed to slot 0 of the LUT
/// (which holds the default rank under map-to-default semantics, and the
/// `0xFF` skip-sentinel under drop semantics). Without this clamp, an input
/// byte like `0xD4` (whose `& 0x1F == 20`) would land in the same
/// slot as `'T'` and be silently miscategorised as T — diverging
/// from the 256-entry scalar LUT, which would map `0xD4` to the
/// alphabet's default rank.
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

/// Vector LUT lookup using the 16-entry low-nibble dna4 LUT. Faster
/// than the 32-byte path (1 op vs 5) and used only for `drop` semantics
/// (the LUT maps newlines / unknowns to the `0xFF` sentinel, which the
/// caller's compaction / fallback path then strips).
template <class p_simd_tag>
HWY_INLINE hn::Vec<p_simd_tag> dna4_lookup_ranks(
    p_simd_tag /* tag, deduction-only */,
    hn::Vec<p_simd_tag> letter_to_rank_table_vec,
    hn::Vec<p_simd_tag> low_nibble_mask,
    hn::Vec<p_simd_tag> chunk_bytes) noexcept {
    const auto letter_index = hn::And(chunk_bytes, low_nibble_mask);
    return hn::TableLookupBytes(letter_to_rank_table_vec, letter_index);
}

/// Pack 16 dna4 ranks into a 32-bit word, 2 bits per letter, lowest
/// letter at bit 0. The `ranks` vector MUST contain only values in
/// `[0, 3]` — caller is responsible for screening `0xFF` sentinels.
///
/// Pipeline: variable per-lane left shift puts each rank in its 2-bit
/// slot. `SumsOf4` reduces every 4 adjacent shifted bytes into one
/// packed byte because the slots are disjoint. `Mul +
/// ReduceSum` collapses the 4 packed bytes into one `uint32_t`.
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

/// Detect whether any rank in the 16-byte vector equals the skip
/// sentinel (`0xFF`). Returns `true` if the slow scalar fallback path
/// must run for this chunk.
template <class p_simd_tag>
HWY_INLINE bool has_skip_sentinel(
    p_simd_tag simd_tag,
    hn::Vec<p_simd_tag> ranks) noexcept {
    const auto sentinel_mask =
        hn::Eq(ranks, hn::Set(simd_tag, std::uint8_t{0xFF}));
    return !hn::AllFalse(simd_tag, sentinel_mask);
}

/// Merge a freshly-packed 32-bit chunk into the partial-word
/// accumulator. `local_partial_letters` MUST be in `[0, 32)`.
template <typename p_storage_unit>
HWY_INLINE void merge_packed_chunk_into_partial(
    p_storage_unit& local_partial_word,
    std::size_t& local_partial_letters,
    std::vector<p_storage_unit>* HWY_RESTRICT packed_words,
    std::uint32_t packed_chunk) noexcept {
    std::size_t appended_letters = 0u;
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

/// Variable-count merge of a packed chunk into the partial-word
/// accumulator. `count` is the number of valid 2-bit letter slots in
/// `packed_chunk`. Used by the multi-line dna4 encoder (0..16 letters
/// per chunk after compaction).
template <typename p_storage_unit>
HWY_INLINE void merge_variable_chunk_into_partial(
    p_storage_unit& local_partial_word,
    std::size_t& local_partial_letters,
    std::vector<p_storage_unit>* HWY_RESTRICT packed_words,
    std::uint32_t packed_chunk,
    std::size_t count) noexcept {
    std::size_t appended_letters = 0u;
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

// ============================================================================
// dna4 SIMD encoder — single-line variant (`p_policy`-templated).
// ============================================================================

/// Encode a dna4 body span into the encoder state via the SIMD LUT +
/// SIMD pack pipeline. Single-line variant: caller must strip
/// newlines / CR before passing the span (or use the multiline
/// variant below).
///
/// `p_policy == drop`          — uses the 16-entry low-nibble LUT.
///                               sentinel-detect + scalar fallback for
///                               chunks containing `0xFF`. 2×-unrolled.
/// `p_policy == map_to_default` — uses the 32-byte LUT split across two
///                               16-byte halves. No sentinels can
///                               appear, so no compaction or fallback
///                               is needed.
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
// ============================================================================
// dna4 SIMD encoder — multi-line variants (always `drop`; FSM-only).
// ============================================================================

/// Multi-line dna4 SIMD encoder. Encodes a body span that may contain
/// `\n` and `\r` — the LUT maps these to `0xFF` and the SIMD compaction
/// branch strips them lane-wise without falling back to scalar.
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
/// Fused multiline encode + record-end detection. Encodes from
/// `body_bytes` up to (but not including) the first byte equal to
/// `p_boundary_character`. Returns the number of bytes consumed.
///
/// `newline_count_out` (if non-null) accumulates the count of `\n`
/// and `\r` bytes encountered.
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
// ============================================================================
// dna4 SIMD decoder — packed uint64 → ASCII bytes.
// ============================================================================

/// Decode 16 packed dna4 source bytes (= 2 packed words = 64 letters)
/// into 64 ASCII bytes via SIMD spread + LUT.
/// Decode `total_letters` letters from packed dna4 words into `dst`.
HWY_INLINE void decode_dna4_words_to_ascii(
    const std::uint64_t* HWY_RESTRICT words,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t total_letters) noexcept {
    detail::decode_dna4_words_to_ascii_impl(words, dst, total_letters);
}
/// Variant of `decode_dna4_words_to_ascii` that injects newline bytes
/// every `wrap_width` letters.
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

// ============================================================================
// dna5 SIMD encoder — `(dna5, uint8, 3, 2)`: 16 chars → 8 packed bytes.
// ============================================================================

/// Encode a dna5 body span into the encoder state, with 2 letters per
/// uint8 storage unit (3 bits per letter, 2 leftover bits). Pipeline:
/// 32-byte LUT lookup (handles both policies), then
/// SIMD pack of even/odd lane pairs.
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
// ============================================================================
// dna5 SIMD decoder — `(dna5, uint8, 3, 2)`: 16 packed bytes → 32 chars.
// ============================================================================

HWY_INLINE void decode_dna5_uint8_words_to_ascii(
    const std::uint8_t* HWY_RESTRICT words,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t total_letters) noexcept {
    detail::decode_dna5_uint8_words_to_ascii_impl(words, dst, total_letters);
}
// ============================================================================
// SIMD-lookup + scalar-pack helper for non-canonical dna4/dna5 shapes.
// ============================================================================

/// SIMD-accelerated ASCII→rank lookup, then scalar `append_ranks` for
/// the bit-pack. Used for shapes other than the two canonical ones —
/// notably `(dna5, uint64, 3, 21)` (the IO default for dna5). The
/// lookup is the expensive part of the encode pipeline, so this gives
/// a real speedup over the fully-scalar fallback even though the pack
/// stays scalar.
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
// ============================================================================
// Unified dispatchers.
// ============================================================================

/// Generic streaming encode entry point. The dispatch tree:
///
///   - `(dna4, uint64, 2, 32)`           — full SIMD pipeline
///                                         (`p_policy`-templated).
///   - `(dna5, uint8, 3, 2)`             — full SIMD pipeline
///                                         (`p_policy`-templated).
///   - other dna4/dna5 shapes             — SIMD lookup + scalar pack.
///   - everything else                    — fully scalar.
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
        // Decode to scratch via SIMD then memcpy with newline injection
        // — same shape as the dna4 wrapped path.
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

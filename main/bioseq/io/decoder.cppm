/**
 * @file decoder.cppm
 * @brief Packed-rank → ASCII pipeline for the IO writer.
 *
 * Mirror of `encoder.cppm` for the inverse direction: given a packed
 * `compact_sequence_view<C>` (the v2 reader's view type, holding 2-bit
 * dna4 ranks or 3-bit dna5 ranks per letter), emit the canonical ASCII
 * letters back to a destination buffer.
 *
 * Two paths exist:
 *
 *   1. **Generic scalar** (`decode_words_to_ascii_scalar`) — works for
 *      any `sequence_alphabet`. Extracts ranks bit-by-bit from each
 *      storage unit, looks up the canonical character via the
 *      alphabet's `seqan3::to_char` table, writes ASCII bytes one at a
 *      time. Used for dna5 and any non-canonical alphabet.
 *
 *   2. **dna4 SIMD fast path** lives in `bioseq_io.simd_kernels`
 *      (`dna4_simd_decode_words_to_ascii` / `_wrapped`). The dispatcher
 *      below picks the SIMD path via `if constexpr` on the alphabet
 *      template parameter when the canonical (dna4, uint64_t, 2 bits,
 *      32 letters) shape matches.
 *
 * The decoder is the inverse of `encoder.cppm`'s pipeline. Same
 * Highway-isolation rules apply: this module does not include
 * `<hwy/highway.h>`; the SIMD primitives go through the non-inline
 * wrappers exposed by `bioseq_io.simd_kernels`.
 */
module;

#include "common.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

export module bioseq_io.decoder;

import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_io.simd_kernels;

export namespace bioseq::io {

/// Build the rank → ASCII LUT for `alphabet_t`. Indexed by the
/// alphabet's rank values; entries past the alphabet's cardinality are
/// 0 and unreachable when the input is well-formed.
template <sequence_alphabet alphabet_t>
[[nodiscard]] consteval std::array<std::uint8_t, 256>
build_rank_to_letter_lut() {
    std::array<std::uint8_t, 256> lut{};
    constexpr std::size_t alphabet_card =
        static_cast<std::size_t>(seqan3::alphabet_size<alphabet_t>);
    for (std::size_t rank = 0; rank < alphabet_card; ++rank) {
        const char canonical_char =
            seqan3::to_char(seqan3::assign_rank_to(rank, alphabet_t{}));
        lut[rank] = static_cast<std::uint8_t>(canonical_char);
    }
    return lut;
}

/// Generic scalar decoder. Walks `total_letters` ranks across the
/// packed storage units and writes ASCII bytes to `dst`. Works for any
/// alphabet and any storage_unit / BitsPerChar / LettersPerUnit shape.
template <sequence_alphabet alphabet_t,
          typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
inline void decode_words_to_ascii_scalar(
    const storage_unit_t* __restrict__ words,
    std::uint8_t* __restrict__ dst,
    std::size_t total_letters) noexcept {
    static constexpr auto rank_to_letter_lut = build_rank_to_letter_lut<alphabet_t>();
    constexpr storage_unit_t char_mask = (storage_unit_t{1} << BitsPerChar) - 1;

    std::size_t letters_emitted = 0;
    while (letters_emitted < total_letters) {
        const std::size_t word_index = letters_emitted / LettersPerUnit;
        const storage_unit_t word = words[word_index];
        const std::size_t letters_left_in_word = std::min(
            LettersPerUnit - (letters_emitted % LettersPerUnit),
            total_letters - letters_emitted);
        const std::size_t letter_in_word_start =
            letters_emitted % LettersPerUnit;
        for (std::size_t letter_offset = 0;
             letter_offset < letters_left_in_word; ++letter_offset) {
            const std::size_t letter_in_word =
                letter_in_word_start + letter_offset;
            const std::uint8_t rank = static_cast<std::uint8_t>(
                (word >> (BitsPerChar * letter_in_word)) & char_mask);
            dst[letters_emitted + letter_offset] = rank_to_letter_lut[rank];
        }
        letters_emitted += letters_left_in_word;
    }
}

/// Same as `decode_words_to_ascii_scalar` but injects `\n` every
/// `wrap_width` letters. Returns the new in-line letter counter so
/// callers can resume mid-line across multiple decode batches.
/// Mirrors the SIMD `_wrapped` variant exposed by
/// `bioseq_io.simd_kernels`.
template <sequence_alphabet alphabet_t,
          typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
[[nodiscard]] inline std::size_t decode_words_to_ascii_scalar_wrapped(
    const storage_unit_t* __restrict__ words,
    std::size_t total_letters,
    std::size_t wrap_width,
    std::size_t letters_in_current_line,
    std::uint8_t* __restrict__ dst) noexcept {
    static constexpr auto rank_to_letter_lut = build_rank_to_letter_lut<alphabet_t>();
    constexpr storage_unit_t char_mask = (storage_unit_t{1} << BitsPerChar) - 1;

    std::size_t letters_emitted = 0;
    std::size_t out_cursor = 0;
    std::size_t line_position = letters_in_current_line;
    while (letters_emitted < total_letters) {
        const std::size_t word_index = letters_emitted / LettersPerUnit;
        const std::size_t letter_in_word = letters_emitted % LettersPerUnit;
        const storage_unit_t word = words[word_index];
        const std::uint8_t rank = static_cast<std::uint8_t>(
            (word >> (BitsPerChar * letter_in_word)) & char_mask);
        dst[out_cursor++] = rank_to_letter_lut[rank];
        ++letters_emitted;
        ++line_position;
        if (line_position == wrap_width &&
            letters_emitted < total_letters) {
            dst[out_cursor++] = static_cast<std::uint8_t>('\n');
            line_position = 0;
        }
    }
    return line_position;
}

/// Whether the alphabet/storage shape qualifies for the dna4 SIMD
/// decoder fast path.
template <sequence_alphabet alphabet_t,
          typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
constexpr bool is_dna4_simd_decode_shape() {
    return std::is_same_v<alphabet_t, seq::dna4> &&
           std::is_same_v<storage_unit_t, std::uint64_t> &&
           BitsPerChar == 2 &&
           LettersPerUnit == 32;
}

/// Dispatcher: pick SIMD dna4 path or scalar fallback via `if
/// constexpr`. Same shape as `encoder_dispatch::encode_body_chunk`.
template <sequence_alphabet alphabet_t,
          typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
inline void decode_words_to_ascii(
    const storage_unit_t* words,
    std::uint8_t* dst,
    std::size_t total_letters) {
    if constexpr (is_dna4_simd_decode_shape<alphabet_t, storage_unit_t,
                                              BitsPerChar, LettersPerUnit>()) {
        bioseq::io::simd::dna4_simd_decode_words_to_ascii(
            words, dst, total_letters);
    } else {
        decode_words_to_ascii_scalar<alphabet_t, storage_unit_t,
                                       BitsPerChar, LettersPerUnit>(
            words, dst, total_letters);
    }
}

/// Dispatcher for line-wrapped decoding. SIMD path consumes a scratch
/// buffer; scalar path inlines the wrap during the bit walk.
template <sequence_alphabet alphabet_t,
          typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
[[nodiscard]] inline std::size_t decode_words_to_ascii_wrapped(
    const storage_unit_t* words,
    std::size_t total_letters,
    std::size_t wrap_width,
    std::size_t letters_in_current_line,
    std::uint8_t* scratch,
    std::uint8_t* dst) {
    if constexpr (is_dna4_simd_decode_shape<alphabet_t, storage_unit_t,
                                              BitsPerChar, LettersPerUnit>()) {
        return bioseq::io::simd::dna4_simd_decode_words_to_ascii_wrapped(
            words, total_letters, wrap_width,
            letters_in_current_line, scratch, dst);
    } else {
        (void)scratch;
        return decode_words_to_ascii_scalar_wrapped<alphabet_t, storage_unit_t,
                                                       BitsPerChar, LettersPerUnit>(
            words, total_letters, wrap_width,
            letters_in_current_line, dst);
    }
}

}  // namespace bioseq::io

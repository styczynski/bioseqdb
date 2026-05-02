module;

#include <hwy/highway.h>

#include "common.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

export module bioseq_io.encoder;

import mlogging;
import sequan_compat;
import bioseq_sequences;

export namespace bioseq::io {

/// Build the 256-entry ASCII → rank LUT for `alphabet_t`. Skip-sentinel
/// `0xFF` for whitespace and any byte that does not map to a rank.
/// Computed at compile time; the LUT is per-alphabet. Spec §8.6.
template <sequence_alphabet alphabet_t>
[[nodiscard]] consteval std::array<std::uint8_t, 256>
build_letter_to_rank_lut() {
    std::array<std::uint8_t, 256> lut{};
    for (std::size_t i = 0; i < 256; ++i) lut[i] = 0xFF;
    constexpr std::size_t alphabet_card =
        static_cast<std::size_t>(seqan3::alphabet_size<alphabet_t>);
    for (std::size_t rank = 0; rank < alphabet_card; ++rank) {
        const char canonical_char =
            seqan3::to_char(seqan3::assign_rank_to(rank, alphabet_t{}));
        const std::uint8_t upper_byte = static_cast<std::uint8_t>(canonical_char);
        lut[upper_byte] = static_cast<std::uint8_t>(rank);
        // Lowercase variant: ASCII A-Z → a-z is +0x20.
        if (canonical_char >= 'A' && canonical_char <= 'Z') {
            lut[upper_byte | 0x20] = static_cast<std::uint8_t>(rank);
        }
        if (canonical_char >= 'a' && canonical_char <= 'z') {
            lut[upper_byte & 0x5F] = static_cast<std::uint8_t>(rank);
        }
    }
    return lut;
}

/// Per-record encoder state. Carries the partial `uint64_t` word being
/// assembled across encoder calls so a record body that arrives in
/// chunks (multi-line FASTA, multiple slabs) is packed correctly.
/// Spec §7.4.
template <typename storage_unit_t, std::size_t BitsPerChar, std::size_t LettersPerUnit>
struct encode_state {
    using base_container = std::vector<storage_unit_t>;
    base_container packed_words;
    storage_unit_t partial_word = 0;
    std::size_t partial_letters = 0;
    std::size_t total_letters = 0;

    void reset() noexcept {
        packed_words.clear();
        partial_word = 0;
        partial_letters = 0;
        total_letters = 0;
    }
};

/// Append `count` ranks to the encoder state, emitting full
/// `storage_unit_t` words to `state.packed_words` whenever
/// `partial_letters == LettersPerUnit`. `0xFF` skip-sentinels are
/// silently dropped. Spec §11.4 / §11.5.
template <typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
inline void append_ranks(
    encode_state<storage_unit_t, BitsPerChar, LettersPerUnit>& state,
    const std::uint8_t* ranks,
    std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t rank = ranks[i];
        if (rank == 0xFF) continue;
        state.partial_word |= static_cast<storage_unit_t>(rank)
                              << (BitsPerChar * state.partial_letters);
        state.partial_letters += 1;
        state.total_letters += 1;
        if (state.partial_letters == LettersPerUnit) {
            state.packed_words.push_back(state.partial_word);
            state.partial_word = 0;
            state.partial_letters = 0;
        }
    }
}

/// Drop everything in the state. Used between records.
template <typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
inline void reset_encode_state(
    encode_state<storage_unit_t, BitsPerChar, LettersPerUnit>& state) noexcept {
    state.reset();
}

/// Finalise the encoder: append a partial word if any and return the
/// `(packed_words, padding)` pair the `compact_sequence_range::from_packed`
/// factory expects. Spec Appendix H.
template <typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
[[nodiscard]] inline std::pair<std::vector<storage_unit_t>, std::size_t>
finalize_encode_state(
    encode_state<storage_unit_t, BitsPerChar, LettersPerUnit>& state) noexcept {
    std::vector<storage_unit_t> packed = std::move(state.packed_words);
    std::size_t padding = 0;
    if (state.partial_letters > 0) {
        packed.push_back(state.partial_word);
        padding = LettersPerUnit - state.partial_letters;
    }
    state.partial_word = 0;
    state.partial_letters = 0;
    state.total_letters = 0;
    return {std::move(packed), padding};
}

/// Convert `size` ASCII bytes to ranks via the alphabet's LUT, write
/// them into `dst`. Skip-sentinel bytes for whitespace are
/// `0xFF`; the encoder filters those out. Scalar implementation
/// (correct for every alphabet); the dna4 fast path can be added
/// later as an `if constexpr` specialisation. Spec §8.6.
template <sequence_alphabet alphabet_t>
inline void ascii_to_rank_scalar(
    const std::uint8_t* HWY_RESTRICT src,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t size) noexcept {
    static constexpr auto lut = build_letter_to_rank_lut<alphabet_t>();
    for (std::size_t i = 0; i < size; ++i) {
        dst[i] = lut[src[i]];
    }
}

/// Encode a contiguous span of body bytes into the per-record state.
/// Path: ascii_to_rank → append_ranks. Used by the FASTA / FASTQ FSMs.
/// `body_bytes` MUST NOT contain `\n` (the caller strips line endings
/// before passing the span). For dna4 fast path, every byte is a valid
/// letter; for dna5+ the LUT may emit `0xFF` for `N` etc. Spec §11.1.
template <sequence_alphabet alphabet_t,
          typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
inline void encode_body_chunk(
    encode_state<storage_unit_t, BitsPerChar, LettersPerUnit>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    constexpr std::size_t scratch_capacity = 1024;
    std::array<std::uint8_t, scratch_capacity> rank_scratch{};
    std::size_t cursor = 0;
    while (cursor < body_size) {
        const std::size_t chunk_size =
            std::min(scratch_capacity, body_size - cursor);
        ascii_to_rank_scalar<alphabet_t>(
            body_bytes + cursor,
            rank_scratch.data(),
            chunk_size);
        append_ranks<storage_unit_t, BitsPerChar, LettersPerUnit>(
            state, rank_scratch.data(), chunk_size);
        cursor += chunk_size;
    }
}

}  // namespace bioseq::io

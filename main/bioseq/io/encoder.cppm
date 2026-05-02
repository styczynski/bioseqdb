/**
 * @file encoder.cppm
 * @brief ASCII → rank → packed-uint64_t pipeline for the IO reader.
 *
 * Two paths exist:
 *
 *   1. **Generic scalar** (`encode_body_chunk_scalar`) — works for any
 *      `sequence_alphabet`. The 256-entry LUT is built at compile time
 *      from the alphabet's `to_char` table. `0xFF` skip-sentinels
 *      stand for whitespace / unknown bytes; `append_ranks` filters
 *      them out.
 *
 *   2. **dna4 SIMD fast path** lives in `bioseq_io.simd_kernels`,
 *      with the dispatcher in `bioseq_io.encoder_dispatch::encode_body_chunk`
 *      picking between the two paths via `if constexpr` on the
 *      alphabet template parameter.
 *
 * Highway is intentionally NOT included here — the only Highway
 * facility this module would consume is the `HWY_RESTRICT` macro for
 * pointer annotations, but bringing `<hwy/highway.h>` into the GMF of
 * a non-SIMD module re-emits Highway's per-target inline operator
 * overloads (NEON `operator==(Vec128…)`, AVX2 equivalents) into this
 * BMI; combining that BMI with `simd_kernels`' BMI in a downstream TU
 * trips clang's "definition with same mangled name" check on AArch64.
 * The scalar paths use plain `__restrict__` directly via the
 * alphabet-templated overloads below.
 *
 * Spec sections: §8.6 (LUT builder), §11.1 (three-layer model),
 * §11.4 (partial word handling), §11.5 (`from_packed` materialisation).
 */
module;

#include "common.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq_io.encoder;

import mlogging;
import sequan_compat;
import bioseq_sequences;

export namespace bioseq::io {

// =====================================================================
// LUT builders
// =====================================================================

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
        if (canonical_char >= 'A' && canonical_char <= 'Z') {
            lut[upper_byte | 0x20] = static_cast<std::uint8_t>(rank);
        }
        if (canonical_char >= 'a' && canonical_char <= 'z') {
            lut[upper_byte & 0x5F] = static_cast<std::uint8_t>(rank);
        }
    }
    return lut;
}

/// Build the 16-entry low-nibble LUT for the dna4 fast path. Spec §11.2.
/// `'A' & 0x0F = 0x01`, `'C' & 0x0F = 0x03`, `'G' & 0x0F = 0x07`,
/// `'T' & 0x0F = 0x04`. Newline (`'\n' & 0x0F = 0x0A`) and CR (`'\r' &
/// 0x0F = 0x0D`) map to the skip-sentinel `0xFF`.
[[nodiscard]] consteval std::array<std::uint8_t, 16>
build_dna4_low_nibble_lut() {
    std::array<std::uint8_t, 16> lut{};
    for (std::size_t i = 0; i < 16; ++i) lut[i] = 0xFF;
    lut[0x01] = 0;  // 'A' & 0x0F == 'a' & 0x0F == 0x01
    lut[0x03] = 1;  // 'C' & 0x0F == 'c' & 0x0F == 0x03
    lut[0x07] = 2;  // 'G' & 0x0F == 'g' & 0x0F == 0x07
    lut[0x04] = 3;  // 'T' & 0x0F == 't' & 0x0F == 0x04
    return lut;
}

// =====================================================================
// Per-record encoder state
// =====================================================================

/// Per-record encoder state. Carries the partial `uint64_t` word being
/// assembled across encoder calls. Letter 0 of the partial group sits
/// at bits `[0, BitsPerChar)`, letter k-1 at bits `[(k-1)*BitsPerChar,
/// k*BitsPerChar)`. Spec §7.4 / §11.4.
template <typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
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

// =====================================================================
// Scalar rank-stream → packed-word folder
// =====================================================================

/// Append `count` ranks to the encoder state. Skip-sentinel `0xFF`
/// bytes are silently dropped. Spec §11.4.
template <typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
inline void append_ranks(
    encode_state<storage_unit_t, BitsPerChar, LettersPerUnit>& state,
    const std::uint8_t* __restrict__ ranks,
    std::size_t count) noexcept {
    DEBUG_ASSERT(state.partial_letters < LettersPerUnit,
                 "encode_state::partial_letters must be < LettersPerUnit on entry");
    for (std::size_t rank_index = 0; rank_index < count; ++rank_index) {
        const std::uint8_t letter_rank = ranks[rank_index];
        if (letter_rank == 0xFF) continue;
        DEBUG_ASSERT(letter_rank < (1u << BitsPerChar),
                     "letter rank exceeds BitsPerChar range");
        state.partial_word |= static_cast<storage_unit_t>(letter_rank)
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

template <typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
inline void reset_encode_state(
    encode_state<storage_unit_t, BitsPerChar, LettersPerUnit>& state) noexcept {
    state.reset();
}

/// Finalise the encoder: append a partial word if any and return the
/// `(packed_words, padding)` pair the
/// `compact_sequence_range::from_packed` factory expects. Spec
/// Appendix H / §14.2.
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

// =====================================================================
// Generic scalar ASCII → rank pipeline
// =====================================================================

/// Convert `size` ASCII bytes to ranks via the alphabet's LUT, write
/// them into `dst`. Scalar implementation; correct for every alphabet.
/// Spec §8.6.
template <sequence_alphabet alphabet_t>
inline void ascii_to_rank_scalar(
    const std::uint8_t* __restrict__ src,
    std::uint8_t* __restrict__ dst,
    std::size_t size) noexcept {
    static constexpr auto alphabet_lut = build_letter_to_rank_lut<alphabet_t>();
    for (std::size_t byte_offset = 0; byte_offset < size; ++byte_offset) {
        dst[byte_offset] = alphabet_lut[src[byte_offset]];
    }
}

/// Generic scalar `body_bytes → rank → append_ranks` pipeline. Used by
/// every alphabet other than `seq::dna4`. Spec §11.1.
template <sequence_alphabet alphabet_t,
          typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
inline void encode_body_chunk_scalar(
    encode_state<storage_unit_t, BitsPerChar, LettersPerUnit>& state,
    const std::uint8_t* __restrict__ body_bytes,
    std::size_t body_size) {
    constexpr std::size_t scratch_capacity = 1024;
    std::array<std::uint8_t, scratch_capacity> rank_scratch{};
    std::size_t read_cursor = 0;
    while (read_cursor < body_size) {
        const std::size_t chunk_size =
            std::min(scratch_capacity, body_size - read_cursor);
        ascii_to_rank_scalar<alphabet_t>(
            body_bytes + read_cursor,
            rank_scratch.data(),
            chunk_size);
        append_ranks<storage_unit_t, BitsPerChar, LettersPerUnit>(
            state, rank_scratch.data(), chunk_size);
        read_cursor += chunk_size;
    }
}

}  // namespace bioseq::io

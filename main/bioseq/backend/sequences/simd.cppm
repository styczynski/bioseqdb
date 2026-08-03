module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include "common.h"
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <functional>

#include <boost/integer.hpp>

export module bioseq.sequences.simd;

import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.sequences.rank_pack_core;
import bioseq.common.circular_append_buffer;
import bioseq.storage.lazy;
import bioseq.sequences.simd_iteration_geometry;

import bioseq.simd.utils;
export import bioseq.simd.vector_stream;

export {

// =====================================================================
// Highway-templated code.
// =====================================================================
HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

#if HWY_TARGET == HWY_SSE2 || HWY_TARGET == HWY_SSSE3 || \
    HWY_TARGET == HWY_SSE4 || HWY_TARGET == HWY_AVX2
#define BIOSEQ_SIMD_X86_PRE_AVX3_FIXED128 1
#else
#define BIOSEQ_SIMD_X86_PRE_AVX3_FIXED128 0
#endif


/// Behaviour for ASCII bytes that don't map to a valid alphabet rank.
///
/// `DROP`            — used by the file-input FSM (FASTA / FASTQ).
///                     Newlines, whitespace, and unknown bytes are
///                     silently skipped so the body can be passed
///                     unchunked.
/// `MAP_TO_DEFAULT`  — used by the SQL `seq_in` literal parser.
///                     Mirrors the legacy seqan3 behaviour where
///                     unknown bytes resolve to the alphabet's
///                     default rank (A for dna4, N for dna5).
enum class unknown_byte_policy {
    DROP,
    MAP_TO_DEFAULT,
};

/// Per-record encoder state. The encoder writes packed words directly
/// into `*output_storage` (a caller-owned `std::vector<storage_unit_t>`,
/// typically the IO batch's shared sequence buffer or a per-call vector
/// for the SQL one-shot adapter). Across records the partial-word
/// state (`partial_word`, `partial_letters`) is reset by `reset()` while
/// the output pointer is preserved.
template <typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
struct encode_state {
    using base_container = std::vector<storage_unit_t>;

    /// Pointer to the destination vector. Set by the caller before
    /// every record (or batch). The encoder appends to `*output_storage`.
    base_container* output_storage = nullptr;
    /// Offset into `*output_storage` where the current record's first
    /// packed word starts. Captured at `reset()`; the FSM reads it back
    /// in `finalize_encode_state` to compute the record's slice.
    std::size_t record_start_offset = 0;
    /// Bits accumulated for the partial word that has not yet been
    /// pushed into `*output_storage`.
    storage_unit_t partial_word = 0;
    /// Number of letters in `partial_word`. Always `< LettersPerUnit`
    /// on call-boundary entry / exit.
    std::size_t partial_letters = 0;
    /// Total letters consumed across the current record (used by FASTQ
    /// to validate quality length).
    std::size_t total_letters = 0;

    /// Reset partial-word state for a new record. The output pointer is
    /// preserved so a single attach call configures the encoder for the
    /// whole batch.
    inline void reset() noexcept {
        record_start_offset =
            (output_storage != nullptr) ? output_storage->size() : 0;
        partial_word = 0;
        partial_letters = 0;
        total_letters = 0;
    }
};

namespace detail {

void encode_body_chunk_dna4_simd_drop_impl(
    encode_state<std::uint64_t, 2, 32>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size);
void encode_body_chunk_dna4_simd_map_to_default_impl(
    encode_state<std::uint64_t, 2, 32>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size);
void encode_body_chunk_dna4_simd_multiline_impl(
    encode_state<std::uint64_t, 2, 32>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size);
[[nodiscard]] std::size_t encode_body_chunk_dna4_simd_multiline_until_byte_impl(
    encode_state<std::uint64_t, 2, 32>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size,
    std::size_t* HWY_RESTRICT newline_count_out,
    std::uint8_t boundary_char);

void encode_body_chunk_dna5_uint8_simd_drop_impl(
    encode_state<std::uint8_t, 3, 2>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size);
void encode_body_chunk_dna5_uint8_simd_map_to_default_impl(
    encode_state<std::uint8_t, 3, 2>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size);

void decode_dna4_words_to_ascii_impl(
    const std::uint64_t* HWY_RESTRICT words,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t total_letters) noexcept;
void decode_dna5_uint8_words_to_ascii_impl(
    const std::uint8_t* HWY_RESTRICT words,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t total_letters) noexcept;

void ascii_to_rank_lutsimd_dna4_drop_impl(
    const std::uint8_t* HWY_RESTRICT src,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t size) noexcept;
void ascii_to_rank_lutsimd_dna4_map_to_default_impl(
    const std::uint8_t* HWY_RESTRICT src,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t size) noexcept;
void ascii_to_rank_lutsimd_dna5_drop_impl(
    const std::uint8_t* HWY_RESTRICT src,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t size) noexcept;
void ascii_to_rank_lutsimd_dna5_map_to_default_impl(
    const std::uint8_t* HWY_RESTRICT src,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t size) noexcept;

}  // namespace detail


/// Build the 256-entry ASCII → rank LUT for `alphabet_t`. Two policies
/// for unknown bytes:
///
/// * `DROP`            — unknown bytes map to `0xFF` skip-sentinel; the
///                       scalar `append_ranks` and the SIMD multiline
///                       compaction path detect and drop them.
/// * `MAP_TO_DEFAULT`  — unknown bytes map to the alphabet's default
///                       rank (A for dna4, N for dna5). Mirrors the
///                       legacy seqan3-driven SQL behaviour.
///
/// Case folding (`A`↔`a`, `C`↔`c`, …) is applied via the bit-5 case
/// flip. RNA `U` / `u` is mapped to T's rank so RNA literals decode
/// correctly under both alphabets.
template <sequence_alphabet alphabet_t,
          unknown_byte_policy Policy = unknown_byte_policy::DROP>
[[nodiscard]] consteval std::array<std::uint8_t, 256>
build_letter_to_rank_lut() {
    std::array<std::uint8_t, 256> lut{};
    constexpr std::size_t alphabet_card =
        static_cast<std::size_t>(seqan3::alphabet_size<alphabet_t>);

    if constexpr (Policy == unknown_byte_policy::DROP) {
        for (std::size_t i = 0; i < 256; ++i) lut[i] = std::uint8_t{0xFF};
    } else {
        std::uint8_t default_rank = 0;
        if constexpr (std::is_same_v<alphabet_t, seq::dna5>) {
            default_rank = 3;  // N
        }
        for (std::size_t i = 0; i < 256; ++i) lut[i] = default_rank;
    }

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
    if constexpr (std::is_same_v<alphabet_t, seq::dna4>
               || std::is_same_v<alphabet_t, seq::dna5>) {
        const auto t_rank = lut[static_cast<std::uint8_t>('T')];
        lut[static_cast<std::uint8_t>('U')] = t_rank;
        lut[static_cast<std::uint8_t>('u')] = t_rank;
    }
    return lut;
}

/// Build the two 16-byte halves of a `c & 0x1F`-indexed 32-byte rank
/// LUT for `alphabet_t` under `Policy`. Returns `{lo, hi}` where
/// `lo[i] = lut32[i]` and `hi[i] = lut32[i + 16]`. Indexed by 5 low
/// bits of the input ASCII byte; case folding is automatic because
/// bit 5 is the case bit.
///
/// The 32-byte LUT is the SIMD-friendly cousin of the 256-byte LUT —
/// it lets the runtime path execute as two 16-byte `TableLookupBytes`
/// + a select on bit 4 (~4-5 SIMD ops total), enabling fully-SIMD
/// `MAP_TO_DEFAULT` lookups on dna4 and dna5.
template <sequence_alphabet alphabet_t,
          unknown_byte_policy Policy>
[[nodiscard]] consteval std::array<std::array<std::uint8_t, 16>, 2>
build_letter_to_rank_lut_32() {
    std::array<std::uint8_t, 32> lut{};

    constexpr std::uint8_t default_byte =
        (Policy == unknown_byte_policy::DROP)
            ? std::uint8_t{0xFF}
            : (std::is_same_v<alphabet_t, seq::dna5>
                ? std::uint8_t{3}    // N
                : std::uint8_t{0});  // A
    for (std::size_t i = 0; i < 32; ++i) lut[i] = default_byte;

    constexpr std::size_t alphabet_card =
        static_cast<std::size_t>(seqan3::alphabet_size<alphabet_t>);
    for (std::size_t rank = 0; rank < alphabet_card; ++rank) {
        const char ch =
            seqan3::to_char(seqan3::assign_rank_to(rank, alphabet_t{}));
        const std::uint8_t idx5 = static_cast<std::uint8_t>(ch) & 0x1F;
        lut[idx5] = static_cast<std::uint8_t>(rank);
    }
    if constexpr (std::is_same_v<alphabet_t, seq::dna4>
               || std::is_same_v<alphabet_t, seq::dna5>) {
        lut[static_cast<std::uint8_t>('U') & 0x1F] =
            lut[static_cast<std::uint8_t>('T') & 0x1F];
    }

    std::array<std::array<std::uint8_t, 16>, 2> halves{};
    for (std::size_t i = 0; i < 16; ++i) halves[0][i] = lut[i];
    for (std::size_t i = 0; i < 16; ++i) halves[1][i] = lut[i + 16];
    return halves;
}

/// Build the 256-entry rank → ASCII LUT for `alphabet_t`. Indexed by
/// rank values; entries past the alphabet's cardinality are 0 and
/// unreachable when the input is well-formed.
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

/// Build the 16-entry low-nibble dna4 LUT used by the dna4 SIMD
/// `DROP` fast path. `'A' & 0x0F = 1`, `'C' & 0x0F = 3`,
/// `'G' & 0x0F = 7`, `'T' & 0x0F = 4` — all distinct, lets us do
/// `TableLookupBytes(table_vec, chunk & 0x0F)` in one SIMD op.
[[nodiscard]] consteval std::array<std::uint8_t, 16>
build_dna4_low_nibble_lut() {
    std::array<std::uint8_t, 16> lut{};
    for (std::size_t i = 0; i < 16; ++i) lut[i] = std::uint8_t{0xFF};
    lut[0x01] = 0;  // 'A' & 0x0F == 'a' & 0x0F
    lut[0x03] = 1;  // 'C'
    lut[0x07] = 2;  // 'G'
    lut[0x04] = 3;  // 'T'
    return lut;
}

/// Build the 16-entry rank → ASCII LUT for the dna4 SIMD decoder.
/// Indexed by ranks in [0..3]; remaining entries are zero.
[[nodiscard]] consteval std::array<std::uint8_t, 16>
build_dna4_rank_to_ascii_lut() {
    std::array<std::uint8_t, 16> lut{};
    lut[0] = static_cast<std::uint8_t>('A');
    lut[1] = static_cast<std::uint8_t>('C');
    lut[2] = static_cast<std::uint8_t>('G');
    lut[3] = static_cast<std::uint8_t>('T');
    return lut;
}

/// Build the 16-entry rank → ASCII LUT for the dna5 SIMD decoder.
/// Ranks: A=0, C=1, G=2, N=3, T=4. Trailing entries are zero.
[[nodiscard]] consteval std::array<std::uint8_t, 16>
build_dna5_rank_to_ascii_lut() {
    std::array<std::uint8_t, 16> lut{};
    lut[0] = static_cast<std::uint8_t>('A');
    lut[1] = static_cast<std::uint8_t>('C');
    lut[2] = static_cast<std::uint8_t>('G');
    lut[3] = static_cast<std::uint8_t>('N');
    lut[4] = static_cast<std::uint8_t>('T');
    return lut;
}

// ---------------------------------------------------------------------
// Lane-compaction tables for the dna4 multi-line SIMD encoder. Each
// entry of `kCompactLowTable` (256 × 16 bytes) is a `TableLookupBytes`
// shuffle pattern that gathers the valid bytes from the low half of a
// 16-byte rank vector to the front. `kCompactHighTable` does the same
// for the high half. `kShiftLeftByteTable[N]` shifts the high half
// left by `N` bytes so it slots into the low half's tail.
// ---------------------------------------------------------------------

[[nodiscard]] consteval std::array<std::array<std::uint8_t, 16>, 256>
build_compact_low_table() {
    std::array<std::array<std::uint8_t, 16>, 256> table{};
    for (int mask = 0; mask < 256; ++mask) {
        std::array<std::uint8_t, 16> pattern{};
        std::size_t out_index = 0;
        for (int input_index = 0; input_index < 8; ++input_index) {
            if ((mask >> input_index) & 1) {
                pattern[out_index] = static_cast<std::uint8_t>(input_index);
                ++out_index;
            }
        }
        for (std::size_t fill = out_index; fill < 16; ++fill) {
            pattern[fill] = std::uint8_t{0xFF};
        }
        table[static_cast<std::size_t>(mask)] = pattern;
    }
    return table;
}

[[nodiscard]] consteval std::array<std::array<std::uint8_t, 16>, 256>
build_compact_high_table() {
    std::array<std::array<std::uint8_t, 16>, 256> table{};
    for (int mask = 0; mask < 256; ++mask) {
        std::array<std::uint8_t, 16> pattern{};
        std::size_t out_index = 0;
        for (int input_index = 0; input_index < 8; ++input_index) {
            if ((mask >> input_index) & 1) {
                pattern[out_index] =
                    static_cast<std::uint8_t>(input_index + 8);
                ++out_index;
            }
        }
        for (std::size_t fill = out_index; fill < 16; ++fill) {
            pattern[fill] = std::uint8_t{0xFF};
        }
        table[static_cast<std::size_t>(mask)] = pattern;
    }
    return table;
}

[[nodiscard]] consteval std::array<std::array<std::uint8_t, 16>, 9>
build_shift_left_byte_table() {
    std::array<std::array<std::uint8_t, 16>, 9> table{};
    for (std::size_t shift = 0; shift < 9; ++shift) {
        std::array<std::uint8_t, 16> pattern{};
        for (std::size_t lane = 0; lane < 16; ++lane) {
            pattern[lane] = (lane >= shift)
                ? static_cast<std::uint8_t>(lane - shift)
                : std::uint8_t{0xFF};
        }
        table[shift] = pattern;
    }
    return table;
}

inline constexpr auto kCompactLowTable = build_compact_low_table();
inline constexpr auto kCompactHighTable = build_compact_high_table();
inline constexpr auto kShiftLeftByteTable = build_shift_left_byte_table();

// ---------------------------------------------------------------------
// Streaming primitives — scalar, no Highway dependency. Used as the
// generic fallback for non-(dna4/dna5) alphabets and as the bookkeeping
// layer for every dna4/dna5 SIMD path.
// ---------------------------------------------------------------------

/// Append `count` ranks to the encoder state, packing them into
/// `partial_word` and pushing completed `storage_unit_t`s into
/// `*output_storage`. Skip-sentinel `0xFF` bytes are silently dropped.
template <typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
inline void append_ranks(
    encode_state<storage_unit_t, BitsPerChar, LettersPerUnit>& state,
    const std::uint8_t* __restrict__ ranks,
    std::size_t count) noexcept {
    DEBUG_ASSERT(state.output_storage != nullptr,
                 "encode_state::output_storage must be configured before append_ranks");
    DEBUG_ASSERT(state.partial_letters < LettersPerUnit,
                 "encode_state::partial_letters must be < LettersPerUnit on entry");
    auto* output = state.output_storage;
    auto write_unit = [output](storage_unit_t value) {
        output->push_back(value);
        return true;
    };
    for (std::size_t rank_index = 0; rank_index < count; ++rank_index) {
        const std::uint8_t letter_rank = ranks[rank_index];
        if (letter_rank == 0xFF) continue;
        DEBUG_ASSERT(letter_rank < (1u << BitsPerChar),
                     "letter rank exceeds BitsPerChar range");
        const bool appended =
            bioseq::sequences::rank_pack_detail::append_valid_rank_lsb<
                storage_unit_t, BitsPerChar, LettersPerUnit>(
                    state.partial_word,
                    state.partial_letters,
                    state.total_letters,
                    letter_rank,
                    write_unit);
        DEBUG_ASSERT(appended,
                     "growing SIMD rank pack unexpectedly overflowed");
    }
}

template <typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
inline void reset_encode_state(
    encode_state<storage_unit_t, BitsPerChar, LettersPerUnit>& state) noexcept {
    state.reset();
}

/// Finalise the encoder for the current record. Pushes the trailing
/// partial word (if any) into `*output_storage` and returns the
/// `(word_count, padding)` pair the FSM needs to record this record's
/// slice. Padding is the number of empty letter slots in the trailing
/// unit (or 0 if the record fills its last unit exactly).
template <typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
[[nodiscard]] inline std::pair<std::size_t, std::size_t>
finalize_encode_state(
    encode_state<storage_unit_t, BitsPerChar, LettersPerUnit>& state) noexcept {
    DEBUG_ASSERT(state.output_storage != nullptr,
                 "encode_state::output_storage must be configured before finalize");
    auto* output = state.output_storage;
    auto write_unit = [output](storage_unit_t value) {
        output->push_back(value);
        return true;
    };
    const auto finished =
        bioseq::sequences::rank_pack_detail::finish_lsb<
            storage_unit_t, BitsPerChar, LettersPerUnit>(
                state.partial_word, state.partial_letters, write_unit);
    DEBUG_ASSERT(finished.capacity_ok,
                 "growing SIMD rank pack unexpectedly overflowed");
    const std::size_t n_words = output->size() - state.record_start_offset;
    state.total_letters = 0;
    return {n_words, finished.padding_letters};
}

/// Convert `size` ASCII bytes to ranks via the alphabet's 256-entry
/// LUT. Generic scalar implementation; correct for every alphabet.
template <sequence_alphabet alphabet_t,
          unknown_byte_policy Policy = unknown_byte_policy::DROP>
inline void ascii_to_rank_scalar(
    const std::uint8_t* __restrict__ src,
    std::uint8_t* __restrict__ dst,
    std::size_t size) noexcept {
    static constexpr auto alphabet_lut =
        build_letter_to_rank_lut<alphabet_t, Policy>();
    for (std::size_t byte_offset = 0; byte_offset < size; ++byte_offset) {
        dst[byte_offset] = alphabet_lut[src[byte_offset]];
    }
}

/// Generic scalar `body_bytes → rank → append_ranks` pipeline. Used by
/// every alphabet shape that doesn't have a dedicated SIMD path.
template <sequence_alphabet alphabet_t,
          unknown_byte_policy Policy,
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
        ascii_to_rank_scalar<alphabet_t, Policy>(
            body_bytes + read_cursor,
            rank_scratch.data(),
            chunk_size);
        append_ranks<storage_unit_t, BitsPerChar, LettersPerUnit>(
            state, rank_scratch.data(), chunk_size);
        read_cursor += chunk_size;
    }
}

/// Generic scalar decoder. Walks `total_letters` ranks across the
/// packed storage units and writes ASCII bytes to `dst`. Works for any
/// alphabet and any storage shape.
template <sequence_alphabet alphabet_t,
          typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
inline void decode_words_to_ascii_scalar(
    const storage_unit_t* __restrict__ words,
    std::uint8_t* __restrict__ dst,
    std::size_t total_letters) noexcept {
    static constexpr auto rank_to_letter_lut =
        build_rank_to_letter_lut<alphabet_t>();
    constexpr storage_unit_t char_mask =
        static_cast<storage_unit_t>(
            (storage_unit_t{1} << BitsPerChar) - 1);

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
    static constexpr auto rank_to_letter_lut =
        build_rank_to_letter_lut<alphabet_t>();
    constexpr storage_unit_t char_mask =
        static_cast<storage_unit_t>(
            (storage_unit_t{1} << BitsPerChar) - 1);

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

/// Constexpr predicate: does `(alphabet, storage, BPC, LPU)` match the
/// canonical dna4 SIMD-decode shape `(dna4, uint64_t, 2, 32)`?
template <sequence_alphabet alphabet_t,
          typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
constexpr bool is_dna4_simd_decode_shape() noexcept {
    return std::is_same_v<alphabet_t, seq::dna4>
        && std::is_same_v<storage_unit_t, std::uint64_t>
        && BitsPerChar == 2
        && LettersPerUnit == 32;
}

/// Constexpr predicate: does `(alphabet, storage, BPC, LPU)` match the
/// canonical dna5+uint8 SIMD-decode shape `(dna5, uint8_t, 3, 2)`?
template <sequence_alphabet alphabet_t,
          typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
constexpr bool is_dna5_uint8_simd_decode_shape() noexcept {
    return std::is_same_v<alphabet_t, seq::dna5>
        && std::is_same_v<storage_unit_t, std::uint8_t>
        && BitsPerChar == 3
        && LettersPerUnit == 2;
}

namespace detail {
	template<typename T, typename F, std::size_t... Is>
	constexpr auto generate_array(F& f, std::index_sequence<Is...>)
		-> std::array<T, sizeof...(Is)> {
		return {{f(std::integral_constant<std::size_t, Is>{})...}};
	}
}

template<typename T, std::size_t N, typename F>
constexpr auto generate_array(F f) {
return detail::generate_array<T>(f, std::make_index_sequence<N>{});
}

}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

}  // export

// Compile the independent static facade from the same canonical target body.
// This textual replay happens after HWY_TARGET is fixed and therefore does
// not move vector-bearing templates or ABI across a module boundary.
HWY_BEFORE_NAMESPACE();

#define BIOSEQ_SEQUENCE_OPS_STATIC_FACADE 1
#define target HWY_NAMESPACE
// Suppress only the canonical source's required leading global fragment.
#define module
#include "highway_sequence_ops.cppm"
#undef module
#undef target
#undef BIOSEQ_SEQUENCE_OPS_STATIC_FACADE

HWY_AFTER_NAMESPACE();

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
// ============================================================================
// SIMD primitives — char→rank lookup, dna4 pack, dna5 pack, decode spreads.
// ============================================================================

/// SIMD 32-byte LUT lookup. `lut_lo` / `lut_hi` are 128-bit-broadcast
/// vectors holding the two halves of a 32-byte table indexed by
/// `chunk[i] & 0x1F`. Result: `lut[chunk & 0x1F]` per lane. Used by
/// the dna4/dna5 SIMD encoders for both `DROP` (with 0xFF sentinels)
/// and `MAP_TO_DEFAULT` (with default-rank fill) lookups.
///
/// Non-ASCII bytes (bit 7 set) are routed to slot 0 of the LUT
/// (which holds the default rank under MAP_TO_DEFAULT, and the
/// `0xFF` skip-sentinel under DROP). Without this clamp, an input
/// byte like `0xD4` (whose `& 0x1F == 20`) would land in the same
/// slot as `'T'` and be silently miscategorised as T — diverging
/// from the 256-entry scalar LUT, which would map `0xD4` to the
/// alphabet's default rank.
template <class D>
HWY_INLINE hn::Vec<D> lookup_ranks_via_32byte_lut(
    D d, hn::Vec<D> lut_lo, hn::Vec<D> lut_hi, hn::Vec<D> chunk) noexcept {
    const auto high_bit = hn::And(chunk, hn::Set(d, std::uint8_t{0x80}));
    const auto is_non_ascii = hn::Ne(high_bit, hn::Zero(d));
    const auto safe = hn::IfThenZeroElse(is_non_ascii, chunk);

    const auto idx_low_4 = hn::And(safe, hn::Set(d, std::uint8_t{0x0F}));
    const auto lo_result = hn::TableLookupBytes(lut_lo, idx_low_4);
    const auto hi_result = hn::TableLookupBytes(lut_hi, idx_low_4);
    const auto bit_4 = hn::And(safe, hn::Set(d, std::uint8_t{0x10}));
    const auto is_high = hn::Ne(bit_4, hn::Zero(d));
    return hn::IfThenElse(is_high, hi_result, lo_result);
}

/// Vector LUT lookup using the 16-entry low-nibble dna4 LUT. Faster
/// than the 32-byte path (1 op vs 5) and used only for `DROP` semantics
/// (the LUT maps newlines / unknowns to the `0xFF` sentinel, which the
/// caller's compaction / fallback path then strips).
template <class D>
HWY_INLINE hn::Vec<D> dna4_lookup_ranks(
    D /* tag, deduction-only */,
    hn::Vec<D> letter_to_rank_table_vec,
    hn::Vec<D> low_nibble_mask,
    hn::Vec<D> chunk_bytes) noexcept {
    const auto letter_index = hn::And(chunk_bytes, low_nibble_mask);
    return hn::TableLookupBytes(letter_to_rank_table_vec, letter_index);
}

/// Pack 16 dna4 ranks into a 32-bit word, 2 bits per letter, lowest
/// letter at bit 0. The `ranks` vector MUST contain only values in
/// `[0, 3]` — caller is responsible for screening `0xFF` sentinels.
///
/// Pipeline: variable per-lane left shift puts each rank in its 2-bit
/// slot; `SumsOf4` reduces every 4 adjacent shifted bytes into one
/// packed byte (since the slots are disjoint, sum == OR); `Mul +
/// ReduceSum` collapses the 4 packed bytes into one `uint32_t`.
template <class D>
HWY_INLINE std::uint32_t pack_16_dna4_to_u32(
    D d_byte,
    hn::Vec<D> ranks) noexcept {
#if BIOSEQ_SIMD_X86_PRE_AVX3_FIXED128
    HWY_ALIGN std::uint8_t scratch[16];
    hn::Store(ranks, d_byte, scratch);
    std::uint32_t packed = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        packed |= static_cast<std::uint32_t>(scratch[i] & 0x03u)
                  << (2 * i);
    }
    return packed;
#else
    HWY_ALIGN constexpr std::uint8_t shift_pattern[16] = {
        0, 2, 4, 6, 0, 2, 4, 6,
        0, 2, 4, 6, 0, 2, 4, 6};
    const auto shift_pattern_v = hn::LoadU(d_byte, shift_pattern);
    const auto shifted = hn::Shl(ranks, shift_pattern_v);

    using d_u32_t = hn::Repartition<std::uint32_t, D>;
    d_u32_t d_u32;
    const auto sums = hn::SumsOf4(shifted);

    HWY_ALIGN constexpr std::uint32_t multiplier_pattern[4] = {
        1u, 1u << 8, 1u << 16, 1u << 24};
    const auto multipliers = hn::LoadU(d_u32, multiplier_pattern);
    const auto multiplied = hn::Mul(sums, multipliers);
    return hn::ReduceSum(d_u32, multiplied);
#endif
}

/// Detect whether any rank in the 16-byte vector equals the skip
/// sentinel (`0xFF`). Returns `true` if the slow scalar fallback path
/// must run for this chunk.
template <class D>
HWY_INLINE bool has_skip_sentinel(
    D d_byte,
    hn::Vec<D> ranks) noexcept {
    const auto sentinel_mask = hn::Eq(ranks, hn::Set(d_byte, std::uint8_t{0xFF}));
    return !hn::AllFalse(d_byte, sentinel_mask);
}

/// Merge a freshly-packed 32-bit chunk into the partial-word
/// accumulator. `local_partial_letters` MUST be in `[0, 32)`.
template <typename storage_unit_t>
HWY_INLINE void merge_packed_chunk_into_partial(
    storage_unit_t& local_partial_word,
    std::size_t& local_partial_letters,
    std::vector<storage_unit_t>* HWY_RESTRICT packed_words,
    std::uint32_t packed_chunk) noexcept {
    std::size_t appended_letters = 0u;
    auto write_unit = [packed_words](storage_unit_t value) {
        packed_words->push_back(value);
        return true;
    };
    const bool appended =
        bioseq::sequences::rank_pack_detail::append_prepacked_ranks_lsb<
            storage_unit_t, 2u, 32u>(
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
template <typename storage_unit_t>
HWY_INLINE void merge_variable_chunk_into_partial(
    storage_unit_t& local_partial_word,
    std::size_t& local_partial_letters,
    std::vector<storage_unit_t>* HWY_RESTRICT packed_words,
    std::uint32_t packed_chunk,
    std::size_t count) noexcept {
    std::size_t appended_letters = 0u;
    auto write_unit = [packed_words](storage_unit_t value) {
        packed_words->push_back(value);
        return true;
    };
    const bool appended =
        bioseq::sequences::rank_pack_detail::append_prepacked_ranks_lsb<
            storage_unit_t, 2u, 32u>(
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
// dna4 SIMD encoder — single-line variant (`Policy`-templated).
// ============================================================================

/// Encode a dna4 body span into the encoder state via the SIMD LUT +
/// SIMD pack pipeline. Single-line variant: caller must strip
/// newlines / CR before passing the span (or use the multiline
/// variant below).
///
/// `Policy == DROP`            — uses the 16-entry low-nibble LUT;
///                               sentinel-detect + scalar fallback for
///                               chunks containing `0xFF`. 2×-unrolled.
/// `Policy == MAP_TO_DEFAULT`  — uses the 32-byte LUT split across two
///                               16-byte halves; no sentinels can
///                               appear, so no compaction or fallback
///                               is needed.
template <typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit,
          unknown_byte_policy Policy = unknown_byte_policy::DROP>
HWY_INLINE void encode_body_chunk_dna4_simd(
    encode_state<storage_unit_t, BitsPerChar, LettersPerUnit>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    static_assert(BitsPerChar == 2 && LettersPerUnit == 32 &&
                  std::is_same_v<storage_unit_t, std::uint64_t>,
                  "encode_body_chunk_dna4_simd is specialised for "
                  "(dna4, uint64_t, 2, 32)");
    if constexpr (Policy == unknown_byte_policy::DROP) {
        detail::encode_body_chunk_dna4_simd_drop_impl(
            state, body_bytes, body_size);
    } else {
        detail::encode_body_chunk_dna4_simd_map_to_default_impl(
            state, body_bytes, body_size);
    }
}
// ============================================================================
// dna4 SIMD encoder — multi-line variants (always `DROP`; FSM-only).
// ============================================================================

/// Multi-line dna4 SIMD encoder. Encodes a body span that may contain
/// `\n` and `\r` — the LUT maps these to `0xFF` and the SIMD compaction
/// branch strips them lane-wise without falling back to scalar.
template <typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
HWY_INLINE void encode_body_chunk_dna4_simd_multiline(
    encode_state<storage_unit_t, BitsPerChar, LettersPerUnit>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    static_assert(BitsPerChar == 2 && LettersPerUnit == 32 &&
                  std::is_same_v<storage_unit_t, std::uint64_t>,
                  "encode_body_chunk_dna4_simd_multiline is specialised for "
                  "(dna4, uint64_t, 2, 32)");
    detail::encode_body_chunk_dna4_simd_multiline_impl(
        state, body_bytes, body_size);
}
/// Fused multiline encode + record-end detection. Encodes from
/// `body_bytes` up to (but not including) the first byte equal to
/// `BoundaryChar`. Returns the number of bytes consumed.
///
/// `newline_count_out` (if non-null) accumulates the count of `\n`
/// and `\r` bytes encountered.
template <std::uint8_t BoundaryChar,
          typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
[[nodiscard]] HWY_INLINE std::size_t
encode_body_chunk_dna4_simd_multiline_until_byte(
    encode_state<storage_unit_t, BitsPerChar, LettersPerUnit>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size,
    std::size_t* HWY_RESTRICT newline_count_out) {
    static_assert(BitsPerChar == 2 && LettersPerUnit == 32 &&
                  std::is_same_v<storage_unit_t, std::uint64_t>,
                  "encode_body_chunk_dna4_simd_multiline_until_byte is "
                  "specialised for (dna4, uint64_t, 2, 32)");
    return detail::encode_body_chunk_dna4_simd_multiline_until_byte_impl(
        state, body_bytes, body_size, newline_count_out, BoundaryChar);
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
/// 32-byte LUT lookup (handles both DROP and MAP_TO_DEFAULT), then
/// SIMD pack of even/odd lane pairs.
template <unknown_byte_policy Policy>
HWY_INLINE void encode_body_chunk_dna5_uint8_simd(
    encode_state<std::uint8_t, 3, 2>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    if constexpr (Policy == unknown_byte_policy::DROP) {
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
template <sequence_alphabet alphabet_t,
          unknown_byte_policy Policy,
          typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
HWY_INLINE void encode_body_chunk_lutsimd_packscalar(
    encode_state<storage_unit_t, BitsPerChar, LettersPerUnit>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    static_assert(std::is_same_v<alphabet_t, seq::dna4>
               || std::is_same_v<alphabet_t, seq::dna5>,
                  "SIMD LUT packscalar path is implemented for dna4/dna5");

    constexpr std::size_t scratch_capacity = 1024;
    std::array<std::uint8_t, scratch_capacity> rank_scratch{};
    std::size_t read_cursor = 0;
    while (read_cursor < body_size) {
        const std::size_t chunk_size =
            std::min(scratch_capacity, body_size - read_cursor);
        if constexpr (std::is_same_v<alphabet_t, seq::dna4>) {
            if constexpr (Policy == unknown_byte_policy::DROP) {
                detail::ascii_to_rank_lutsimd_dna4_drop_impl(
                    body_bytes + read_cursor, rank_scratch.data(), chunk_size);
            } else {
                detail::ascii_to_rank_lutsimd_dna4_map_to_default_impl(
                    body_bytes + read_cursor, rank_scratch.data(), chunk_size);
            }
        } else {
            if constexpr (Policy == unknown_byte_policy::DROP) {
                detail::ascii_to_rank_lutsimd_dna5_drop_impl(
                    body_bytes + read_cursor, rank_scratch.data(), chunk_size);
            } else {
                detail::ascii_to_rank_lutsimd_dna5_map_to_default_impl(
                    body_bytes + read_cursor, rank_scratch.data(), chunk_size);
            }
        }
        append_ranks<storage_unit_t, BitsPerChar, LettersPerUnit>(
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
///                                         (Policy-templated).
///   - `(dna5, uint8, 3, 2)`             — full SIMD pipeline
///                                         (Policy-templated).
///   - other dna4/dna5 shapes             — SIMD lookup + scalar pack.
///   - everything else                    — fully scalar.
template <sequence_alphabet alphabet_t,
          unknown_byte_policy Policy = unknown_byte_policy::DROP,
          typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
HWY_INLINE void encode_body_chunk(
    encode_state<storage_unit_t, BitsPerChar, LettersPerUnit>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    constexpr bool dna4_uint64_shape =
        std::is_same_v<alphabet_t, seq::dna4>
        && std::is_same_v<storage_unit_t, std::uint64_t>
        && BitsPerChar == 2
        && LettersPerUnit == 32;
    constexpr bool dna5_uint8_shape =
        std::is_same_v<alphabet_t, seq::dna5>
        && std::is_same_v<storage_unit_t, std::uint8_t>
        && BitsPerChar == 3
        && LettersPerUnit == 2;
    constexpr bool simd_lut_alphabet =
        std::is_same_v<alphabet_t, seq::dna4>
        || std::is_same_v<alphabet_t, seq::dna5>;

    if constexpr (dna4_uint64_shape) {
        encode_body_chunk_dna4_simd<storage_unit_t, BitsPerChar,
                                       LettersPerUnit, Policy>(
            state, body_bytes, body_size);
    } else if constexpr (dna5_uint8_shape) {
        encode_body_chunk_dna5_uint8_simd<Policy>(
            state, body_bytes, body_size);
    } else if constexpr (simd_lut_alphabet) {
        encode_body_chunk_lutsimd_packscalar<alphabet_t, Policy,
                                                storage_unit_t,
                                                BitsPerChar,
                                                LettersPerUnit>(
            state, body_bytes, body_size);
    } else {
        encode_body_chunk_scalar<alphabet_t, Policy,
                                  storage_unit_t, BitsPerChar,
                                  LettersPerUnit>(
            state, body_bytes, body_size);
    }
}

template <sequence_alphabet alphabet_t,
          typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
HWY_INLINE void decode_words_to_ascii(
    const storage_unit_t* words,
    std::uint8_t* dst,
    std::size_t total_letters) {
    if constexpr (is_dna4_simd_decode_shape<alphabet_t, storage_unit_t,
                                              BitsPerChar, LettersPerUnit>()) {
        decode_dna4_words_to_ascii(
            reinterpret_cast<const std::uint64_t*>(words),
            dst, total_letters);
    } else if constexpr (is_dna5_uint8_simd_decode_shape<alphabet_t,
                                                            storage_unit_t,
                                                            BitsPerChar,
                                                            LettersPerUnit>()) {
        decode_dna5_uint8_words_to_ascii(
            reinterpret_cast<const std::uint8_t*>(words),
            dst, total_letters);
    } else {
        decode_words_to_ascii_scalar<alphabet_t, storage_unit_t,
                                       BitsPerChar, LettersPerUnit>(
            words, dst, total_letters);
    }
}

template <sequence_alphabet alphabet_t,
          typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
[[nodiscard]] HWY_INLINE std::size_t decode_words_to_ascii_wrapped(
    const storage_unit_t* words,
    std::size_t total_letters,
    std::size_t wrap_width,
    std::size_t letters_in_current_line,
    std::uint8_t* scratch,
    std::uint8_t* dst) {
    if constexpr (is_dna4_simd_decode_shape<alphabet_t, storage_unit_t,
                                              BitsPerChar, LettersPerUnit>()) {
        return decode_dna4_words_to_ascii_wrapped(
            reinterpret_cast<const std::uint64_t*>(words),
            total_letters, wrap_width,
            letters_in_current_line, scratch, dst);
    } else if constexpr (is_dna5_uint8_simd_decode_shape<alphabet_t,
                                                            storage_unit_t,
                                                            BitsPerChar,
                                                            LettersPerUnit>()) {
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
        return decode_words_to_ascii_scalar_wrapped<alphabet_t, storage_unit_t,
                                                       BitsPerChar, LettersPerUnit>(
            words, total_letters, wrap_width,
            letters_in_current_line, dst);
    }
}

// ============================================================================
// One-shot adapters. Same public names as before; new bodies built on
// top of the streaming pipeline.
// ============================================================================

/// One-shot scalar encoder. Adapter around `encode_body_chunk_scalar`.
/// Default `Policy = MAP_TO_DEFAULT` preserves the legacy SQL `seq_in`
/// behaviour (silent map of unknown bytes to the alphabet's default
/// rank).
template <class Dst,
          unknown_byte_policy Policy = unknown_byte_policy::MAP_TO_DEFAULT>
requires is_sequence_custom_alphabet<Dst>
[[nodiscard]] compact_sequence_range<Dst> encode_sequence_scalar(
    const char* input, std::size_t length) {
    using seq_t = compact_sequence_range<Dst>;
    using storage_unit_t = typename seq_t::traits::storage_unit;
    using alphabet_t     = typename seq_t::traits::alphabet_type;
    constexpr std::size_t bpc = seq_t::traits::bits_per_char;
    constexpr std::size_t lpu = seq_t::traits::letters_per_unit;

    std::vector<storage_unit_t> packed;
    packed.reserve((length + lpu - 1) / lpu);

    encode_state<storage_unit_t, bpc, lpu> state{};
    state.output_storage = &packed;
    state.reset();
    encode_body_chunk_scalar<alphabet_t, Policy,
                              storage_unit_t, bpc, lpu>(
        state,
        reinterpret_cast<const std::uint8_t*>(input),
        length);
    const auto [n_words, padding] =
        finalize_encode_state<storage_unit_t, bpc, lpu>(state);
    (void)n_words;
    return seq_t::from_packed(std::move(packed), padding);
}

/// One-shot SIMD encoder. Adapter around `encode_body_chunk` —
/// dispatches to the dna4/dna5 SIMD pipeline when applicable.
template <class Dst,
          unknown_byte_policy Policy = unknown_byte_policy::MAP_TO_DEFAULT>
requires is_sequence_custom_alphabet<Dst>
[[nodiscard]] compact_sequence_range<Dst> encode_sequence_simd(
    const char* input, std::size_t length) {
    using seq_t = compact_sequence_range<Dst>;
    using storage_unit_t = typename seq_t::traits::storage_unit;
    using alphabet_t     = typename seq_t::traits::alphabet_type;
    constexpr std::size_t bpc = seq_t::traits::bits_per_char;
    constexpr std::size_t lpu = seq_t::traits::letters_per_unit;

    std::vector<storage_unit_t> packed;
    packed.reserve((length + lpu - 1) / lpu);

    encode_state<storage_unit_t, bpc, lpu> state{};
    state.output_storage = &packed;
    state.reset();
    encode_body_chunk<alphabet_t, Policy,
                       storage_unit_t, bpc, lpu>(
        state,
        reinterpret_cast<const std::uint8_t*>(input),
        length);
    const auto [n_words, padding] =
        finalize_encode_state<storage_unit_t, bpc, lpu>(state);
    (void)n_words;
    return seq_t::from_packed(std::move(packed), padding);
}

/// One-shot scalar decoder. Adapter around
/// `decode_words_to_ascii_scalar` for `is_sequence_custom_alphabet`
/// shapes (which expose `_data.data()` as `const storage_unit_t*`).
/// For other `supports_compact_sequence_range` shapes (e.g.
/// `compact_sequence_range<std::vector<seq::dna4>>`), falls back to
/// the iterator-based path so the function works for every range.
template <class Src>
requires supports_compact_sequence_range<Src>
[[nodiscard]] std::string decode_sequence_scalar(
    const compact_sequence_range<Src>& seq) {
    using seq_traits = typename compact_sequence_range<Src>::traits;
    const std::size_t bp_count = seq.bp_size();
    if (bp_count == 0) return std::string{};

    if constexpr (is_sequence_custom_alphabet<Src> || is_compact_sequence_stdc_instance<Src>) {
        using storage_unit_t = typename seq_traits::storage_unit;
        using alphabet_t     = typename seq_traits::alphabet_type;
        constexpr std::size_t bpc = seq_traits::bits_per_char;
        constexpr std::size_t lpu = seq_traits::letters_per_unit;
        std::string out(bp_count, '\0');
        decode_words_to_ascii_scalar<alphabet_t, storage_unit_t, bpc, lpu>(
            seq.read_cptr(seq.cbegin()),
            reinterpret_cast<std::uint8_t*>(out.data()),
            bp_count);
        return out;
    } else {
        std::string out;
        out.reserve(bp_count);
        for (const auto& rank : seq.template iterable<BP>()) {
            out.push_back(seq_traits::letter_to_char(
                static_cast<typename seq_traits::storage_unit>(rank)));
        }
        return out;
    }
}

/// One-shot SIMD decoder. Adapter around `decode_words_to_ascii` —
/// SIMD path engages for `(dna4, uint64, 2, 32)` and
/// `(dna5, uint8, 3, 2)`; everything else uses the scalar bit-walk.
/// Falls back to the iterator-based scalar path for non-custom-alphabet
/// ranges.
template <class Src>
requires supports_compact_sequence_range<Src>
[[nodiscard]] std::string decode_sequence_simd(
    const compact_sequence_range<Src>& seq) {
    using seq_traits = typename compact_sequence_range<Src>::traits;
    const std::size_t bp_count = seq.bp_size();
    if (bp_count == 0) return std::string{};

    if constexpr (is_sequence_custom_alphabet<Src> || is_compact_sequence_stdc_instance<Src>) {
        using storage_unit_t = typename seq_traits::storage_unit;
        using alphabet_t     = typename seq_traits::alphabet_type;
        constexpr std::size_t bpc = seq_traits::bits_per_char;
        constexpr std::size_t lpu = seq_traits::letters_per_unit;
        std::string out(bp_count, '\0');
        decode_words_to_ascii<alphabet_t, storage_unit_t, bpc, lpu>(
            seq.read_cptr(seq.cbegin()),
            reinterpret_cast<std::uint8_t*>(out.data()),
            bp_count);
        return out;
    } else {
        std::string out;
        out.reserve(bp_count);
        for (const auto& rank : seq.template iterable<BP>()) {
            out.push_back(seq_traits::letter_to_char(
                static_cast<typename seq_traits::storage_unit>(rank)));
        }
        return out;
    }
}

// ============================================================================
// Lexicographic sequence comparator. Renamed from compare_sequences_simd —
// the body is purely scalar, the previous `_simd` suffix was misleading.
// Both sequences must share the same (alphabet, storage_unit).
//
// Constrained against `supports_compact_sequence_bitcast_view<V>` so the
// caller can pass either a raw `compact_sequence_range<R>` or a
// `bit_casted_view<base, output>` — both satisfy the concept (DOC0260
// pre-existing latent bug: the cross-storage-width branch in
// stdc_compare passes a bit_casted_view here, which the previous
// `compact_sequence_range<R1>&` signature failed to match).
// ============================================================================
template<class V1, class V2>
requires supports_compact_sequence_bitcast_view<V1>
      && supports_compact_sequence_bitcast_view<V2>
      && std::is_same_v<typename V1::traits::alphabet_type,
                        typename V2::traits::alphabet_type>
      && std::is_same_v<typename V1::traits::storage_unit,
                        typename V2::traits::storage_unit>
[[nodiscard]] std::strong_ordering compare_sequences_scalar(
	const V1& a,
	const V2& b)
{
	using seq_traits = typename V1::traits;
	using storage_unit_t = typename seq_traits::storage_unit;
	constexpr size_t bits_per_char    = seq_traits::bits_per_char;
	constexpr size_t letters_per_unit = seq_traits::letters_per_unit;
	constexpr storage_unit_t char_mask = seq_traits::char_mask;
	constexpr storage_unit_t used_mask = seq_traits::used_storage_mask;

	const size_t bp_a = a.bp_size();
	const size_t bp_b = b.bp_size();
	const size_t bp_common = std::min(bp_a, bp_b);

	// Short-circuit on either-empty inputs so we never fetch read_cptr on
	// a zero-sized container — the existing custom-alphabet specialisation
	// dereferences cbegin() inside read_cptr, which is UB on an empty
	// std::vector. Empty <=> X collapses to the size-compare below.
	if (bp_common == 0) return bp_a <=> bp_b;

	const storage_unit_t* a_data = a.read_cptr(a.cbegin());
	const storage_unit_t* b_data = b.read_cptr(b.cbegin());

	size_t bp_idx = 0;
	while (bp_idx + letters_per_unit <= bp_common) {
		const size_t unit_idx = bp_idx / letters_per_unit;
		storage_unit_t au = static_cast<storage_unit_t>(a_data[unit_idx] & used_mask);
		storage_unit_t bu = static_cast<storage_unit_t>(b_data[unit_idx] & used_mask);
		if (au != bu) {
			for (size_t k = 0; k < letters_per_unit; ++k) {
				const storage_unit_t ac = au & char_mask;
				const storage_unit_t bc = bu & char_mask;
				if (ac != bc) return ac <=> bc;
				au = static_cast<storage_unit_t>(au >> bits_per_char);
				bu = static_cast<storage_unit_t>(bu >> bits_per_char);
			}
		}
		bp_idx += letters_per_unit;
	}
	if (bp_idx < bp_common) {
		const size_t unit_idx = bp_idx / letters_per_unit;
		storage_unit_t au = a_data[unit_idx];
		storage_unit_t bu = b_data[unit_idx];
		for (size_t k = 0; k < bp_common - bp_idx; ++k) {
			const storage_unit_t ac = au & char_mask;
			const storage_unit_t bc = bu & char_mask;
			if (ac != bc) return ac <=> bc;
			au = static_cast<storage_unit_t>(au >> bits_per_char);
			bu = static_cast<storage_unit_t>(bu >> bits_per_char);
		}
	}
	return bp_a <=> bp_b;
}

template <typename T, typename InfoT, typename I>
requires (
	requires (I const a) {
        { seqan3::to_rank(a) };
    }
)
struct hwy_vector_string_printer<T, InfoT, I> {
	static inline std::string format_block(const InfoT info, const void* ptr) {
		return decode_compressed_seq_el<T, I>(*((T*)ptr));
	}
};

// =====================================================================
// DOC0258: SIMD-using free functions over `compact_sequence_stdc<…>`.
// These live in `bioseq_sequences.simd` (the module that owns the SIMD
// encoder / decoder) so the templated bodies can call those primitives
// directly. The functions are exposed via the `bioseq::simd::active_target`
// namespace so the postgres binding (`pg_compact_sequence.cppm`) can
// call them without naming a different namespace.
//
// The class declaration in `bioseq_sequences:sequences` deliberately
// does NOT expose these as members because cross-module forward
// declarations of templates do not link (each module attaches its own
// mangled symbol).
// =====================================================================

template<
    class StdcT,
    sequence_alphabet alphabet_t,
    class Alloc
>
requires (
    is_compact_sequence_stdc<StdcT>
    && is_in_supported_alphabets_v<typename StdcT::supported_alphabets_type, alphabet_t>
    && stdc_allocator<Alloc, StdcT>
)
[[nodiscard]]
inline StdcT* stdc_convert_from_cstr(
    std::uint8_t target_storage_byte_width,
    typename StdcT::bp_size_t length,
    const char* input,
    Alloc&& alloc)
{
    DEBUG_ASSERT(
        StdcT::supported_storage_unit_byte_widths_type::contains_runtime(
            target_storage_byte_width),
        fmt::format(
            "stdc_convert_from_cstr: storage_unit_byte_width {} is not enabled",
                    static_cast<unsigned>(target_storage_byte_width)));

    StdcT* result = nullptr;
    StdcT::supported_storage_unit_byte_widths_type::dispatch(
        target_storage_byte_width,
        [&]<std::size_t BW>() {
            // DOC0268 (this run) — pin to std::uintN_t (NOT
            // boost::uint_t<>::least). On macOS arm64 the boost variant
            // resolves to `unsigned long` for BW==8, which is a different
            // C++ type from std::uint64_t (= `unsigned long long`) and
            // tripped Highway's `IsIntegerLaneType` static_assert when this
            // storage_unit fed into encode_sequence_simd.
            using storage_unit_t =
                std::conditional_t<BW == 8, std::uint64_t,
                std::conditional_t<BW == 4, std::uint32_t,
                std::conditional_t<BW == 2, std::uint16_t,
                                              std::uint8_t>>>;
            using sca_t = sequence_custom_alphabet<
                alphabet_t, storage_unit_t,
                seqan3::detail::ceil_log2(seqan3::alphabet_size<alphabet_t>)>;

            const auto packed =
                encode_sequence_simd<sca_t>(
                    input, static_cast<std::size_t>(length));

            const std::size_t unit_count_value  = packed.size();
            const std::size_t payload_bytes_value = unit_count_value * BW;
            const std::size_t total_bytes =
                StdcT::header_bytes() + round_up_to_uint64(payload_bytes_value);

            StdcT* hdr = std::forward<Alloc>(alloc)(total_bytes);
            if (hdr == nullptr) {
                result = nullptr;
                return;
            }
            hdr->vl_len_                 =
                StdcT::encode_varlena_header(total_bytes);
            hdr->alphabet_code           =
                StdcT::supported_alphabets_type::template index_of<alphabet_t>();
            hdr->storage_unit_byte_width = static_cast<std::uint8_t>(BW);
            hdr->padding_letters         =
                static_cast<std::uint16_t>(packed.padding());
            hdr->bp_size                 =
                static_cast<typename StdcT::bp_size_t>(packed.bp_size());

            if (unit_count_value > 0) {
                std::memmove(hdr->data, packed._data.data(), payload_bytes_value);
            }
            result = hdr;
        });
    return result;
}

template<
    class StdcT,
    sequence_alphabet target_alphabet_t,
    class Alloc
>
requires (
    is_compact_sequence_stdc<StdcT>
    && is_in_supported_alphabets_v<typename StdcT::supported_alphabets_type, target_alphabet_t>
    && stdc_allocator<Alloc, StdcT>
)
[[nodiscard]]
inline StdcT* stdc_recode_to(
    const StdcT& src,
    std::uint8_t target_byte_width,
    Alloc&& alloc)
{
    std::string ascii;
    src.convert_to([&]<class S>(const compact_sequence_range<S>& view) {
        ascii = decode_sequence_simd<S>(view);
    });
    return stdc_convert_from_cstr<StdcT, target_alphabet_t>(
        target_byte_width,
        static_cast<typename StdcT::bp_size_t>(ascii.size()),
        ascii.data(),
        std::forward<Alloc>(alloc));
}

template<class StdcT, class Alloc>
requires (
    is_compact_sequence_stdc<StdcT>
    && stdc_allocator<Alloc, StdcT>
)
[[nodiscard]]
inline StdcT* stdc_rewidth_to_type_params(
    const StdcT& src,
    compact_sequence_stdc_type_params target,
    Alloc&& alloc)
{
    // DOC0377: width coalescing is never an alphabet conversion. Callers
    // reject two differently bound alphabets with SQLSTATE 42804 before this
    // helper; retain the guard here so no future caller can silently enter the
    // generic target-alphabet recoder.
    if (target.alphabet_code != src.alphabet_code) {
        return nullptr;
    }

    // Same alphabet, width change. recast_storage_width copies the packed
    // payload with a raw memcpy, which preserves the sequence ONLY when the
    // alphabet's letters pack identically across storage widths — i.e. each
    // byte holds a whole number of letters (8 % letter_bit_width == 0). dna4
    // (2 bits/letter) qualifies; dna5 (3 bits/letter) does NOT — its letters
    // straddle byte boundaries differently at each width. For non-byte-aligned
    // alphabets use an alphabet-preserving ASCII round-trip below.
    bool byte_aligned = false;
    StdcT::supported_alphabets_type::dispatch_alphabet_by_code(
        src.alphabet_code,
        [&byte_aligned]<class A>() {
            constexpr std::size_t lbw =
                seqan3::detail::ceil_log2(
                    static_cast<std::size_t>(seqan3::alphabet_size<A>));
            byte_aligned = (lbw != 0u) && (8u % lbw == 0u);
        });
    if (byte_aligned) {
        return src.recast_storage_width(target.storage_unit_byte_width,
                                        std::forward<Alloc>(alloc));
    }
    StdcT* result = nullptr;
    StdcT::supported_alphabets_type::dispatch_alphabet_by_code(
        src.alphabet_code,
        [&result, &src, target, &alloc]<class SourceA>() {
            result = stdc_recode_to<StdcT, SourceA>(
                src, target.storage_unit_byte_width,
                std::forward<Alloc>(alloc));
        });
    return result;
}

/// Lexicographic compare across two compact_sequence_stdc values. Same
/// alphabet required at runtime; same storage width is not required
/// (cross-width compares cast via bit_cast_to_storage_unit).
template<class StdcT>
requires (is_compact_sequence_stdc<StdcT>)
[[nodiscard]]
inline std::strong_ordering stdc_compare(const StdcT& a, const StdcT& b)
{
    // DOC0260 R8 — load-bearing caller-contract check. Cross-alphabet
    // calls would silently return `std::strong_ordering::equal` from the
    // cross-alphabet `if constexpr` branch below (in release builds the
    // surrounding DEBUG_ASSERT compiles out), so raise loudly here. The
    // NO_POSTGRES branch covers the standalone aligner build where the
    // Postgres `ERROR` level constant is undefined; that path keeps the
    // historical DEBUG_ASSERT behaviour, which is acceptable because the
    // aligner exec never calls stdc_compare with cross-alphabet inputs
    // (every test fixture pins a single alphabet).
#ifndef NO_POSTGRES
    if (a.alphabet_code != b.alphabet_code) {
        bioseq_log<ERR, PG>(
            "stdc_compare: callers must pass same-alphabet sequences "
            "(lhs={}, rhs={})",
            static_cast<unsigned>(a.alphabet_code),
            static_cast<unsigned>(b.alphabet_code));
    }
#else
    DEBUG_ASSERT(a.alphabet_code == b.alphabet_code,
                 fmt::format("stdc_compare across alphabets: {} vs {}",
                             static_cast<unsigned>(a.alphabet_code),
                             static_cast<unsigned>(b.alphabet_code)));
#endif
    std::strong_ordering result = std::strong_ordering::equal;
    a.convert_to([&result, &b]<class Sa>(const compact_sequence_range<Sa>& ra) {
        b.convert_to([&result, &ra]<class Sb>(const compact_sequence_range<Sb>& rb) {
            using TraitsA = typename compact_sequence_range<Sa>::traits;
            using TraitsB = typename compact_sequence_range<Sb>::traits;
            if constexpr (
                !std::is_same_v<typename TraitsA::alphabet_type,
                                typename TraitsB::alphabet_type>
            ) {
                (void)ra;
                (void)rb;
            } else {
                const auto lazy_a = bioseq::storage::make_lazy_in_memory(ra);
                const auto lazy_b = bioseq::storage::make_lazy_in_memory(rb);
                const int comparison = bioseq::storage::compare_sequence_views(
                    bioseq::storage::as_lazy_sequence_view(lazy_a),
                    bioseq::storage::as_lazy_sequence_view(lazy_b));
                result = comparison < 0 ? std::strong_ordering::less
                    : comparison > 0 ? std::strong_ordering::greater
                                     : std::strong_ordering::equal;
            }
        });
    });
    return result;
}

}
HWY_AFTER_NAMESPACE();

#undef BIOSEQ_SIMD_X86_PRE_AVX3_FIXED128

namespace bioseq::simd {
// Target-neutral import surface for code outside HWY_NAMESPACE. This aliases
// the Highway namespace selected for this module build instead of hardcoding
// a concrete architecture.
namespace active_target = ::bioseq::simd::HWY_NAMESPACE;

template<class StdcT, class Alloc>
requires (
    is_compact_sequence_stdc<StdcT>
    && stdc_allocator<Alloc, StdcT>
)
[[nodiscard]] inline StdcT* stdc_rewidth_to_type_params(
    const StdcT& source,
    compact_sequence_stdc_type_params target,
    Alloc&& allocate)
{
    return active_target::stdc_rewidth_to_type_params(
        source, target, std::forward<Alloc>(allocate));
}
}

}

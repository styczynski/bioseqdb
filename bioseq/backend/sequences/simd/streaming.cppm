/**
 * @file streaming.cppm
 * @brief Streaming state, scalar codecs, lookup tables, and shape predicates.
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

export module bioseq.sequences.simd:streaming;

import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.rank_pack_core;
import bioseq.simd.vector_stream;

export {

// =====================================================================
// Highway-templated code.
// =====================================================================
HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;


/// Behaviour for ASCII bytes that don't map to a valid alphabet rank.
///
/// `drop`            — used by the file-input FSM (FASTA / FASTQ).
///                     Newlines, whitespace, and unknown bytes are
///                     silently skipped so the body can be passed
///                     unchunked.
/// `map_to_default`  — used by the SQL `seq_in` literal parser.
///                     Matches SeqAn's permissive alphabet assignment where
///                     unknown bytes resolve to the alphabet's
///                     default rank (A for dna4, N for dna5).
enum class unknown_byte_policy {
    drop,
    map_to_default,
};

/// Per-record encoder state. The encoder writes packed words directly
/// into `*output_storage` (a caller-owned `std::vector<p_storage_unit>`,
/// typically the IO batch's shared sequence buffer or a per-call vector
/// for the SQL one-shot adapter). Across records the partial-word
/// state (`partial_word`, `partial_letters`) is reset by `reset()` while
/// the output pointer is preserved.
template <typename p_storage_unit,
          std::size_t p_bits_per_character,
          std::size_t p_letters_per_unit>
struct encode_state {
    using base_container = std::vector<p_storage_unit>;

    /// Pointer to the destination vector. Set by the caller before
    /// every record (or batch). The encoder appends to `*output_storage`.
    base_container* output_storage = nullptr;
    /// Offset into `*output_storage` where the current record's first
    /// packed word starts. Captured at `reset()`. The FSM reads it back
    /// in `finalize_encode_state` to compute the record's slice.
    std::size_t record_start_offset = 0;
    /// Bits accumulated for the partial word that has not yet been
    /// pushed into `*output_storage`.
    p_storage_unit partial_word = 0;
    /// Number of letters in `partial_word`. Always `< p_letters_per_unit`
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

void ascii_to_rank_lut_simd_dna4_drop_impl(
    const std::uint8_t* HWY_RESTRICT src,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t size) noexcept;
void ascii_to_rank_lut_simd_dna4_map_to_default_impl(
    const std::uint8_t* HWY_RESTRICT src,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t size) noexcept;
void ascii_to_rank_lut_simd_dna5_drop_impl(
    const std::uint8_t* HWY_RESTRICT src,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t size) noexcept;
void ascii_to_rank_lut_simd_dna5_map_to_default_impl(
    const std::uint8_t* HWY_RESTRICT src,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t size) noexcept;

}  // namespace detail


/// Build the 256-entry ASCII → rank LUT for `p_alphabet`. Two policies
/// for unknown bytes:
///
/// * `drop`            — unknown bytes map to `0xFF` skip-sentinel. The
///                       scalar `append_ranks` and the SIMD multiline
///                       compaction path detect and drop them.
/// * `map_to_default`  — unknown bytes map to the alphabet's default
///                       rank (A for dna4, N for dna5).
///
/// Case folding (`A`↔`a`, `C`↔`c`, …) is applied via the bit-5 case
/// flip. RNA `U` / `u` is mapped to T's rank so RNA literals decode
/// correctly under both alphabets.
template <sequence_alphabet p_alphabet,
          unknown_byte_policy p_policy = unknown_byte_policy::drop>
[[nodiscard]] consteval std::array<std::uint8_t, 256>
build_letter_to_rank_lut() {
    std::array<std::uint8_t, 256> lut{};
    constexpr std::size_t k_alphabet_cardinality =
        static_cast<std::size_t>(seqan3::alphabet_size<p_alphabet>);

    if constexpr (p_policy == unknown_byte_policy::drop) {
        for (std::size_t byte_value = 0; byte_value < 256; ++byte_value) {
            lut[byte_value] = std::uint8_t{0xFF};
        }
    } else {
        std::uint8_t default_rank = 0;
        if constexpr (std::is_same_v<p_alphabet, seq::dna5>) {
            default_rank = 3;  // N
        }
        for (std::size_t byte_value = 0; byte_value < 256; ++byte_value) {
            lut[byte_value] = default_rank;
        }
    }

    for (std::size_t rank = 0; rank < k_alphabet_cardinality; ++rank) {
        const char canonical_char =
            seqan3::to_char(seqan3::assign_rank_to(rank, p_alphabet{}));
        const std::uint8_t upper_byte = static_cast<std::uint8_t>(canonical_char);
        lut[upper_byte] = static_cast<std::uint8_t>(rank);
        if (canonical_char >= 'A' && canonical_char <= 'Z') {
            lut[upper_byte | 0x20] = static_cast<std::uint8_t>(rank);
        }
        if (canonical_char >= 'a' && canonical_char <= 'z') {
            lut[upper_byte & 0x5F] = static_cast<std::uint8_t>(rank);
        }
    }
    if constexpr (std::is_same_v<p_alphabet, seq::dna4>
               || std::is_same_v<p_alphabet, seq::dna5>) {
        const auto thymine_rank = lut[static_cast<std::uint8_t>('T')];
        lut[static_cast<std::uint8_t>('U')] = thymine_rank;
        lut[static_cast<std::uint8_t>('u')] = thymine_rank;
    }
    return lut;
}

/// Build the two 16-byte halves of a `c & 0x1F`-indexed 32-byte rank
/// LUT for `p_alphabet` under `p_policy`. Returns `{low, high}` where
/// `low[i] = lut32[i]` and `high[i] = lut32[i + 16]`. Indexed by 5 low
/// bits of the input ASCII byte. Case folding is automatic because
/// bit 5 is the case bit.
///
/// The 32-byte LUT is the SIMD-friendly cousin of the 256-byte LUT —
/// it lets the runtime path execute as two 16-byte `TableLookupBytes`
/// + a select on bit 4 (~4-5 SIMD ops total), enabling fully-SIMD
/// `map_to_default` lookups on dna4 and dna5.
template <sequence_alphabet p_alphabet,
          unknown_byte_policy p_policy>
[[nodiscard]] consteval std::array<std::array<std::uint8_t, 16>, 2>
build_letter_to_rank_lut_32() {
    std::array<std::uint8_t, 32> lut{};

    constexpr std::uint8_t k_default_byte =
        (p_policy == unknown_byte_policy::drop)
            ? std::uint8_t{0xFF}
            : (std::is_same_v<p_alphabet, seq::dna5>
                ? std::uint8_t{3}    // N
                : std::uint8_t{0});  // A
    for (std::size_t lut_index = 0; lut_index < 32; ++lut_index) {
        lut[lut_index] = k_default_byte;
    }

    constexpr std::size_t k_alphabet_cardinality =
        static_cast<std::size_t>(seqan3::alphabet_size<p_alphabet>);
    for (std::size_t rank = 0; rank < k_alphabet_cardinality; ++rank) {
        const char canonical_char =
            seqan3::to_char(seqan3::assign_rank_to(rank, p_alphabet{}));
        const std::uint8_t low_five_bits =
            static_cast<std::uint8_t>(canonical_char) & 0x1F;
        lut[low_five_bits] = static_cast<std::uint8_t>(rank);
    }
    if constexpr (std::is_same_v<p_alphabet, seq::dna4>
               || std::is_same_v<p_alphabet, seq::dna5>) {
        lut[static_cast<std::uint8_t>('U') & 0x1F] =
            lut[static_cast<std::uint8_t>('T') & 0x1F];
    }

    std::array<std::array<std::uint8_t, 16>, 2> halves{};
    for (std::size_t lut_index = 0; lut_index < 16; ++lut_index) {
        halves[0][lut_index] = lut[lut_index];
    }
    for (std::size_t lut_index = 0; lut_index < 16; ++lut_index) {
        halves[1][lut_index] = lut[lut_index + 16];
    }
    return halves;
}

/// Build the 256-entry rank → ASCII LUT for `p_alphabet`. Indexed by
/// rank values. Entries past the alphabet's cardinality are 0 and
/// unreachable when the input is well-formed.
template <sequence_alphabet p_alphabet>
[[nodiscard]] consteval std::array<std::uint8_t, 256>
build_rank_to_letter_lut() {
    std::array<std::uint8_t, 256> lut{};
    constexpr std::size_t k_alphabet_cardinality =
        static_cast<std::size_t>(seqan3::alphabet_size<p_alphabet>);
    for (std::size_t rank = 0; rank < k_alphabet_cardinality; ++rank) {
        const char canonical_char =
            seqan3::to_char(seqan3::assign_rank_to(rank, p_alphabet{}));
        lut[rank] = static_cast<std::uint8_t>(canonical_char);
    }
    return lut;
}

/// Build the 16-entry low-nibble dna4 LUT used by the dna4 SIMD
/// `drop` fast path. `'A' & 0x0F = 1`, `'C' & 0x0F = 3`,
/// `'G' & 0x0F = 7`, `'T' & 0x0F = 4` — all distinct, lets us do
/// `TableLookupBytes(table_vec, chunk & 0x0F)` in one SIMD op.
[[nodiscard]] consteval std::array<std::uint8_t, 16>
build_dna4_low_nibble_lut() {
    std::array<std::uint8_t, 16> lut{};
    for (std::size_t lut_index = 0; lut_index < 16; ++lut_index) {
        lut[lut_index] = std::uint8_t{0xFF};
    }
    lut[0x01] = 0;  // 'A' & 0x0F == 'a' & 0x0F
    lut[0x03] = 1;  // 'C'
    lut[0x07] = 2;  // 'G'
    lut[0x04] = 3;  // 'T'
    return lut;
}

/// Build the 16-entry rank → ASCII LUT for the dna4 SIMD decoder.
/// Indexed by ranks in [0..3]. Remaining entries are zero.
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
// entry of `k_compact_low_table` (256 × 16 bytes) is a `TableLookupBytes`
// shuffle pattern that gathers the valid bytes from the low half of a
// 16-byte rank vector to the front. `k_compact_high_table` does the same
// for the high half. `k_shift_left_byte_table[shift]` shifts the high half
// left by `shift` bytes so it slots into the low half's tail.
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

inline constexpr auto k_compact_low_table = build_compact_low_table();
inline constexpr auto k_compact_high_table = build_compact_high_table();
inline constexpr auto k_shift_left_byte_table = build_shift_left_byte_table();

// ---------------------------------------------------------------------
// Streaming primitives — scalar, no Highway dependency. Used as the
// generic fallback for non-(dna4/dna5) alphabets and as the bookkeeping
// layer for every dna4/dna5 SIMD path.
// ---------------------------------------------------------------------

/// Append `count` ranks to the encoder state, packing them into
/// `partial_word` and pushing completed `p_storage_unit`s into
/// `*output_storage`. Skip-sentinel `0xFF` bytes are silently dropped.
template <typename p_storage_unit,
          std::size_t p_bits_per_character,
          std::size_t p_letters_per_unit>
inline void append_ranks(
    encode_state<p_storage_unit, p_bits_per_character, p_letters_per_unit>& state,
    const std::uint8_t* __restrict__ ranks,
    std::size_t count) noexcept {
    DEBUG_ASSERT(state.output_storage != nullptr,
                 "encode_state::output_storage must be configured before append_ranks");
    DEBUG_ASSERT(state.partial_letters < p_letters_per_unit,
                 "encode_state::partial_letters must be < p_letters_per_unit on entry");
    auto* output = state.output_storage;
    auto write_unit = [output](p_storage_unit value) {
        output->push_back(value);
        return true;
    };
    for (std::size_t rank_index = 0; rank_index < count; ++rank_index) {
        const std::uint8_t letter_rank = ranks[rank_index];
        if (letter_rank == 0xFF) continue;
        DEBUG_ASSERT(letter_rank < (1u << p_bits_per_character),
                     "letter rank exceeds p_bits_per_character range");
        const bool appended =
            bioseq::sequences::rank_pack_detail::append_valid_rank_lsb<
                p_storage_unit, p_bits_per_character, p_letters_per_unit>(
                    state.partial_word,
                    state.partial_letters,
                    state.total_letters,
                    letter_rank,
                    write_unit);
        DEBUG_ASSERT(appended,
                     "growing SIMD rank pack unexpectedly overflowed");
    }
}

template <typename p_storage_unit,
          std::size_t p_bits_per_character,
          std::size_t p_letters_per_unit>
inline void reset_encode_state(
    encode_state<p_storage_unit, p_bits_per_character, p_letters_per_unit>& state) noexcept {
    state.reset();
}

/// Finalise the encoder for the current record. Pushes the trailing
/// partial word (if any) into `*output_storage` and returns the
/// `(word_count, padding)` pair the FSM needs to record this record's
/// slice. Padding is the number of empty letter slots in the trailing
/// unit (or 0 if the record fills its last unit exactly).
template <typename p_storage_unit,
          std::size_t p_bits_per_character,
          std::size_t p_letters_per_unit>
[[nodiscard]] inline std::pair<std::size_t, std::size_t>
finalize_encode_state(
    encode_state<p_storage_unit, p_bits_per_character, p_letters_per_unit>& state) noexcept {
    DEBUG_ASSERT(state.output_storage != nullptr,
                 "encode_state::output_storage must be configured before finalize");
    auto* output = state.output_storage;
    auto write_unit = [output](p_storage_unit value) {
        output->push_back(value);
        return true;
    };
    const auto finished =
        bioseq::sequences::rank_pack_detail::finish_lsb<
            p_storage_unit, p_bits_per_character, p_letters_per_unit>(
                state.partial_word, state.partial_letters, write_unit);
    DEBUG_ASSERT(finished.capacity_ok,
                 "growing SIMD rank pack unexpectedly overflowed");
    const std::size_t word_count = output->size() - state.record_start_offset;
    state.total_letters = 0;
    return {word_count, finished.padding_letters};
}

/// Convert `size` ASCII bytes to ranks via the alphabet's 256-entry
/// LUT. This generic scalar implementation is correct for every alphabet.
template <sequence_alphabet p_alphabet,
          unknown_byte_policy p_policy = unknown_byte_policy::drop>
inline void ascii_to_rank_scalar(
    const std::uint8_t* __restrict__ src,
    std::uint8_t* __restrict__ dst,
    std::size_t size) noexcept {
    static constexpr auto k_alphabet_lut =
        build_letter_to_rank_lut<p_alphabet, p_policy>();
    for (std::size_t byte_offset = 0; byte_offset < size; ++byte_offset) {
        dst[byte_offset] = k_alphabet_lut[src[byte_offset]];
    }
}

/// Generic scalar `body_bytes → rank → append_ranks` pipeline. Used by
/// every alphabet shape that doesn't have a dedicated SIMD path.
template <sequence_alphabet p_alphabet,
          unknown_byte_policy p_policy,
          typename p_storage_unit,
          std::size_t p_bits_per_character,
          std::size_t p_letters_per_unit>
inline void encode_body_chunk_scalar(
    encode_state<p_storage_unit, p_bits_per_character, p_letters_per_unit>& state,
    const std::uint8_t* __restrict__ body_bytes,
    std::size_t body_size) {
    constexpr std::size_t k_scratch_capacity = 1024;
    std::array<std::uint8_t, k_scratch_capacity> rank_scratch{};
    std::size_t read_cursor = 0;
    while (read_cursor < body_size) {
        const std::size_t chunk_size =
            std::min(k_scratch_capacity, body_size - read_cursor);
        ascii_to_rank_scalar<p_alphabet, p_policy>(
            body_bytes + read_cursor,
            rank_scratch.data(),
            chunk_size);
        append_ranks<p_storage_unit, p_bits_per_character, p_letters_per_unit>(
            state, rank_scratch.data(), chunk_size);
        read_cursor += chunk_size;
    }
}

/// Generic scalar decoder. Walks `total_letters` ranks across the
/// packed storage units and writes ASCII bytes to `dst`. Works for any
/// alphabet and any storage shape.
template <sequence_alphabet p_alphabet,
          typename p_storage_unit,
          std::size_t p_bits_per_character,
          std::size_t p_letters_per_unit>
inline void decode_words_to_ascii_scalar(
    const p_storage_unit* __restrict__ words,
    std::uint8_t* __restrict__ dst,
    std::size_t total_letters) noexcept {
    static constexpr auto k_rank_to_letter_lut =
        build_rank_to_letter_lut<p_alphabet>();
    constexpr p_storage_unit k_character_mask =
        static_cast<p_storage_unit>(
            (p_storage_unit{1} << p_bits_per_character) - 1);

    std::size_t letters_emitted = 0;
    while (letters_emitted < total_letters) {
        const std::size_t word_index = letters_emitted / p_letters_per_unit;
        const p_storage_unit word = words[word_index];
        const std::size_t letters_left_in_word = std::min(
            p_letters_per_unit - (letters_emitted % p_letters_per_unit),
            total_letters - letters_emitted);
        const std::size_t letter_in_word_start =
            letters_emitted % p_letters_per_unit;
        for (std::size_t letter_offset = 0;
             letter_offset < letters_left_in_word; ++letter_offset) {
            const std::size_t letter_in_word =
                letter_in_word_start + letter_offset;
            const std::uint8_t rank = static_cast<std::uint8_t>(
                (word >> (p_bits_per_character * letter_in_word))
                & k_character_mask);
            dst[letters_emitted + letter_offset] = k_rank_to_letter_lut[rank];
        }
        letters_emitted += letters_left_in_word;
    }
}

/// Same as `decode_words_to_ascii_scalar` but injects `\n` every
/// `wrap_width` letters. Returns the new in-line letter counter so
/// callers can resume mid-line across multiple decode batches.
template <sequence_alphabet p_alphabet,
          typename p_storage_unit,
          std::size_t p_bits_per_character,
          std::size_t p_letters_per_unit>
[[nodiscard]] inline std::size_t decode_words_to_ascii_scalar_wrapped(
    const p_storage_unit* __restrict__ words,
    std::size_t total_letters,
    std::size_t wrap_width,
    std::size_t letters_in_current_line,
    std::uint8_t* __restrict__ dst) noexcept {
    static constexpr auto k_rank_to_letter_lut =
        build_rank_to_letter_lut<p_alphabet>();
    constexpr p_storage_unit k_character_mask =
        static_cast<p_storage_unit>(
            (p_storage_unit{1} << p_bits_per_character) - 1);

    std::size_t letters_emitted = 0;
    std::size_t out_cursor = 0;
    std::size_t line_position = letters_in_current_line;
    while (letters_emitted < total_letters) {
        const std::size_t word_index = letters_emitted / p_letters_per_unit;
        const std::size_t letter_in_word =
            letters_emitted % p_letters_per_unit;
        const p_storage_unit word = words[word_index];
        const std::uint8_t rank = static_cast<std::uint8_t>(
            (word >> (p_bits_per_character * letter_in_word))
            & k_character_mask);
        dst[out_cursor++] = k_rank_to_letter_lut[rank];
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
template <sequence_alphabet p_alphabet,
          typename p_storage_unit,
          std::size_t p_bits_per_character,
          std::size_t p_letters_per_unit>
constexpr bool is_dna4_simd_decode_shape() noexcept {
    return std::is_same_v<p_alphabet, seq::dna4>
        && std::is_same_v<p_storage_unit, std::uint64_t>
        && p_bits_per_character == 2
        && p_letters_per_unit == 32;
}

/// Constexpr predicate: does `(alphabet, storage, BPC, LPU)` match the
/// canonical dna5+uint8 SIMD-decode shape `(dna5, uint8_t, 3, 2)`?
template <sequence_alphabet p_alphabet,
          typename p_storage_unit,
          std::size_t p_bits_per_character,
          std::size_t p_letters_per_unit>
constexpr bool is_dna5_uint8_simd_decode_shape() noexcept {
    return std::is_same_v<p_alphabet, seq::dna5>
        && std::is_same_v<p_storage_unit, std::uint8_t>
        && p_bits_per_character == 3
        && p_letters_per_unit == 2;
}

namespace detail {
	template<typename p_element, typename p_generator, std::size_t... p_indices>
	constexpr auto generate_array(
		p_generator& generator,
		std::index_sequence<p_indices...>)
		-> std::array<p_element, sizeof...(p_indices)> {
		return {{generator(
			std::integral_constant<std::size_t, p_indices>{})...}};
	}
}

template<typename p_element, std::size_t p_size, typename p_generator>
constexpr auto generate_array(p_generator generator) {
    return detail::generate_array<p_element>(
        generator, std::make_index_sequence<p_size>{});
}

}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

}  // export

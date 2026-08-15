/**
 * @file
 * @brief Streaming and one-shot SIMD codecs for packed biological sequences.
 *
 * The interface selects Highway target implementations while retaining scalar
 * fallbacks for unsupported alphabet and storage-width combinations.
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

export module bioseq.sequences.simd;

import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.rank_pack_core;
import bioseq.storage.lazy_sequence;

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
    /// packed word starts. Captured at `reset()`; the FSM reads it back
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
/// * `drop`            — unknown bytes map to `0xFF` skip-sentinel; the
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
/// bits of the input ASCII byte; case folding is automatic because
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
/// rank values; entries past the alphabet's cardinality are 0 and
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
/// LUT. Generic scalar implementation; correct for every alphabet.
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

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
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
/// slot; `SumsOf4` reduces every 4 adjacent shifted bytes into one
/// packed byte (since the slots are disjoint, sum == OR); `Mul +
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
/// `p_policy == drop`          — uses the 16-entry low-nibble LUT;
///                               sentinel-detect + scalar fallback for
///                               chunks containing `0xFF`. 2×-unrolled.
/// `p_policy == map_to_default` — uses the 32-byte LUT split across two
///                               16-byte halves; no sentinels can
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

// ============================================================================
// One-shot adapters over the streaming encoder and decoder primitives.
// ============================================================================

/// One-shot scalar encoder. Adapter around `encode_body_chunk_scalar`.
/// The default maps unknown bytes to the alphabet's default rank, matching
/// the permissive SQL literal parser.
template <class p_destination_configuration,
          unknown_byte_policy p_policy = unknown_byte_policy::map_to_default>
requires is_sequence_custom_alphabet<p_destination_configuration>
[[nodiscard]] compact_sequence_range<p_destination_configuration>
encode_sequence_scalar(
    const char* input, std::size_t length) {
    using sequence_type =
        compact_sequence_range<p_destination_configuration>;
    using storage_unit_type = typename sequence_type::traits::storage_unit;
    using alphabet_type = typename sequence_type::traits::alphabet_type;
    constexpr std::size_t k_bits_per_character =
        sequence_type::traits::bits_per_char;
    constexpr std::size_t k_letters_per_unit =
        sequence_type::traits::letters_per_unit;

    std::vector<storage_unit_type> packed;
    packed.reserve(
        (length + k_letters_per_unit - 1) / k_letters_per_unit);

    encode_state<
        storage_unit_type,
        k_bits_per_character,
        k_letters_per_unit> state{};
    state.output_storage = &packed;
    state.reset();
    encode_body_chunk_scalar<
        alphabet_type,
        p_policy,
        storage_unit_type,
        k_bits_per_character,
        k_letters_per_unit>(
        state,
        reinterpret_cast<const std::uint8_t*>(input),
        length);
    const auto [word_count, padding] =
        finalize_encode_state<
            storage_unit_type,
            k_bits_per_character,
            k_letters_per_unit>(state);
    (void)word_count;
    return sequence_type::from_packed(std::move(packed), padding);
}

/// One-shot SIMD encoder. Adapter around `encode_body_chunk` —
/// dispatches to the dna4/dna5 SIMD pipeline when applicable.
template <class p_destination_configuration,
          unknown_byte_policy p_policy = unknown_byte_policy::map_to_default>
requires is_sequence_custom_alphabet<p_destination_configuration>
[[nodiscard]] compact_sequence_range<p_destination_configuration>
encode_sequence_simd(
    const char* input, std::size_t length) {
    using sequence_type =
        compact_sequence_range<p_destination_configuration>;
    using storage_unit_type = typename sequence_type::traits::storage_unit;
    using alphabet_type = typename sequence_type::traits::alphabet_type;
    constexpr std::size_t k_bits_per_character =
        sequence_type::traits::bits_per_char;
    constexpr std::size_t k_letters_per_unit =
        sequence_type::traits::letters_per_unit;

    std::vector<storage_unit_type> packed;
    packed.reserve(
        (length + k_letters_per_unit - 1) / k_letters_per_unit);

    encode_state<
        storage_unit_type,
        k_bits_per_character,
        k_letters_per_unit> state{};
    state.output_storage = &packed;
    state.reset();
    encode_body_chunk<
        alphabet_type,
        p_policy,
        storage_unit_type,
        k_bits_per_character,
        k_letters_per_unit>(
        state,
        reinterpret_cast<const std::uint8_t*>(input),
        length);
    const auto [word_count, padding] =
        finalize_encode_state<
            storage_unit_type,
            k_bits_per_character,
            k_letters_per_unit>(state);
    (void)word_count;
    return sequence_type::from_packed(std::move(packed), padding);
}

/// One-shot scalar decoder. Adapter around
/// `decode_words_to_ascii_scalar` for `is_sequence_custom_alphabet`
/// shapes (which expose packed storage through `read_cptr`).
/// For other `supports_compact_sequence_range` shapes (e.g.
/// `compact_sequence_range<std::vector<seq::dna4>>`), falls back to
/// the iterator-based path so the function works for every range.
template <class p_source_configuration>
requires supports_compact_sequence_range<p_source_configuration>
[[nodiscard]] std::string decode_sequence_scalar(
    const compact_sequence_range<p_source_configuration>& sequence) {
    using sequence_traits =
        typename compact_sequence_range<p_source_configuration>::traits;
    const std::size_t base_pair_count = sequence.bp_size();
    if (base_pair_count == 0) return std::string{};

    if constexpr (
        is_sequence_custom_alphabet<p_source_configuration>
        || is_compact_sequence_stdc_instance<p_source_configuration>) {
        using storage_unit_type = typename sequence_traits::storage_unit;
        using alphabet_type = typename sequence_traits::alphabet_type;
        constexpr std::size_t k_bits_per_character =
            sequence_traits::bits_per_char;
        constexpr std::size_t k_letters_per_unit =
            sequence_traits::letters_per_unit;
        std::string output(base_pair_count, '\0');
        decode_words_to_ascii_scalar<
            alphabet_type,
            storage_unit_type,
            k_bits_per_character,
            k_letters_per_unit>(
            sequence.read_cptr(sequence.cbegin()),
            reinterpret_cast<std::uint8_t*>(output.data()),
            base_pair_count);
        return output;
    } else {
        std::string output;
        output.reserve(base_pair_count);
        for (const auto& rank :
             sequence.template iterable<logical_rank>()) {
            output.push_back(sequence_traits::letter_to_char(
                static_cast<typename sequence_traits::storage_unit>(rank)));
        }
        return output;
    }
}

/// One-shot SIMD decoder. Adapter around `decode_words_to_ascii` —
/// SIMD path engages for `(dna4, uint64, 2, 32)` and
/// `(dna5, uint8, 3, 2)`; everything else uses the scalar bit-walk.
/// Falls back to the iterator-based scalar path for non-custom-alphabet
/// ranges.
template <class p_source_configuration>
requires supports_compact_sequence_range<p_source_configuration>
[[nodiscard]] std::string decode_sequence_simd(
    const compact_sequence_range<p_source_configuration>& sequence) {
    using sequence_traits =
        typename compact_sequence_range<p_source_configuration>::traits;
    const std::size_t base_pair_count = sequence.bp_size();
    if (base_pair_count == 0) return std::string{};

    if constexpr (
        is_sequence_custom_alphabet<p_source_configuration>
        || is_compact_sequence_stdc_instance<p_source_configuration>) {
        using storage_unit_type = typename sequence_traits::storage_unit;
        using alphabet_type = typename sequence_traits::alphabet_type;
        constexpr std::size_t k_bits_per_character =
            sequence_traits::bits_per_char;
        constexpr std::size_t k_letters_per_unit =
            sequence_traits::letters_per_unit;
        std::string output(base_pair_count, '\0');
        decode_words_to_ascii<
            alphabet_type,
            storage_unit_type,
            k_bits_per_character,
            k_letters_per_unit>(
            sequence.read_cptr(sequence.cbegin()),
            reinterpret_cast<std::uint8_t*>(output.data()),
            base_pair_count);
        return output;
    } else {
        std::string output;
        output.reserve(base_pair_count);
        for (const auto& rank :
             sequence.template iterable<logical_rank>()) {
            output.push_back(sequence_traits::letter_to_char(
                static_cast<typename sequence_traits::storage_unit>(rank)));
        }
        return output;
    }
}

// ============================================================================
// Lexicographic scalar comparator. Both sequences must share the same
// alphabet and storage-unit type.
//
// Constrained against `supports_compact_sequence_bitcast_view<p_view>` so the
// caller can pass either a raw `compact_sequence_range<p_range>` or a
// `bit_casted_view<base, output>`.
// ============================================================================
template<class p_left_view, class p_right_view>
requires supports_compact_sequence_bitcast_view<p_left_view>
      && supports_compact_sequence_bitcast_view<p_right_view>
      && std::is_same_v<typename p_left_view::traits::alphabet_type,
                        typename p_right_view::traits::alphabet_type>
      && std::is_same_v<typename p_left_view::traits::storage_unit,
                        typename p_right_view::traits::storage_unit>
[[nodiscard]] std::strong_ordering compare_sequences_scalar(
	const p_left_view& lhs,
	const p_right_view& rhs)
{
	using sequence_traits = typename p_left_view::traits;
	using storage_unit_type = typename sequence_traits::storage_unit;
	constexpr size_t k_bits_per_character = sequence_traits::bits_per_char;
	constexpr size_t k_letters_per_unit = sequence_traits::letters_per_unit;
	constexpr storage_unit_type k_character_mask = sequence_traits::char_mask;
	constexpr storage_unit_type k_used_storage_mask =
		sequence_traits::used_storage_mask;

	const size_t lhs_size = lhs.bp_size();
	const size_t rhs_size = rhs.bp_size();
	const size_t common_size = std::min(lhs_size, rhs_size);

	// Short-circuit on either-empty inputs and reduce the result to length.
	if (common_size == 0) return lhs_size <=> rhs_size;

	const storage_unit_type* lhs_data = lhs.read_cptr(lhs.cbegin());
	const storage_unit_type* rhs_data = rhs.read_cptr(rhs.cbegin());

	size_t symbol_index = 0;
	while (symbol_index + k_letters_per_unit <= common_size) {
		const size_t unit_index = symbol_index / k_letters_per_unit;
		storage_unit_type lhs_unit = static_cast<storage_unit_type>(
			lhs_data[unit_index] & k_used_storage_mask);
		storage_unit_type rhs_unit = static_cast<storage_unit_type>(
			rhs_data[unit_index] & k_used_storage_mask);
		if (lhs_unit != rhs_unit) {
			for (size_t letter_index = 0;
			     letter_index < k_letters_per_unit;
			     ++letter_index) {
				const storage_unit_type lhs_rank =
					lhs_unit & k_character_mask;
				const storage_unit_type rhs_rank =
					rhs_unit & k_character_mask;
				if (lhs_rank != rhs_rank) return lhs_rank <=> rhs_rank;
				lhs_unit = static_cast<storage_unit_type>(
					lhs_unit >> k_bits_per_character);
				rhs_unit = static_cast<storage_unit_type>(
					rhs_unit >> k_bits_per_character);
			}
		}
		symbol_index += k_letters_per_unit;
	}
	if (symbol_index < common_size) {
		const size_t unit_index = symbol_index / k_letters_per_unit;
		storage_unit_type lhs_unit = lhs_data[unit_index];
		storage_unit_type rhs_unit = rhs_data[unit_index];
		for (size_t letter_index = 0;
		     letter_index < common_size - symbol_index;
		     ++letter_index) {
			const storage_unit_type lhs_rank = lhs_unit & k_character_mask;
			const storage_unit_type rhs_rank = rhs_unit & k_character_mask;
			if (lhs_rank != rhs_rank) return lhs_rank <=> rhs_rank;
			lhs_unit = static_cast<storage_unit_type>(
				lhs_unit >> k_bits_per_character);
			rhs_unit = static_cast<storage_unit_type>(
				rhs_unit >> k_bits_per_character);
		}
	}
	return lhs_size <=> rhs_size;
}

// =====================================================================
// Standard-layout compact-sequence adapters live beside the SIMD encoder and
// decoder so their function templates can instantiate the target-specific
// primitives. Callers use `bioseq::simd::active_target` without naming an
// architecture namespace.
// =====================================================================

/**
 * @brief Encode text directly into an allocated standard-layout sequence.
 * @tparam p_sequence_header Destination header type.
 * @tparam p_alphabet Alphabet used to interpret input symbols.
 * @tparam p_allocator Byte allocator returning `p_sequence_header*`.
 * @param target_storage_byte_width Packed storage-unit width in bytes.
 * @param length Number of input characters.
 * @param input Input characters.
 * @param allocator Destination allocator.
 * @return Allocated sequence, or `nullptr` when allocation fails.
 */
template<
    class p_sequence_header,
    sequence_alphabet p_alphabet,
    class p_allocator
>
requires (
    is_compact_sequence_stdc<p_sequence_header>
    && alphabet_in_supported_set<
        typename p_sequence_header::supported_alphabets_type,
        p_alphabet>
    && stdc_allocator<p_allocator, p_sequence_header>
)
[[nodiscard]]
inline p_sequence_header* stdc_convert_from_cstr(
    std::uint8_t target_storage_byte_width,
    typename p_sequence_header::bp_size_t length,
    const char* input,
    p_allocator&& allocator)
{
    DEBUG_ASSERT(
        p_sequence_header::supported_storage_unit_byte_widths_type::
            contains_runtime(target_storage_byte_width),
        fmt::format(
            "stdc_convert_from_cstr: storage_unit_byte_width {} is not enabled",
                    static_cast<unsigned>(target_storage_byte_width)));

    p_sequence_header* result = nullptr;
    p_sequence_header::supported_storage_unit_byte_widths_type::dispatch(
        target_storage_byte_width,
        [&result, &length, &input, &allocator]<std::size_t p_storage_byte_width>() {
            // Fixed-width standard types give Highway an accepted lane type
            // on every supported LP64 target. Some least-width integer aliases
            // use a different C++ spelling despite an identical bit width.
            using storage_unit_type =
                std::conditional_t<p_storage_byte_width == 8, std::uint64_t,
                std::conditional_t<p_storage_byte_width == 4, std::uint32_t,
                std::conditional_t<p_storage_byte_width == 2, std::uint16_t,
                                                   std::uint8_t>>>;
            using sequence_configuration = sequence_custom_alphabet<
                p_alphabet,
                storage_unit_type,
                seqan3::detail::ceil_log2(
                    seqan3::alphabet_size<p_alphabet>)>;

            const auto packed =
                encode_sequence_simd<sequence_configuration>(
                    input, static_cast<std::size_t>(length));

            const std::size_t unit_count = packed.size();
            const std::size_t payload_bytes =
                unit_count * p_storage_byte_width;
            const std::size_t total_bytes =
                p_sequence_header::header_bytes()
                + round_up_to_uint64(payload_bytes);

            p_sequence_header* header =
                std::forward<p_allocator>(allocator)(total_bytes);
            if (header == nullptr) {
                result = nullptr;
                return;
            }
            header->vl_len_ =
                p_sequence_header::encode_varlena_header(total_bytes);
            header->alphabet_code =
                p_sequence_header::supported_alphabets_type::template
                    index_of<p_alphabet>();
            header->storage_unit_byte_width =
                static_cast<std::uint8_t>(p_storage_byte_width);
            header->padding_letters =
                static_cast<std::uint16_t>(packed.padding());
            header->bp_size =
                static_cast<typename p_sequence_header::bp_size_t>(
                    packed.bp_size());

            if (unit_count > 0) {
                std::memmove(
                    header->data,
                    packed.packed_units.data(),
                    payload_bytes);
            }
            result = header;
        });
    return result;
}

/**
 * @brief Recode a standard-layout sequence into another supported alphabet.
 * @tparam p_sequence_header Source and destination header type.
 * @tparam p_target_alphabet Destination alphabet.
 * @tparam p_allocator Byte allocator returning `p_sequence_header*`.
 * @param source Sequence to decode and recode.
 * @param target_byte_width Destination packed-unit width in bytes.
 * @param allocator Destination allocator.
 * @return Newly allocated recoded sequence, or `nullptr` on allocation failure.
 */
template<
    class p_sequence_header,
    sequence_alphabet p_target_alphabet,
    class p_allocator
>
requires (
    is_compact_sequence_stdc<p_sequence_header>
    && alphabet_in_supported_set<
        typename p_sequence_header::supported_alphabets_type,
        p_target_alphabet>
    && stdc_allocator<p_allocator, p_sequence_header>
)
[[nodiscard]]
inline p_sequence_header* stdc_recode_to(
    const p_sequence_header& source,
    std::uint8_t target_byte_width,
    p_allocator&& allocator)
{
    std::string ascii;
    source.convert_to([&ascii]<class p_instance>(const compact_sequence_range<p_instance>& view) {
        ascii = decode_sequence_simd<p_instance>(view);
    });
    return stdc_convert_from_cstr<p_sequence_header, p_target_alphabet>(
        target_byte_width,
        static_cast<typename p_sequence_header::bp_size_t>(ascii.size()),
        ascii.data(),
        std::forward<p_allocator>(allocator));
}

/**
 * @brief Change packed storage width while retaining the logical alphabet.
 * @tparam p_sequence_header Source and destination header type.
 * @tparam p_allocator Byte allocator returning `p_sequence_header*`.
 * @param source Sequence to recast.
 * @param target Destination alphabet code and storage width.
 * @param allocator Destination allocator.
 * @return Newly allocated sequence, or `nullptr` for an alphabet mismatch or
 * allocation failure.
 */
template<class p_sequence_header, class p_allocator>
requires (
    is_compact_sequence_stdc<p_sequence_header>
    && stdc_allocator<p_allocator, p_sequence_header>
)
[[nodiscard]]
inline p_sequence_header* stdc_rewidth_to_type_params(
    const p_sequence_header& source,
    compact_sequence_stdc_type_params target,
    p_allocator&& allocator)
{
    // Width coalescing never changes alphabets. Keep this local guard even
    // though SQL callers validate bound alphabets before entering the helper.
    if (target.alphabet_code != source.alphabet_code) {
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
    p_sequence_header::supported_alphabets_type::dispatch_alphabet_by_code(
        source.alphabet_code,
        [&byte_aligned]<class p_alphabet>() {
            constexpr std::size_t k_letter_bit_width =
                seqan3::detail::ceil_log2(
                    static_cast<std::size_t>(
                        seqan3::alphabet_size<p_alphabet>));
            byte_aligned = (k_letter_bit_width != 0u)
                && (8u % k_letter_bit_width == 0u);
        });
    if (byte_aligned) {
        return source.recast_storage_width(
            target.storage_unit_byte_width,
            std::forward<p_allocator>(allocator));
    }
    p_sequence_header* result = nullptr;
    p_sequence_header::supported_alphabets_type::dispatch_alphabet_by_code(
        source.alphabet_code,
        [&result, &source, target, &allocator]<class p_source_alphabet>() {
            result = stdc_recode_to<
                p_sequence_header, p_source_alphabet>(
                source,
                target.storage_unit_byte_width,
                std::forward<p_allocator>(allocator));
        });
    return result;
}

/**
 * @brief Compare two standard-layout compact sequences lexicographically.
 *
 * Both values must use the same alphabet. Their packed storage widths may
 * differ because lazy views normalize the comparison.
 * @tparam p_sequence_header Shared standard-layout header type.
 * @param lhs Left sequence.
 * @param rhs Right sequence.
 * @return Strong ordering of logical symbol sequences.
 */
template<class p_sequence_header>
requires (is_compact_sequence_stdc<p_sequence_header>)
[[nodiscard]]
inline std::strong_ordering stdc_compare(
    const p_sequence_header& lhs,
    const p_sequence_header& rhs)
{
    // Cross-alphabet comparison is outside this function's contract. The
    // PostgreSQL build reports the violation at runtime; standalone builds
    // retain a debug assertion because they do not provide PostgreSQL logging
    // levels.
#ifndef NO_POSTGRES
    if (lhs.alphabet_code != rhs.alphabet_code) {
        bioseq_log<logging_level::error, logging_target::postgres>(
            "stdc_compare: callers must pass same-alphabet sequences "
            "(lhs={}, rhs={})",
            static_cast<unsigned>(lhs.alphabet_code),
            static_cast<unsigned>(rhs.alphabet_code));
    }
#else
    DEBUG_ASSERT(lhs.alphabet_code == rhs.alphabet_code,
                 fmt::format("stdc_compare across alphabets: {} vs {}",
                             static_cast<unsigned>(lhs.alphabet_code),
                             static_cast<unsigned>(rhs.alphabet_code)));
#endif
    std::strong_ordering result = std::strong_ordering::equal;
    lhs.convert_to([&result, &rhs]<class p_left_instance>(
                       const compact_sequence_range<p_left_instance>& left) {
        rhs.convert_to([&result, &left]<class p_right_instance>(
                           const compact_sequence_range<p_right_instance>& right) {
            using left_traits =
                typename compact_sequence_range<p_left_instance>::traits;
            using right_traits =
                typename compact_sequence_range<p_right_instance>::traits;
            if constexpr (
                !std::is_same_v<typename left_traits::alphabet_type,
                                typename right_traits::alphabet_type>
            ) {
                (void)left;
                (void)right;
            } else {
                const auto left_view =
                    bioseq::storage::make_lazy_sequence_view(left);
                const auto right_view =
                    bioseq::storage::make_lazy_sequence_view(right);
                const int comparison = bioseq::storage::compare_lazy_sequences(
                    left_view, right_view);
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

template<class p_sequence_header, class p_allocator>
requires (
    is_compact_sequence_stdc<p_sequence_header>
    && stdc_allocator<p_allocator, p_sequence_header>
)
[[nodiscard]] inline p_sequence_header* stdc_rewidth_to_type_params(
    const p_sequence_header& source,
    compact_sequence_stdc_type_params target,
    p_allocator&& allocator)
{
    return active_target::stdc_rewidth_to_type_params(
        source, target, std::forward<p_allocator>(allocator));
}
}

}

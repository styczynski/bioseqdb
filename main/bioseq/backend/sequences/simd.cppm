module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include "common.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <functional>

#include <boost/integer.hpp>

#include "common/simd/utils_impl.hpp"
#if defined(__GNUC__) && !defined(__clang__)
#include "common/simd/vector_stream_impl.hpp"
#endif

export module bioseq.sequences.simd;

import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.common.circular_append_buffer;

#if !defined(__GNUC__) || defined(__clang__)
export import bioseq.simd.vector_stream;
#endif

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
    storage_unit_t local_partial_word = state.partial_word;
    std::size_t local_partial_letters = state.partial_letters;
    std::size_t local_total_letters = state.total_letters;
    for (std::size_t rank_index = 0; rank_index < count; ++rank_index) {
        const std::uint8_t letter_rank = ranks[rank_index];
        if (letter_rank == 0xFF) continue;
        DEBUG_ASSERT(letter_rank < (1u << BitsPerChar),
                     "letter rank exceeds BitsPerChar range");
        local_partial_word |= static_cast<storage_unit_t>(letter_rank)
                              << (BitsPerChar * local_partial_letters);
        local_partial_letters += 1;
        local_total_letters += 1;
        if (local_partial_letters == LettersPerUnit) {
            output->push_back(local_partial_word);
            local_partial_word = 0;
            local_partial_letters = 0;
        }
    }
    state.partial_word = local_partial_word;
    state.partial_letters = local_partial_letters;
    state.total_letters = local_total_letters;
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
    std::size_t padding = 0;
    if (state.partial_letters > 0) {
        output->push_back(state.partial_word);
        padding = LettersPerUnit - state.partial_letters;
    }
    const std::size_t n_words = output->size() - state.record_start_offset;
    state.partial_word = 0;
    state.partial_letters = 0;
    state.total_letters = 0;
    return {n_words, padding};
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

template <typename T>
inline constexpr std::pair<T, T> int_div_ceil(const T& val, const T& div) {
	const T a = (val + div - 1) / div;
	return std::make_pair(a, a * div - val);
}

// deprecated
enum simd_bp_iteration_type {
    FULL_MATRIX = 0,
	PARTIAL_VALID_LEFT = 1,
	PARTIAL_INVALID_LEFT = 2,
	PARTIAL_VALID_LEFT_PADDING = 3
};

enum simd_bp_iteration_part {
    ITER_FULL_MATRIX = 0,
	ITER_PARTIAL_MATRIX = 1
};

/// NEW!!!!

template<size_t max_lanes>
std::tuple<size_t, std::array<size_t, max_lanes>> calculate_iteration_offsets(
	const size_t seq_len,
	const size_t lanes_n,
	const size_t context_units
) {
	const auto& [units_per_lane, _] = int_div_ceil(seq_len - context_units, lanes_n);
	const size_t max_offset = seq_len - context_units - units_per_lane;
	size_t i = lanes_n - 1;
	std::array<size_t, max_lanes> offsets = {};
	offsets[i] = max_offset;
	while (i>=1) {
		--i;
		if (offsets[i+1] < units_per_lane) {
			// bioseq_log<DBG>("offset problematic at {} ({} cmp {})", i, offsets[i+1], units_per_lane);
			break;
		}
		offsets[i] = offsets[i+1] - units_per_lane;
		// bioseq_log<DBG>("{} -> {} - {} = {}", i, offsets[i+1], units_per_lane, offsets[i]);
	}
	for(; i > 0; --i) {
		offsets[i] = 0;
	}
	return make_tuple(units_per_lane, offsets);
}

template<
	typename... delays_ts,
	supports_compact_sequence_range R,
	typename handler_fn_t,
	typename finish_fn_t,
	typename I = decltype(compact_sequence_range<R>().begin()),
	size_t max_delay_bound = 256
>
inline void iterate_sequence_bp_simd_offsets(
	const compact_sequence_range<R>& seq,
	const I& begin,
	const I& end,
	const size_t context_bps,
	handler_fn_t&& handler_fn,
	finish_fn_t&& finish_fn,
	//const size_t context,
	delays_ts... delays
) {
    using seq_traits = compact_sequence_range<R>::traits;

	// Function-entry log gated behind BIOSEQ_HOT_TRACE_LOGS — formatting
	// the offsets vector + sequence pretty-print costs ~5-10 us per call,
	// dominating the steady-state mm_sketch_simd time when this fired
	// raw on every call.
	const auto& [context_units, _] = int_div_ceil(context_bps, seq_traits::letters_per_unit);
	constexpr size_t delays_count = sizeof...(delays);

	const size_t seq_len = seq.size();
	if (seq.bp_size() < context_bps) {
		// Sequence shorter than required context
		return;
	}

	const size_t max_delay = std::max<size_t>({ static_cast<size_t>(0), delays... });
	DEBUG_ASSERT(max_delay <= max_delay_bound, "Delay used cannot exceed manimum delay bound defined by max_delay_bound");

	const hn::ScalableTag<typename seq_traits::storage_unit> d;
	using VecT = hn::Vec<decltype(d)>;
	using vec_mask_t = hn::Mask<decltype(d)>;
	// template <class D, class VecT = hn::Vec<D>, typename p_matrix_t = VecT*>
	using matrix = square_lane_matrix<decltype(d)>;

	const VecT bp_bitmask_vec = hn::Set(d, seq_traits::char_mask);

    auto iter = begin;

	const size_t matrix_size = hn::Lanes(d) * hn::Lanes(d);
	constexpr size_t matrix_size_bound = hn::MaxLanes(d) * hn::MaxLanes(d);

	const size_t buffer_size = (int_div_ceil(max_delay / seq_traits::letters_per_unit, matrix_size).first + 1) * matrix_size;
	constexpr size_t buffer_size_bound = (int_div_ceil(max_delay_bound / seq_traits::letters_per_unit, matrix_size_bound).first + 1) * matrix_size_bound;

	const auto& [units_per_lane, offsets] = calculate_iteration_offsets<hn::MaxLanes(d)>(
		seq_len,
		hn::Lanes(d),
		context_units
	);

	const size_t bps_per_lane = (context_units + units_per_lane) * seq_traits::letters_per_unit;
	auto [full_chunks_count_raw, bps_last_chunk_raw] = int_div_floor(bps_per_lane, hn::Lanes(d) * seq_traits::letters_per_unit);
	// When the sequence length is an exact multiple of (Lanes * letters_per_unit) per
	// lane we end up with bps_last_chunk == 0. That means:
	//   (a) the partial-matrix loop never runs, so `finish_fn` is never invoked, and
	//   (b) any padding that lives in the tail of the last "full" chunk is processed
	//       without the partial-matrix lane mask, so garbage bytes can poison the
	//       minimizer search. Both cases showed up in dna5 × uint32_t and dna4 ×
	//       uint64_t parameterizations of the kmer tests. Fix: whenever this happens
	//       move the final full chunk into the partial-matrix path (reduce full
	//       chunks by one, set bps_last_chunk to a full matrix worth). This keeps
	//       the full-chunk fast path for everything in the middle but gives the tail
	//       proper masking and a finish_fn call.
	size_t full_chunks_count = full_chunks_count_raw;
	size_t bps_last_chunk = bps_last_chunk_raw;
	if (bps_last_chunk == 0 && full_chunks_count > 0) {
		--full_chunks_count;
		bps_last_chunk = hn::Lanes(d) * seq_traits::letters_per_unit;
	}

	using buffer_t = circular_append_buffer<VecT, buffer_size_bound, 0, false, hwy::AlignedFreeUniquePtr<VecT[]>, simd_array_allocator>;
	buffer_t buffer(buffer_size);

	HWY_ALIGN typename std::conditional<delays_count == 0, std::monostate, std::array<std::tuple<typename buffer_t::iterator, size_t, const size_t, VecT>, delays_count>>::type delays_state{
		(std::make_tuple(buffer.get_begin(), delays, seq_traits::letters_per_unit - 1, hn::Zero(d)))...
	};

	auto buffer_write_pos = buffer.get_begin();

	const vec_mask_t full_mask = hn::FirstN(d, hn::Lanes(d));
	const vec_mask_t none_mask = hn::MaskFalse(d);
	//const vec_mask_t dummy_mask = hn::FirstN(d, hn::Lanes(d));
	HWY_ALIGN std::array<vec_mask_t, delays_count> delays_masks;
	if constexpr (delays_count > 0) {
		delays_masks.fill(none_mask);
	}

	const vec_mask_t left_mask = offsets[0] == offsets[hn::Lanes(d)-1] ? hn::MaskFalse(d) : hn::FirstN(d, hn::Lanes(d)-1);
	typename std::conditional<delays_count == 0, std::monostate, std::array<int32_t, delays_count>>::type delays_counters = { -static_cast<int32_t>(delays)... };
	vec_mask_t main_mask = full_mask;
	int32_t main_counter = 0;

	DEBUG_ASSERT(bps_last_chunk >= seq.padding() || bps_last_chunk == 0, "Sanity check. This should never fail as we should have more basses than are padded, otherwise all calculations are wrong.");
	const int32_t main_counter_only_left = static_cast<int32_t>(bps_last_chunk) - static_cast<int32_t>(seq.padding());
	const int32_t main_counter_none = static_cast<int32_t>(bps_last_chunk);

	size_t live_iterators_count = 1 + delays_count;

	HWY_ALIGN std::array<VecT, delays_count> all_bp_vecs;
	VecT base_bp_vec;

	auto handle_row_bp = [&]<simd_bp_iteration_part mode>(VecT row) -> bool {
		for (size_t bp_index = 0; bp_index < seq_traits::letters_per_unit; ++bp_index) {
			VecT base_bp_vec = hn::And(row, bp_bitmask_vec);
			row = hn::ShiftRightSame(row, seq_traits::bits_per_char);
			// Check delayed
			if constexpr (delays_count > 0) {
				for(auto delay_no = 0; delay_no < delays_count; ++delay_no) {
					auto& [ read_pos, counter, delay, delayed_buffer ] = delays_state[delay_no];
					if (counter == 0) [[unlikely]] {
						delayed_buffer = buffer.get_value_iter(read_pos);
						counter = delay;
						buffer.increment_iter(read_pos);
						if (mode == ITER_FULL_MATRIX || delays_counters[delay_no] < main_counter_only_left) {
							delays_masks[delay_no] = full_mask;
						}
					} else {
						--counter;
					}
					// The same
					all_bp_vecs[delay_no] = hn::And(delayed_buffer, bp_bitmask_vec);
					delayed_buffer = hn::ShiftRightSame(delayed_buffer, seq_traits::bits_per_char);
					//bioseq_log<DBG>("next (-{})bp vec = {}", delay, hwy_str<decltype(d), seq::dna4>(d, delayed_bp));
				}
			}
			if constexpr (mode == ITER_FULL_MATRIX) {
				std::apply(handler_fn, std::tuple_cat(std::forward_as_tuple(std::bool_constant<true>{}, offsets, d, full_mask, base_bp_vec), delays_masks, all_bp_vecs));
			} else if constexpr (mode == ITER_PARTIAL_MATRIX) {
				std::apply(handler_fn, std::tuple_cat(std::forward_as_tuple(std::bool_constant<false>{}, offsets, d, main_mask, base_bp_vec), delays_masks, all_bp_vecs));
			}
			// bioseq_log<DBG>("next (-{})bp vec = {}", 0, hwy_str<decltype(d), seq::dna4>(d, base_bp_vec));

			if constexpr (mode == ITER_PARTIAL_MATRIX) {
				if constexpr (delays_count > 0) {
					for(auto delay_no = 0; delay_no < delays_count; ++delay_no) {
						auto& [ read_pos, counter, delay, delayed_buffer ] = delays_state[delay_no];
						++delays_counters[delay_no];
						if (delays_counters[delay_no] == main_counter_none) {
							delays_masks[delay_no] = none_mask;
							--live_iterators_count;
							if (live_iterators_count == 0) {
								return true;
							}
						} else if (delays_counters[delay_no] == main_counter_only_left) {
							delays_masks[delay_no] = left_mask;
						}
					}
				}

				++main_counter;
				if (main_counter == main_counter_none) {
					main_mask = none_mask;
					--live_iterators_count;
					if (live_iterators_count == 0) {
						return true;
					}
				} else if (main_counter == main_counter_only_left) {
					main_mask = left_mask;
				}

				// bioseq_log<DBG>("main_counter={} of (left): {} / (none): {}", main_counter, main_counter_only_left, main_counter_none);
			}
		}
		return false;
	};

	for(size_t chunk_no=0; chunk_no<full_chunks_count; ++chunk_no) {
		matrix mat(buffer.get_ptr_iter(buffer_write_pos));

		// We read only full matrices
		DEBUG_ASSERT(buffer.get_forward_capacity() >= matrix_size, "Demand correct capacity in the buffer pre-load");
		// iter += mat.fill_with_offsets(d, offsets,static_cast<const typename seq_traits::storage_unit*>(&(*iter)));
		iter += mat.fill_with_offsets(d, offsets, seq.read_cptr(iter));
		mat.transpose(d);
		DEBUG_ASSERT(buffer.get_forward_capacity() >= matrix_size, "Demand correct capacity in the buffer post-load");

		for (size_t row_index = 0; row_index < hn::Lanes(d); ++row_index) {
			//row = buffer.get_value_iter(buffer_write_pos);
			if(handle_row_bp.template operator()<ITER_FULL_MATRIX>(buffer.get_value_iter(buffer_write_pos))) {
				return;
			}
			buffer.increment_iter(buffer_write_pos);
		}
		// context change
	}

	DEBUG_ASSERT(
		static_cast<size_t>(end - iter) * seq_traits::letters_per_unit - offsets[hn::Lanes(d)-1] * seq_traits::letters_per_unit \
		== bps_last_chunk,
		"Expect the last available part to load be exactly of size bps_last_chunk (ignoring padding)"
	);

	DEBUG_ASSERT(buffer.get_forward_capacity() >= bps_last_chunk / seq_traits::letters_per_unit, "Demand correct capacity in the buffer when loading last (partial) chunk");

	//bioseq_log<DBG>("LOAD {} VALUES PARTIAL cap={}", static_cast<uint32_t>(end - iter), buffer.get_forward_capacity());

	matrix mat(buffer.get_ptr_iter(buffer_write_pos));
	mat.partial_fill_with_offsets(
		d, end - iter, offsets, seq.read_cptr(iter) //static_cast<const typename seq_traits::storage_unit*>(&(*iter))
	);
	mat.transpose(d);

	if (main_counter_none == 0) {
		return;
	}

	DEBUG_ASSERT(main_counter_none >= 1, "None counter cannot be less than 1.");
	DEBUG_ASSERT(main_counter_only_left >= 1, "Only-left counter cannot be less than 1.");
	DEBUG_ASSERT(main_counter_none >= main_counter_only_left, "Counters should have proper order (first should be left, then none)");
	// `main_counter_none` is how many partial-matrix iterations we will run; that
	// upper bound is the per-lane reach (bps_per_lane), NOT the total sequence
	// length. After the `bps_last_chunk == 0` fix we may pull a full chunk into
	// partial mode, and then bps_per_lane can be larger than seq.bp_size() (e.g.
	// dna4 × uint64_t: 64 iterations over a 62-bp sequence — the last 2 are
	// masked out via left_mask/none_mask transitions). Use bps_per_lane as the
	// sanity bound instead of seq.bp_size().
	DEBUG_ASSERT(static_cast<size_t>(main_counter_none) <= bps_per_lane, "Sanity check. If this fails, there's probably size_t overflow");

	while(true) {
		// todo: break condition
		//row = buffer.get_value_iter(buffer_write_pos);
		if(handle_row_bp.template operator()<ITER_PARTIAL_MATRIX>(buffer.get_value_iter(buffer_write_pos))) {
			finish_fn(offsets, d, left_mask);
			return;
		}
		buffer.increment_iter(buffer_write_pos);
	}
	finish_fn(offsets, d, left_mask);
	return;
}

/**
 * Multi-stream sibling of `iterate_sequence_bp_simd_offsets`.
 *
 * Same algorithmic shape (calculate_iteration_offsets → square_lane_matrix
 * fill+transpose → per-row per-bp handler call → partial-chunk tail with
 * full_mask → left_mask → none_mask transitions → finish_fn), generalised
 * along one axis: `NumStreams` SIMD streams are interleaved per call to the
 * handler. The original (single-stream) caller pattern is recovered exactly
 * with `NumStreams == 1`.
 *
 * Why this exists: the two-stack minimizer collector wants 2× ILP on M1's
 * NEON pipes by running two independent SIMD streams over disjoint slices
 * of the same sequence. Today it inlines the matrix-load / transpose /
 * per-bp loop twice (once for the single-stream fallback, once for the
 * dual-stream production path). v2 lets the algorithm body live in ONE
 * `[&]` lambda whose only `NumStreams`-aware bit is a fixed-size BP-vector
 * array per call.
 *
 * Handler call shape (when `delays_count == 0` — the only case currently
 * supported here; see static_assert below):
 *
 *     handler_fn(
 *         std::bool_constant<is_full>,
 *         offsets,                  // std::array<size_t, MaxLanes(d) * NumStreams>
 *         d,                        // ScalableTag<storage_unit>
 *         live_mask,                // hn::Mask<D>, full_mask in full-chunk pass,
 *                                   //   transitions full → left → none in tail
 *         bp_v                      // vec_stream<D, ST, NumStreams>, by value
 *     );
 *
 * Phase-4 ABI change: BPs are bundled into a `vec_stream<D, ST, NumStreams>`
 * and passed BY VALUE to `handler_fn`. With `[[clang::trivial_abi]]` on
 * `vec_stream` (16-byte HVA on M1 NEON for W=2), the call still uses the
 * V0/V1 NEON registers the parameter-pack form used in Phase-3, so the
 * register-safety contract (Phase-1) is preserved. Pre-Phase-4 the call
 * site expanded the bundle to a parameter pack via `index_sequence` —
 * removed because v2's caller (`two_stacks_minimizers`) was repacking it
 * into the same bundle on the receiving side, so the unpack→repack was
 * pure boilerplate that obscured the data flow.
 *
 * Lane-count discipline (main/DOCS/GUIDELINES/CODE_STYLE__DOC0145.md §2):
 *   - `lanes_per_stream`     = hn::Lanes(d)              [runtime]
 *   - `total_lanes`          = lanes_per_stream * NumStreams [runtime]
 *   - `max_lanes_per_stream` = hn::MaxLanes(D{})         [constexpr]
 *   - `total_max_lanes`      = max_lanes_per_stream * NumStreams [constexpr]
 *   - Offsets array: sized at `total_max_lanes`, used at runtime up to
 *     `total_lanes` slots.
 *   - Per-stream matrix storage: sized at `max_lanes_per_stream`, used up
 *     to `lanes_per_stream` rows.
 *
 * Delay support: `delays_count == 0` is the only path implemented here.
 * The variadic `delays_ts...` is preserved for API-shape parity with v1
 * but a non-empty pack triggers a `static_assert`. Adding delay support
 * requires per-stream `circular_append_buffer` instances; that is a
 * mechanical extension once a caller actually needs it (none today).
 *
 * The `bps_last_chunk == 0` fix-up (steal one full chunk into the partial
 * path so the partial-mask transitions actually fire and finish_fn always
 * gets called) is the same one v1 carries; the bug-fix comment in v1 also
 * applies verbatim here.
 */
template<
	size_t NumStreams,
	typename... delays_ts,
	supports_compact_sequence_bitcast_view View,
	typename handler_fn_t,
	typename finish_fn_t,
	typename I = typename View::iterator
>
[[gnu::always_inline]] [[gnu::flatten]] inline void
iterate_sequence_bp_simd_v2(
	const View& seq,
	const I& begin,
	const I& end,
	const size_t context_bps,
	handler_fn_t&& handler_fn,
	finish_fn_t&& finish_fn,
	delays_ts... delays
) {
	using seq_traits = typename View::traits;
	constexpr size_t delays_count = sizeof...(delays);

	static_assert(delays_count == 0,
		"iterate_sequence_bp_simd_v2 currently supports only delays_count == 0. "
		"For delayed (multi-stream lock-step) iteration, extend this function "
		"with per-stream circular_append_buffer instances; for delayed "
		"single-stream iteration use the original iterate_sequence_bp_simd_offsets.");
	static_assert(NumStreams >= 1, "NumStreams must be >= 1");

	// Suppress unused-parameter warnings for the parameter-pack args we are
	// preserving for API shape parity with v1.
	((void)delays, ...);

	const auto& [context_units, _ctx_pad] =
		int_div_ceil(context_bps, seq_traits::letters_per_unit);

	const size_t seq_len = seq.size();
	if (seq.bp_size() < context_bps) {
		// Sequence shorter than required context.
		return;
	}

	// Use the non-cv-qualified tag here; vec_stream's lifted ops deduce
	// `D` via `deduce_tag` which strips cv (vector_stream.cppm line ~310),
	// so a `vec_stream<const ScalableTag<…>, …>` declaration would not
	// match the type returned by lift_op. `d` itself stays non-const so
	// `square_lane_matrix<d_unq_t>::transpose(D& d)` (and friends) bind
	// without const-dance — matching what `iterate_sequence_bp_simd_offsets`
	// (v1) achieves via `decltype(d)` propagation.
	using d_unq_t      = hn::ScalableTag<typename seq_traits::storage_unit>;
	using vec_t        = hn::Vec<d_unq_t>;
	using vec_mask_t   = hn::Mask<d_unq_t>;
	using matrix       = square_lane_matrix<d_unq_t>;
	using vec_stream_t = vec_stream<d_unq_t,
		typename seq_traits::storage_unit, NumStreams>;
	d_unq_t d;

	const vec_t bp_bitmask_vec = hn::Set(d, seq_traits::char_mask);
	auto iter = begin;

	const size_t lanes_per_stream             = hn::Lanes(d);
	constexpr size_t max_lanes_per_stream     = hn::MaxLanes(d_unq_t{});
	const size_t total_lanes                  = lanes_per_stream * NumStreams;
	constexpr size_t total_max_lanes          = max_lanes_per_stream * NumStreams;

	// Compute global offsets across ALL streams (NumStreams * Lanes(d) lanes
	// total). Identical recipe to v1's `calculate_iteration_offsets` call,
	// just with a larger `lanes_n` argument.
	const auto& [units_per_lane, offsets] =
		calculate_iteration_offsets<total_max_lanes>(
			seq_len,
			total_lanes,
			context_units);

	// Per-stream offsets slice (computed once before the chunk loop). Indexed
	// at runtime by `lane`, but `s` is constexpr inside the chunk loop so the
	// array indirection is a free address-arithmetic load.
	HWY_ALIGN std::array<std::array<size_t, max_lanes_per_stream>, NumStreams>
		offsets_per_stream{};
	for (size_t s = 0; s < NumStreams; ++s) {
		for (size_t lane = 0; lane < lanes_per_stream; ++lane) {
			offsets_per_stream[s][lane] = offsets[s * lanes_per_stream + lane];
		}
	}

	const size_t bps_per_lane =
		(context_units + units_per_lane) * seq_traits::letters_per_unit;
	auto [full_chunks_count_raw, bps_last_chunk_raw] =
		int_div_floor(bps_per_lane,
			lanes_per_stream * seq_traits::letters_per_unit);

	// Same `bps_last_chunk == 0` fix-up as v1 — keeps the partial-mask
	// transitions firing and `finish_fn` reachable on exact-fit lengths.
	size_t full_chunks_count = full_chunks_count_raw;
	size_t bps_last_chunk    = bps_last_chunk_raw;
	if (bps_last_chunk == 0 && full_chunks_count > 0) {
		--full_chunks_count;
		bps_last_chunk = lanes_per_stream * seq_traits::letters_per_unit;
	}

	const vec_mask_t full_mask = hn::FirstN(d, lanes_per_stream);
	// `left_mask` drops only the LAST lane of the LAST stream — the tail of
	// the sequence (max_offset reaches seq_end → padded). Earlier streams /
	// earlier lanes have plenty of margin and never sit on padding bytes,
	// but we apply the same mask uniformly across streams to match the
	// existing dual-stream production path's behaviour.
	const vec_mask_t left_mask =
		(offsets[0] == offsets[total_lanes - 1])
			? hn::MaskFalse(d)
			: hn::FirstN(d, lanes_per_stream - 1);

	vec_mask_t main_mask = full_mask;
	int32_t main_counter = 0;

	DEBUG_ASSERT(bps_last_chunk >= seq.padding() || bps_last_chunk == 0,
		"Sanity check. bps_last_chunk should always cover at least seq.padding().");
	const int32_t main_counter_only_left =
		static_cast<int32_t>(bps_last_chunk) - static_cast<int32_t>(seq.padding());
	const int32_t main_counter_none =
		static_cast<int32_t>(bps_last_chunk);

	// (4) Per-stream raw matrix storage. With `delays_count == 0` we don't
	// need the circular buffer indirection from v1 — one chunk of rows is
	// loaded, consumed in order, then overwritten by the next chunk.
	HWY_ALIGN std::array<std::array<vec_t, max_lanes_per_stream>, NumStreams>
		raw_matrix_storage;

	// (5) Full-chunk fast path.
	//
	// Phase-4 cleanup: per-row state lives in a `vec_stream<D, ST, NumStreams>`
	// bundle, AND `handler_fn` receives the bundle directly (by value, HVA-
	// passed in V0/V1 NEON registers). The Phase-3 unpack→repack at the
	// handler boundary is gone because the only caller (`two_stacks_minimizers`)
	// repacked the parameter pack into the same bundle on receipt — pure
	// boilerplate that obscured the data flow. The bundle is scalar-replaced
	// inside the impl because the [[gnu::always_inline]] + [[gnu::flatten]]
	// annotations let SROA see through the std::array storage at compile
	// time for W ∈ {1, 2}.
	for (size_t chunk_no = 0; chunk_no < full_chunks_count; ++chunk_no) {
		// Per-stream matrix construction + fill (compile-time unrolled).
		// `for_each_stream<W>` is a `vec_stream`-namespaced alias for the
		// `for_each_index<W>` pattern, signalling that the body iterates
		// over stream slots (the matrix builder isn't a vec_stream lift —
		// each stream gets its own raw `square_lane_matrix`).
		for_each_stream<NumStreams>([&](auto stream_idx_const) [[gnu::always_inline]] {
			constexpr size_t s = decltype(stream_idx_const)::value;
			matrix(raw_matrix_storage[s].data())
				.fill_with_offsets(d, offsets_per_stream[s],
					seq.read_cptr(iter));
		});
		// Advance iter ONCE per chunk — every stream shares the starting
		// position; the offsets-per-stream slices encode per-stream stride.
		// `View::advance_iter` is the view-aware iterator step: for plain
		// `compact_sequence_range<R>` this is `iter += n`; for a
		// `bit_casted_view<…>` it scales by `cast_ratio` so we step by the
		// right number of base-iter increments per `n` view-units.
		View::advance_iter(iter, lanes_per_stream);
		// Per-stream transpose.
		for_each_stream<NumStreams>([&](auto stream_idx_const) [[gnu::always_inline]] {
			constexpr size_t s = decltype(stream_idx_const)::value;
			matrix(raw_matrix_storage[s].data()).transpose(d);
		});

		for (size_t row_index = 0; row_index < lanes_per_stream;
		     ++row_index) {
			// Pack the row vec_t from each per-stream matrix into a single
			// vec_stream. `make_vec_stream_from_per_stream` is the
			// vector_stream.cppm helper that wraps the index_sequence + pack
			// IIFE so it stays one expression here.
			vec_stream_t row = make_vec_stream_from_per_stream<
				d_unq_t, typename seq_traits::storage_unit, NumStreams>(
					raw_matrix_storage, row_index);

			constexpr size_t lpu = seq_traits::letters_per_unit;
			for (size_t bp_index = 0; bp_index < lpu; ++bp_index) {
				const vec_stream_t bp_v = And(row, bp_bitmask_vec);
				row = ShiftRight<seq_traits::bits_per_char>(row);
				handler_fn(std::bool_constant<true>{},
					offsets, d, full_mask, bp_v);
			}
		}
	}

	// (6) Partial-chunk tail. `View::view_distance(iter, end)` returns the
	// remaining storage units in *view* terms — for plain compact ranges
	// this is the raw `end - iter`, for a `bit_casted_view<…>` it scales
	// by `cast_ratio` so partial_fill_with_offsets sees the correct
	// per-row availability.
	const size_t partial_units_remaining = View::view_distance(iter, end);
	for_each_stream<NumStreams>([&](auto stream_idx_const) [[gnu::always_inline]] {
		constexpr size_t s = decltype(stream_idx_const)::value;
		matrix(raw_matrix_storage[s].data())
			.partial_fill_with_offsets(d, partial_units_remaining,
				offsets_per_stream[s], seq.read_cptr(iter));
		matrix(raw_matrix_storage[s].data()).transpose(d);
	});

	if (main_counter_none == 0) {
		// Sequence length was an exact full-chunk multiple — fix-up didn't
		// apply (full_chunks_count was 0 going in). No tail iterations.
		return;
	}

	DEBUG_ASSERT(main_counter_none >= 1, "None counter cannot be less than 1.");
	DEBUG_ASSERT(main_counter_only_left >= 1,
		"Only-left counter cannot be less than 1.");
	DEBUG_ASSERT(main_counter_none >= main_counter_only_left,
		"Counters should have proper order (first should be only_left, then none)");
	DEBUG_ASSERT(static_cast<size_t>(main_counter_none) <= bps_per_lane,
		"Sanity check. If this fails, there's probably size_t overflow.");

	// Tail loop. Same body shape as the full-chunk fast path but with
	// `main_counter` bookkeeping for the full → left → none mask transitions.
	for (size_t row_index = 0; row_index < lanes_per_stream; ++row_index) {
		vec_stream_t row = make_vec_stream_from_per_stream<
			d_unq_t, typename seq_traits::storage_unit, NumStreams>(
				raw_matrix_storage, row_index);

		constexpr size_t lpu = seq_traits::letters_per_unit;
		for (size_t bp_index = 0; bp_index < lpu; ++bp_index) {
			const vec_stream_t bp_v = And(row, bp_bitmask_vec);
			row = ShiftRight<seq_traits::bits_per_char>(row);
			handler_fn(std::bool_constant<false>{},
				offsets, d, main_mask, bp_v);

			++main_counter;
			if (main_counter == main_counter_none) {
				finish_fn(offsets, d, left_mask);
				return;
			} else if (main_counter == main_counter_only_left) {
				main_mask = left_mask;
			}
		}
	}
	// Unreachable in practice: the tail iteration count is bounded by
	// `lanes_per_stream * lpu = bps_last_chunk = main_counter_none` by
	// construction, so the early return inside the loop always fires.
	finish_fn(offsets, d, left_mask);
}

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
    const std::size_t shift_amount = 2 * local_partial_letters;
    local_partial_word |= static_cast<storage_unit_t>(packed_chunk) << shift_amount;
    const std::size_t new_letters = local_partial_letters + 16;
    if (new_letters >= 32) {
        packed_words->push_back(local_partial_word);
        local_partial_word =
            (shift_amount > 32)
                ? (static_cast<storage_unit_t>(packed_chunk) >> (64 - shift_amount))
                : storage_unit_t{0};
        local_partial_letters = new_letters - 32;
    } else {
        local_partial_letters = new_letters;
    }
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
    if (count == 0) return;
    const std::uint32_t valid_mask = (count >= 16)
        ? std::uint32_t{0xFFFFFFFFu}
        : static_cast<std::uint32_t>((std::uint64_t{1} << (2 * count)) - 1u);
    const storage_unit_t valid_chunk =
        static_cast<storage_unit_t>(packed_chunk & valid_mask);
    const std::size_t shift_amount = 2 * local_partial_letters;
    local_partial_word |= valid_chunk << shift_amount;
    const std::size_t new_total = local_partial_letters + count;
    if (new_total >= 32) {
        packed_words->push_back(local_partial_word);
        if (new_total > 32) {
            local_partial_word = valid_chunk >> (64 - shift_amount);
            local_partial_letters = new_total - 32;
        } else {
            local_partial_word = 0;
            local_partial_letters = 0;
        }
    } else {
        local_partial_letters = new_total;
    }
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

    using D = hn::FixedTag<std::uint8_t, 16>;
    constexpr D d_byte;
    constexpr std::size_t lanes = 16;

    DEBUG_ASSERT(state.output_storage != nullptr,
                 "encode_state::output_storage must be configured before encode_body_chunk_dna4_simd");

    static constexpr auto fallback_lut =
        build_letter_to_rank_lut<seq::dna4, Policy>();

    storage_unit_t local_partial_word = state.partial_word;
    std::size_t local_partial_letters = state.partial_letters;
    std::size_t local_total_letters = state.total_letters;
    auto* packed_words = state.output_storage;

    if constexpr (Policy == unknown_byte_policy::DROP) {
        // 16-byte low-nibble LUT — `'A' & 0x0F = 1`, etc. Sentinel
        // (0xFF) appears for newlines, CR, and any unknown byte;
        // chunks containing one fall back to scalar.
        static constexpr auto low_nibble_lut_array =
            build_dna4_low_nibble_lut();
        const auto letter_to_rank_table_vec = hn::LoadDup128(
            d_byte, low_nibble_lut_array.data());
        const auto low_nibble_mask = hn::Set(d_byte, std::uint8_t{0x0F});

        std::size_t read_cursor = 0;

        // Two-chunk unrolled fast path (32 bytes / 64 packed bits =
        // exactly one uint64 storage unit when partial_letters == 0).
        while (read_cursor + 2 * lanes <= body_size) {
            const auto chunk_a = hn::LoadU(d_byte, body_bytes + read_cursor);
            const auto chunk_b = hn::LoadU(d_byte, body_bytes + read_cursor + lanes);

            const auto low_a = hn::And(chunk_a, low_nibble_mask);
            const auto low_b = hn::And(chunk_b, low_nibble_mask);

            const auto ranks_a = hn::TableLookupBytes(letter_to_rank_table_vec, low_a);
            const auto ranks_b = hn::TableLookupBytes(letter_to_rank_table_vec, low_b);

            if (HWY_UNLIKELY(has_skip_sentinel(d_byte, ranks_a) ||
                             has_skip_sentinel(d_byte, ranks_b))) {
                for (std::size_t scalar_offset = 0;
                     scalar_offset < 2 * lanes; ++scalar_offset) {
                    const std::uint8_t letter_rank =
                        fallback_lut[body_bytes[read_cursor + scalar_offset]];
                    if (letter_rank != 0xFF) {
                        local_partial_word |=
                            static_cast<storage_unit_t>(letter_rank)
                            << (BitsPerChar * local_partial_letters);
                        local_partial_letters += 1;
                        local_total_letters += 1;
                        if (local_partial_letters == LettersPerUnit) {
                            packed_words->push_back(local_partial_word);
                            local_partial_word = 0;
                            local_partial_letters = 0;
                        }
                    }
                }
                read_cursor += 2 * lanes;
                continue;
            }

            const std::uint32_t packed_a = pack_16_dna4_to_u32(d_byte, ranks_a);
            const std::uint32_t packed_b = pack_16_dna4_to_u32(d_byte, ranks_b);

            merge_packed_chunk_into_partial(local_partial_word,
                                            local_partial_letters,
                                            packed_words,
                                            packed_a);
            merge_packed_chunk_into_partial(local_partial_word,
                                            local_partial_letters,
                                            packed_words,
                                            packed_b);
            local_total_letters += 2 * lanes;
            read_cursor += 2 * lanes;
        }

        // Single-chunk path for the trailing 16-byte block.
        if (read_cursor + lanes <= body_size) {
            const auto chunk_bytes = hn::LoadU(d_byte, body_bytes + read_cursor);
            const auto low_nibbles = hn::And(chunk_bytes, low_nibble_mask);
            const auto letter_ranks = hn::TableLookupBytes(
                letter_to_rank_table_vec, low_nibbles);

            if (HWY_UNLIKELY(has_skip_sentinel(d_byte, letter_ranks))) {
                for (std::size_t scalar_offset = 0;
                     scalar_offset < lanes; ++scalar_offset) {
                    const std::uint8_t letter_rank =
                        fallback_lut[body_bytes[read_cursor + scalar_offset]];
                    if (letter_rank != 0xFF) {
                        local_partial_word |=
                            static_cast<storage_unit_t>(letter_rank)
                            << (BitsPerChar * local_partial_letters);
                        local_partial_letters += 1;
                        local_total_letters += 1;
                        if (local_partial_letters == LettersPerUnit) {
                            packed_words->push_back(local_partial_word);
                            local_partial_word = 0;
                            local_partial_letters = 0;
                        }
                    }
                }
            } else {
                const std::uint32_t packed = pack_16_dna4_to_u32(d_byte, letter_ranks);
                merge_packed_chunk_into_partial(local_partial_word,
                                                local_partial_letters,
                                                packed_words,
                                                packed);
                local_total_letters += lanes;
            }
            read_cursor += lanes;
        }

        // Scalar tail.
        while (read_cursor < body_size) {
            const std::uint8_t letter_rank = fallback_lut[body_bytes[read_cursor]];
            if (letter_rank != 0xFF) {
                local_partial_word |= static_cast<storage_unit_t>(letter_rank)
                                      << (BitsPerChar * local_partial_letters);
                local_partial_letters += 1;
                local_total_letters += 1;
                if (local_partial_letters == LettersPerUnit) {
                    packed_words->push_back(local_partial_word);
                    local_partial_word = 0;
                    local_partial_letters = 0;
                }
            }
            read_cursor += 1;
        }
    } else {
        // MAP_TO_DEFAULT: use the 32-byte two-half LUT. No sentinels,
        // no fallback path — every input byte produces a valid rank.
        static constexpr auto lut32 =
            build_letter_to_rank_lut_32<seq::dna4, Policy>();
        const auto lut_lo = hn::LoadDup128(d_byte, lut32[0].data());
        const auto lut_hi = hn::LoadDup128(d_byte, lut32[1].data());

        std::size_t read_cursor = 0;
        while (read_cursor + lanes <= body_size) {
            const auto chunk_bytes = hn::LoadU(d_byte, body_bytes + read_cursor);
            const auto ranks = lookup_ranks_via_32byte_lut(
                d_byte, lut_lo, lut_hi, chunk_bytes);
            const std::uint32_t packed_chunk =
                pack_16_dna4_to_u32(d_byte, ranks);
            merge_packed_chunk_into_partial(local_partial_word,
                                            local_partial_letters,
                                            packed_words,
                                            packed_chunk);
            local_total_letters += lanes;
            read_cursor += lanes;
        }
        // Scalar tail (always-valid rank under MAP_TO_DEFAULT).
        while (read_cursor < body_size) {
            const std::uint8_t letter_rank = fallback_lut[body_bytes[read_cursor]];
            local_partial_word |= static_cast<storage_unit_t>(letter_rank)
                                  << (BitsPerChar * local_partial_letters);
            local_partial_letters += 1;
            local_total_letters += 1;
            if (local_partial_letters == LettersPerUnit) {
                packed_words->push_back(local_partial_word);
                local_partial_word = 0;
                local_partial_letters = 0;
            }
            read_cursor += 1;
        }
    }

    state.partial_word = local_partial_word;
    state.partial_letters = local_partial_letters;
    state.total_letters = local_total_letters;
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

    using D = hn::FixedTag<std::uint8_t, 16>;
    constexpr D d_byte;
    constexpr std::size_t lanes = 16;

    static constexpr auto fallback_lut =
        build_letter_to_rank_lut<seq::dna4>();
    static constexpr auto low_nibble_lut_array =
        build_dna4_low_nibble_lut();

    const auto letter_to_rank_table_vec =
        hn::LoadDup128(d_byte, low_nibble_lut_array.data());
    const auto low_nibble_mask = hn::Set(d_byte, std::uint8_t{0x0F});
    const auto sentinel_marker = hn::Set(d_byte, std::uint8_t{0xFF});

    storage_unit_t local_partial_word = state.partial_word;
    std::size_t local_partial_letters = state.partial_letters;
    std::size_t local_total_letters = state.total_letters;
    auto* packed_words = state.output_storage;

    std::size_t read_cursor = 0;
    while (read_cursor + lanes <= body_size) {
        const auto chunk_bytes =
            hn::LoadU(d_byte, body_bytes + read_cursor);
        const auto low_nibbles = hn::And(chunk_bytes, low_nibble_mask);
        const auto ranks =
            hn::TableLookupBytes(letter_to_rank_table_vec, low_nibbles);
        const auto sentinel_eq = hn::Eq(ranks, sentinel_marker);

        if (HWY_LIKELY(hn::AllFalse(d_byte, sentinel_eq))) {
            const std::uint32_t packed_chunk =
                pack_16_dna4_to_u32(d_byte, ranks);
            merge_packed_chunk_into_partial(
                local_partial_word, local_partial_letters, packed_words,
                packed_chunk);
            local_total_letters += lanes;
            read_cursor += lanes;
            continue;
        }

        // Slow path: chunk contains newline/unknown sentinels. Keep this
        // target-independent; mask bit ordering differs across Highway
        // backends, while this path only runs at multiline boundaries.
        for (std::size_t lane = 0; lane < lanes; ++lane) {
            const std::uint8_t letter_rank =
                fallback_lut[body_bytes[read_cursor + lane]];
            if (letter_rank == 0xFF) continue;
            local_partial_word |=
                static_cast<storage_unit_t>(letter_rank)
                << (BitsPerChar * local_partial_letters);
            local_partial_letters += 1;
            local_total_letters += 1;
            if (local_partial_letters == LettersPerUnit) {
                packed_words->push_back(local_partial_word);
                local_partial_word = 0;
                local_partial_letters = 0;
            }
        }
        read_cursor += lanes;
    }

    while (read_cursor < body_size) {
        const std::uint8_t letter_rank =
            fallback_lut[body_bytes[read_cursor]];
        if (letter_rank != 0xFF) {
            local_partial_word |=
                static_cast<storage_unit_t>(letter_rank)
                << (BitsPerChar * local_partial_letters);
            local_partial_letters += 1;
            local_total_letters += 1;
            if (local_partial_letters == LettersPerUnit) {
                packed_words->push_back(local_partial_word);
                local_partial_word = 0;
                local_partial_letters = 0;
            }
        }
        read_cursor += 1;
    }

    state.partial_word = local_partial_word;
    state.partial_letters = local_partial_letters;
    state.total_letters = local_total_letters;
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

    using D = hn::FixedTag<std::uint8_t, 16>;
    constexpr D d_byte;
    constexpr std::size_t lanes = 16;

    static constexpr auto fallback_lut =
        build_letter_to_rank_lut<seq::dna4>();
    static constexpr auto low_nibble_lut_array =
        build_dna4_low_nibble_lut();

    const auto letter_to_rank_table_vec =
        hn::LoadDup128(d_byte, low_nibble_lut_array.data());
    const auto low_nibble_mask = hn::Set(d_byte, std::uint8_t{0x0F});
    const auto sentinel_marker = hn::Set(d_byte, std::uint8_t{0xFF});
    const auto boundary_marker = hn::Set(d_byte, BoundaryChar);
    const auto lf_marker = hn::Set(d_byte, std::uint8_t{'\n'});
    const auto cr_marker = hn::Set(d_byte, std::uint8_t{'\r'});

    storage_unit_t local_partial_word = state.partial_word;
    std::size_t local_partial_letters = state.partial_letters;
    std::size_t local_total_letters = state.total_letters;
    std::size_t local_newline_count = 0;
    auto* packed_words = state.output_storage;

    std::size_t read_cursor = 0;

    auto flush_state_and_return = [&](std::size_t consumed) {
        state.partial_word = local_partial_word;
        state.partial_letters = local_partial_letters;
        state.total_letters = local_total_letters;
        if (newline_count_out != nullptr) {
            *newline_count_out += local_newline_count;
        }
        return consumed;
    };

    while (read_cursor + lanes <= body_size) {
        const auto chunk_bytes =
            hn::LoadU(d_byte, body_bytes + read_cursor);

        const auto boundary_eq = hn::Eq(chunk_bytes, boundary_marker);
        if (HWY_UNLIKELY(!hn::AllFalse(d_byte, boundary_eq))) {
            const std::size_t boundary_position = static_cast<std::size_t>(
                hn::FindFirstTrue(d_byte, boundary_eq));
            const std::size_t scalar_end =
                read_cursor + boundary_position;
            for (std::size_t scalar_offset = read_cursor;
                 scalar_offset < scalar_end; ++scalar_offset) {
                const std::uint8_t b = body_bytes[scalar_offset];
                if (b == '\n' || b == '\r') {
                    local_newline_count += 1;
                    continue;
                }
                const std::uint8_t rank = fallback_lut[b];
                if (rank != 0xFF) {
                    local_partial_word |=
                        static_cast<storage_unit_t>(rank)
                        << (BitsPerChar * local_partial_letters);
                    local_partial_letters += 1;
                    local_total_letters += 1;
                    if (local_partial_letters == LettersPerUnit) {
                        packed_words->push_back(local_partial_word);
                        local_partial_word = 0;
                        local_partial_letters = 0;
                    }
                }
            }
            return flush_state_and_return(scalar_end);
        }

        const auto low_nibbles = hn::And(chunk_bytes, low_nibble_mask);
        const auto ranks =
            hn::TableLookupBytes(letter_to_rank_table_vec, low_nibbles);
        const auto sentinel_eq = hn::Eq(ranks, sentinel_marker);

        if (HWY_LIKELY(hn::AllFalse(d_byte, sentinel_eq))) {
            const std::uint32_t packed_chunk =
                pack_16_dna4_to_u32(d_byte, ranks);
            merge_packed_chunk_into_partial(
                local_partial_word, local_partial_letters, packed_words,
                packed_chunk);
            local_total_letters += lanes;
            read_cursor += lanes;
            continue;
        }

        const auto lf_eq = hn::Eq(chunk_bytes, lf_marker);
        const auto cr_eq = hn::Eq(chunk_bytes, cr_marker);
        const std::uint64_t newline_bits =
            hn::BitsFromMask(d_byte, hn::Or(lf_eq, cr_eq));
        local_newline_count += static_cast<std::size_t>(
            std::popcount(newline_bits & 0xFFFFu));

        // Boundary was checked above; this chunk only needs newline/unknown
        // filtering. Keep the rare multiline-boundary path scalar so it is
        // independent of target-specific mask bit ordering.
        for (std::size_t lane = 0; lane < lanes; ++lane) {
            const std::uint8_t current_byte = body_bytes[read_cursor + lane];
            if (current_byte == '\n' || current_byte == '\r') continue;
            const std::uint8_t rank = fallback_lut[current_byte];
            if (rank == 0xFF) continue;
            local_partial_word |=
                static_cast<storage_unit_t>(rank)
                << (BitsPerChar * local_partial_letters);
            local_partial_letters += 1;
            local_total_letters += 1;
            if (local_partial_letters == LettersPerUnit) {
                packed_words->push_back(local_partial_word);
                local_partial_word = 0;
                local_partial_letters = 0;
            }
        }
        read_cursor += lanes;
    }

    while (read_cursor < body_size) {
        const std::uint8_t current_byte = body_bytes[read_cursor];
        if (current_byte == BoundaryChar) {
            return flush_state_and_return(read_cursor);
        }
        if (current_byte == '\n' || current_byte == '\r') {
            local_newline_count += 1;
            read_cursor += 1;
            continue;
        }
        const std::uint8_t rank = fallback_lut[current_byte];
        if (rank != 0xFF) {
            local_partial_word |=
                static_cast<storage_unit_t>(rank)
                << (BitsPerChar * local_partial_letters);
            local_partial_letters += 1;
            local_total_letters += 1;
            if (local_partial_letters == LettersPerUnit) {
                packed_words->push_back(local_partial_word);
                local_partial_word = 0;
                local_partial_letters = 0;
            }
        }
        read_cursor += 1;
    }

    return flush_state_and_return(read_cursor);
}

// ============================================================================
// dna4 SIMD decoder — packed uint64 → ASCII bytes.
// ============================================================================

/// Decode 16 packed dna4 source bytes (= 2 packed words = 64 letters)
/// into 64 ASCII bytes via SIMD spread + LUT.
template <class D>
HWY_INLINE void unpack_16_bytes_to_64_ascii(
    D d_byte,
    hn::Vec<D> packed_bytes,
    hn::Vec<D> rank_to_ascii_lut,
    std::uint8_t* HWY_RESTRICT dst) noexcept {
    const auto mask_low_2 = hn::Set(d_byte, std::uint8_t{0x03});

    const auto r0 = hn::And(packed_bytes, mask_low_2);
    const auto r1 = hn::And(hn::ShiftRight<2>(packed_bytes), mask_low_2);
    const auto r2 = hn::And(hn::ShiftRight<4>(packed_bytes), mask_low_2);
    const auto r3 = hn::And(hn::ShiftRight<6>(packed_bytes), mask_low_2);

    using d_u16_t = hn::Repartition<std::uint16_t, D>;
    d_u16_t d_u16;

    const auto r01_lo = hn::InterleaveLower(d_byte, r0, r1);
    const auto r01_hi = hn::InterleaveUpper(d_byte, r0, r1);
    const auto r23_lo = hn::InterleaveLower(d_byte, r2, r3);
    const auto r23_hi = hn::InterleaveUpper(d_byte, r2, r3);

    const auto chunk0 = hn::BitCast(
        d_byte,
        hn::InterleaveLower(d_u16,
                             hn::BitCast(d_u16, r01_lo),
                             hn::BitCast(d_u16, r23_lo)));
    const auto chunk1 = hn::BitCast(
        d_byte,
        hn::InterleaveUpper(d_u16,
                             hn::BitCast(d_u16, r01_lo),
                             hn::BitCast(d_u16, r23_lo)));
    const auto chunk2 = hn::BitCast(
        d_byte,
        hn::InterleaveLower(d_u16,
                             hn::BitCast(d_u16, r01_hi),
                             hn::BitCast(d_u16, r23_hi)));
    const auto chunk3 = hn::BitCast(
        d_byte,
        hn::InterleaveUpper(d_u16,
                             hn::BitCast(d_u16, r01_hi),
                             hn::BitCast(d_u16, r23_hi)));

    const auto ascii0 = hn::TableLookupBytes(rank_to_ascii_lut, chunk0);
    const auto ascii1 = hn::TableLookupBytes(rank_to_ascii_lut, chunk1);
    const auto ascii2 = hn::TableLookupBytes(rank_to_ascii_lut, chunk2);
    const auto ascii3 = hn::TableLookupBytes(rank_to_ascii_lut, chunk3);

    constexpr std::size_t lanes_per_chunk = 16;
    hn::StoreU(ascii0, d_byte, dst + 0 * lanes_per_chunk);
    hn::StoreU(ascii1, d_byte, dst + 1 * lanes_per_chunk);
    hn::StoreU(ascii2, d_byte, dst + 2 * lanes_per_chunk);
    hn::StoreU(ascii3, d_byte, dst + 3 * lanes_per_chunk);
}

/// Decode `total_letters` letters from packed dna4 words into `dst`.
HWY_INLINE void decode_dna4_words_to_ascii(
    const std::uint64_t* HWY_RESTRICT words,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t total_letters) noexcept {
    using D = hn::FixedTag<std::uint8_t, 16>;
    constexpr D d_byte;

    static constexpr auto rank_to_ascii_lut_array =
        build_dna4_rank_to_ascii_lut();
    const auto rank_to_ascii_lut = hn::LoadDup128(
        d_byte, rank_to_ascii_lut_array.data());

    constexpr std::size_t letters_per_iter = 64;

    std::size_t letters_emitted = 0;
    std::size_t word_cursor = 0;
    const auto* packed_bytes_base =
        reinterpret_cast<const std::uint8_t*>(words);

    while (letters_emitted + letters_per_iter <= total_letters) {
        const auto packed_bytes = hn::LoadU(
            d_byte, packed_bytes_base + word_cursor * 8);
        unpack_16_bytes_to_64_ascii(d_byte, packed_bytes,
                                     rank_to_ascii_lut, dst + letters_emitted);
        letters_emitted += letters_per_iter;
        word_cursor += 2;
    }

    while (letters_emitted < total_letters) {
        const std::size_t word_index = letters_emitted / 32;
        const std::size_t letter_in_word = letters_emitted % 32;
        const std::uint64_t word = words[word_index];
        const std::uint8_t rank = static_cast<std::uint8_t>(
            (word >> (2 * letter_in_word)) & 0x3u);
        dst[letters_emitted] = rank_to_ascii_lut_array[rank];
        ++letters_emitted;
    }
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
    using D = hn::FixedTag<std::uint8_t, 16>;
    constexpr D d_byte;
    constexpr std::size_t lanes = 16;

    DEBUG_ASSERT(state.output_storage != nullptr,
                 "encode_state::output_storage must be configured before encode_body_chunk_dna5_uint8_simd");

    static constexpr auto lut32 =
        build_letter_to_rank_lut_32<seq::dna5, Policy>();
    static constexpr auto lut256 =
        build_letter_to_rank_lut<seq::dna5, Policy>();
    const auto lut_lo = hn::LoadDup128(d_byte, lut32[0].data());
    const auto lut_hi = hn::LoadDup128(d_byte, lut32[1].data());

    auto* packed_words = state.output_storage;
    std::uint8_t local_partial_word = state.partial_word;
    std::size_t local_partial_letters = state.partial_letters;
    std::size_t local_total_letters = state.total_letters;

    auto append_one_rank = [&](std::uint8_t rank) {
        local_partial_word |=
            static_cast<std::uint8_t>(rank << (3 * local_partial_letters));
        local_partial_letters += 1;
        local_total_letters += 1;
        if (local_partial_letters == 2) {
            packed_words->push_back(local_partial_word);
            local_partial_word = 0;
            local_partial_letters = 0;
        }
    };

    std::size_t cursor = 0;

    // Compaction shuffles for the SIMD pack fast path.
#if !BIOSEQ_SIMD_X86_PRE_AVX3_FIXED128
    HWY_ALIGN constexpr std::uint8_t even_indices_pattern[16] = {
        0, 2, 4, 6, 8, 10, 12, 14,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    HWY_ALIGN constexpr std::uint8_t odd_indices_pattern[16] = {
        1, 3, 5, 7, 9, 11, 13, 15,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const auto even_indices = hn::LoadU(d_byte, even_indices_pattern);
    const auto odd_indices = hn::LoadU(d_byte, odd_indices_pattern);
#endif
    HWY_ALIGN std::uint8_t store_buf[16];

    while (cursor + lanes <= body_size) {
        const auto chunk = hn::LoadU(d_byte, body_bytes + cursor);
        const auto ranks = lookup_ranks_via_32byte_lut(
            d_byte, lut_lo, lut_hi, chunk);

        if constexpr (Policy == unknown_byte_policy::DROP) {
            const auto sentinel_eq = hn::Eq(ranks, hn::Set(d_byte, std::uint8_t{0xFF}));
            if (HWY_UNLIKELY(!hn::AllFalse(d_byte, sentinel_eq))) {
                HWY_ALIGN std::uint8_t scratch[16];
                hn::Store(ranks, d_byte, scratch);
                for (std::size_t i = 0; i < lanes; ++i) {
                    if (scratch[i] == 0xFF) continue;
                    append_one_rank(scratch[i]);
                }
                cursor += lanes;
                continue;
            }
        }

        // Fast path: 16 valid ranks. If the partial state is empty,
        // the SIMD pack writes 8 contiguous bytes. If partial_letters
        // == 1, fall back to scalar to keep state coherent (rare).
        if (local_partial_letters == 0) {
#if BIOSEQ_SIMD_X86_PRE_AVX3_FIXED128
            HWY_ALIGN std::uint8_t scratch[16];
            hn::Store(ranks, d_byte, scratch);
            for (std::size_t i = 0; i < lanes; i += 2) {
                store_buf[i / 2] = static_cast<std::uint8_t>(
                    (scratch[i] & 0x07u) | ((scratch[i + 1] & 0x07u) << 3));
            }
#else
            const auto evens = hn::TableLookupBytes(ranks, even_indices);
            const auto odds = hn::TableLookupBytes(ranks, odd_indices);
            const auto packed = hn::Or(evens, hn::ShiftLeft<3>(odds));
            hn::Store(packed, d_byte, store_buf);
#endif
            packed_words->insert(packed_words->end(),
                                  store_buf, store_buf + 8);
            local_total_letters += lanes;
        } else {
            HWY_ALIGN std::uint8_t scratch[16];
            hn::Store(ranks, d_byte, scratch);
            for (std::size_t i = 0; i < lanes; ++i) {
                append_one_rank(scratch[i]);
            }
        }
        cursor += lanes;
    }

    // Scalar tail.
    while (cursor < body_size) {
        const std::uint8_t rank = lut256[body_bytes[cursor]];
        if constexpr (Policy == unknown_byte_policy::DROP) {
            if (rank == 0xFF) {
                ++cursor;
                continue;
            }
        }
        append_one_rank(rank);
        ++cursor;
    }

    state.partial_word = local_partial_word;
    state.partial_letters = local_partial_letters;
    state.total_letters = local_total_letters;
}

// ============================================================================
// dna5 SIMD decoder — `(dna5, uint8, 3, 2)`: 16 packed bytes → 32 chars.
// ============================================================================

HWY_INLINE void decode_dna5_uint8_words_to_ascii(
    const std::uint8_t* HWY_RESTRICT words,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t total_letters) noexcept {
    using D = hn::FixedTag<std::uint8_t, 16>;
    constexpr D d_byte;

    static constexpr auto rank_to_ascii_lut_array =
        build_dna5_rank_to_ascii_lut();
    const auto rank_to_ascii_lut = hn::LoadDup128(
        d_byte, rank_to_ascii_lut_array.data());

    constexpr std::size_t letters_per_iter = 32;
    constexpr std::size_t bytes_per_iter = 16;

    std::size_t letters_emitted = 0;
    std::size_t bytes_consumed = 0;
    while (letters_emitted + letters_per_iter <= total_letters) {
        const auto packed = hn::LoadU(d_byte, words + bytes_consumed);
        const auto mask3 = hn::Set(d_byte, std::uint8_t{0x07});
        const auto rank_a = hn::And(packed, mask3);
        const auto rank_b = hn::And(hn::ShiftRight<3>(packed), mask3);

        const auto interleaved_lo =
            hn::InterleaveLower(d_byte, rank_a, rank_b);
        const auto interleaved_hi =
            hn::InterleaveUpper(d_byte, rank_a, rank_b);
        const auto ascii_lo =
            hn::TableLookupBytes(rank_to_ascii_lut, interleaved_lo);
        const auto ascii_hi =
            hn::TableLookupBytes(rank_to_ascii_lut, interleaved_hi);
        hn::StoreU(ascii_lo, d_byte, dst + letters_emitted);
        hn::StoreU(ascii_hi, d_byte, dst + letters_emitted + 16);
        letters_emitted += letters_per_iter;
        bytes_consumed += bytes_per_iter;
    }

    while (letters_emitted < total_letters) {
        const std::size_t byte_idx = letters_emitted / 2;
        const std::size_t letter_in_byte = letters_emitted % 2;
        const std::uint8_t byte = words[byte_idx];
        const std::uint8_t rank = static_cast<std::uint8_t>(
            (byte >> (3 * letter_in_byte)) & 0x07u);
        dst[letters_emitted] = rank_to_ascii_lut_array[rank];
        ++letters_emitted;
    }
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
    using D = hn::FixedTag<std::uint8_t, 16>;
    constexpr D d_byte;
    constexpr std::size_t lanes = 16;

    static constexpr auto lut32 =
        build_letter_to_rank_lut_32<alphabet_t, Policy>();
    const auto lut_lo = hn::LoadDup128(d_byte, lut32[0].data());
    const auto lut_hi = hn::LoadDup128(d_byte, lut32[1].data());

    HWY_ALIGN std::uint8_t scratch[16];
    std::size_t cursor = 0;
    while (cursor + lanes <= body_size) {
        const auto chunk = hn::LoadU(d_byte, body_bytes + cursor);
        const auto ranks = lookup_ranks_via_32byte_lut(
            d_byte, lut_lo, lut_hi, chunk);
        hn::Store(ranks, d_byte, scratch);
        append_ranks<storage_unit_t, BitsPerChar, LettersPerUnit>(
            state, scratch, lanes);
        cursor += lanes;
    }
    if (cursor < body_size) {
        // Scalar tail via the 256-entry LUT (kept consistent with
        // append_ranks's 0xFF skip handling).
        static constexpr auto lut256 =
            build_letter_to_rank_lut<alphabet_t, Policy>();
        const std::size_t tail = body_size - cursor;
        std::array<std::uint8_t, 16> tail_scratch{};
        for (std::size_t i = 0; i < tail; ++i) {
            tail_scratch[i] = lut256[body_bytes[cursor + i]];
        }
        append_ranks<storage_unit_t, BitsPerChar, LettersPerUnit>(
            state, tail_scratch.data(), tail);
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
        target_storage_byte_width == 1 || target_storage_byte_width == 2
        || target_storage_byte_width == 4 || target_storage_byte_width == 8,
        fmt::format("stdc_convert_from_cstr: storage_unit_byte_width {} not in {{1,2,4,8}}",
                    static_cast<unsigned>(target_storage_byte_width)));

    StdcT* result = nullptr;
    dispatch_storage_unit_byte_width(target_storage_byte_width,
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
inline StdcT* stdc_recode_to_type_params(
    const StdcT& src,
    compact_sequence_stdc_type_params target,
    Alloc&& alloc)
{
    if (target.alphabet_code == src.alphabet_code) {
        // Same alphabet, width change. recast_storage_width copies the
        // packed payload with a raw memcpy, which preserves the sequence
        // ONLY when the alphabet's letters pack identically across storage
        // widths — i.e. each byte holds a whole number of letters
        // (8 % letter_bit_width == 0). dna4 (2 bits/letter) qualifies; dna5
        // (3 bits/letter) does NOT — its letters straddle byte boundaries
        // differently at each width, so a memcpy garbles the value
        // (DOC0268 Appendix: 'ACGTN'::seq('dna5',32) decoded as "ACACG").
        // For non-byte-aligned alphabets fall through to the
        // alphabet-preserving ASCII round-trip below, which re-packs the
        // letters correctly for the target width.
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
    }
    StdcT* result = nullptr;
    StdcT::supported_alphabets_type::dispatch_alphabet_by_code(target.alphabet_code,
        [&]<class TargetA>() {
            result = stdc_recode_to<StdcT, TargetA>(
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
            // The dispatch_instance fanout synthesises every (alphabet,
            // storage_unit) pair at compile time, but at runtime the
            // DEBUG_ASSERT above pins same alphabet and the typmod pins
            // same storage_unit_byte_width. Compile-time guards filter
            // out unreachable pairs so we never try to instantiate
            // bit_cast_to_storage_unit / compare_sequences_scalar on
            // mismatched types (e.g. dna5 has 3 bits/char which doesn't
            // pack evenly into smaller storage units, so the
            // bit_cast_to_storage_unit constraint fails on dna5).
            if constexpr (
                !std::is_same_v<typename TraitsA::alphabet_type,
                                typename TraitsB::alphabet_type>
            ) {
                (void)ra;
                (void)rb;
            } else if constexpr (
                std::is_same_v<typename TraitsA::storage_unit,
                               typename TraitsB::storage_unit>
            ) {
                result = compare_sequences_scalar(ra, rb);
            } else if constexpr (
                is_sequence_bit_castable_to<Sa, typename TraitsB::storage_unit>
                && sizeof(typename TraitsA::storage_unit) >=
                   sizeof(typename TraitsB::storage_unit)
            ) {
                const auto va =
                    ra.template bit_cast_to_storage_unit<typename TraitsB::storage_unit>();
                result = compare_sequences_scalar(va, rb);
            } else if constexpr (
                is_sequence_bit_castable_to<Sb, typename TraitsA::storage_unit>
            ) {
                const auto vb =
                    rb.template bit_cast_to_storage_unit<typename TraitsA::storage_unit>();
                result = compare_sequences_scalar(ra, vb);
            } else {
                // Unreachable at runtime: same alphabet + different
                // storage widths that can't bit-cast (e.g. dna5 with
                // 3-bit leftover). This is a caller-contract
                // violation, so per DOC0260 R8 raise a load-bearing
                // bioseq_log<ERR, PG>( …) — DEBUG_ASSERT would be silent in
                // release builds and return std::strong_ordering::equal
                // on a value mismatch. NO_POSTGRES build path keeps the
                // historical DEBUG_ASSERT (aligner exec never hits
                // cross-storage-width compares with dna5).
                (void)ra;
                (void)rb;
#ifndef NO_POSTGRES
                bioseq_log<ERR, PG>(
                    "stdc_compare: cross-storage-width same-alphabet pair "
                    "with non-bit-castable layout (alphabet does not satisfy "
                    "is_sequence_custom_alphabet_bit_castable_to)");
#else
                DEBUG_ASSERT(false,
                    "stdc_compare: cross-storage-width same-alphabet pair "
                    "with non-bit-castable layout (dna5 + mismatched width?)");
#endif
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
}

}

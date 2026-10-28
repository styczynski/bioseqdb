/**
 * @file streaming.cppm
 * @brief Streaming packing state, scalar codecs, and rank lookup, tables.
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
#include <concepts>
#include <expected>
#include <limits>
#include <optional>
#include <span>

#include <boost/integer.hpp>

export module bioseq.sequences.simd:streaming;

import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.rank_pack_core;
export import bioseq.sequences.rank_pack;
import bioseq.simd.vector_stream;

export namespace bioseq::sequences {

/** Conversion failures cross the PostgreSQL boundary as values. */
enum class sequence_conversion_error_code : std::uint8_t {
    invalid_frame, invalid_interval, unsupported_genetic_code, incompatible_alphabets,
    incomplete_codon, unrepresentable_symbol, size_overflow, output_capacity, allocation_failure,
    invalid_source_rank, source_read_failed, output_contract_violation
};

/** Coordinates refer to the supplied logical source. */
struct sequence_conversion_error {
    sequence_conversion_error_code code{};
    std::optional<std::size_t> input_position{};
    std::optional<std::size_t> oriented_position{};
    std::optional<std::uint8_t> source_rank{};
    std::optional<rank_pack_status> pack_status{};
};

/** Existing checked packing is the destination contract for all conversions. */
template<class Writer>
concept sequence_conversion_sink = requires(Writer& writer, std::uint8_t rank,
        typename Writer::storage_unit packed_word, std::size_t packed_count,
        std::uint64_t symbols, std::uint64_t units) {
    typename Writer::traits;
    typename Writer::traits::alphabet_type;
    typename Writer::storage_unit;
    { writer.append_prepacked_ranks(packed_word, packed_count) } -> std::same_as<rank_pack_status>;
    { writer.append_rank(rank) } -> std::same_as<rank_pack_status>;
    { writer.finish(symbols, units) } -> std::same_as<rank_pack_result>;
};

/** Native destination ranks and representability for each source rank. */
template<std::size_t SourceSize>
struct native_rank_recode_table {
    std::array<std::uint8_t, SourceSize> destination_ranks{};
    std::array<std::uint8_t, SourceSize> valid{};
};

/** Derive exact DNA and explicitly reduced protein mappings from SeqAn. */
template<seqan3::alphabet Source, seqan3::alphabet Destination>
[[nodiscard]] consteval auto make_native_rank_recode_table() {
    static_assert((seqan3::nucleotide_alphabet<Source> && seqan3::nucleotide_alphabet<Destination>)
        || (seqan3::aminoacid_alphabet<Source> && seqan3::aminoacid_alphabet<Destination>));
    static_assert(seqan3::alphabet_size<Destination> <= 256u);
    native_rank_recode_table<seqan3::alphabet_size<Source>> table{};
    for (std::size_t rank = 0; rank < table.destination_ranks.size(); ++rank) {
        const auto character = seqan3::to_char(seqan3::assign_rank_to(
            static_cast<seqan3::alphabet_rank_t<Source>>(rank), Source{}));
        const auto destination = seqan3::assign_char_to(character, Destination{});
        table.destination_ranks[rank] = static_cast<std::uint8_t>(seqan3::to_rank(destination));
        if constexpr (seqan3::nucleotide_alphabet<Source>)
            table.valid[rank] = static_cast<std::uint8_t>(seqan3::to_char(destination) == character);
        else table.valid[rank] = 1u;
    }
    return table;
}

/** Exact progress, excluding a rejected byte, for streaming character input. */
enum class sequence_encode_status : std::uint8_t {
    ok, invalid_character, capacity_overflow, arithmetic_overflow
};
struct sequence_encode_result {
    sequence_encode_status status{sequence_encode_status::ok};
    std::size_t consumed_bytes{};
    std::size_t emitted_symbols{};
    std::uint8_t offending_byte{};
    /** True only after all requested bytes were handled successfully. */
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return status == sequence_encode_status::ok;
    }
};

} // namespace bioseq::sequences

export {

// Helpers in the selected Highway target namespace.
HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;
using bioseq::sequences::sequence_encode_result;
using bioseq::sequences::sequence_encode_status;


/**
 * Controls how a character lookup handles an unrecognized byte.
 *
 * drop omits invalid bytes; reject reports their exact input offset.
 * map_to_default delegates every byte to the destination SeqAn alphabet.
 */
enum class unknown_byte_policy {
    drop,
    map_to_default,
    reject,
};

/**
 * Packing state  for one sequence record.
 *
 * Completed words are appended to a caller-owned vector. A partial word stays
 * in this state between input chunks until the record is finished The same
 * vector can hold several records, each starting at record_start_offset.
 */
template <typename p_storage_unit,
          std::size_t p_bits_per_character,
          std::size_t p_letters_per_unit>
struct encode_state {
    using base_container = std::vector<p_storage_unit>;

    /// Caller-owned vector receiving completed packed words.
    base_container* output_storage = nullptr;
    /// Output-vector size when reset() started this record.
    std::size_t record_start_offset = 0;
    /// Bits accumulated for the partial word that has not yet been
    /// pushed into `*output_storage`.
    p_storage_unit partial_word = 0;
    /// Number of letters in `partial_word`. Always `< p_letters_per_unit`
    /// on call-boundary entry / exit.
    std::size_t partial_letters = 0;
    /// Number of letters kept for this record, used to cherk FASTQ quality length.
    std::size_t total_letters = 0;

    /// Starts a new record without clearing the output vector or changing its pointer.
    inline void reset() noexcept {
        record_start_offset =
            (output_storage != nullptr) ? output_storage->size() : 0;
        partial_word = 0;
        partial_letters = 0;
        total_letters = 0;
    }
};

namespace detail {

void decode_dna4_words_to_ascii_impl(
    const std::uint64_t* HWY_RESTRICT words,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t total_letters) noexcept;
void decode_dna5_uint8_words_to_ascii_impl(
    const std::uint8_t* HWY_RESTRICT words,
    std::uint8_t* HWY_RESTRICT dst,
    std::size_t total_letters) noexcept;

}  // namespace detail


/**
 * Builds a byte-to-rank table with the selected  unknown-byte policy.
 *
 * Protein input validity comes from aa27 before the selected alphabet's native
 * assignment applies aliases and reductions. Drop/reject use a distinct invalid
 * sentinel; map_to_default uses SeqAn assignment for all byte values.
 */
template <sequence_alphabet p_alphabet,
          unknown_byte_policy p_policy = unknown_byte_policy::drop>
[[nodiscard]] consteval std::array<std::uint8_t, 256>
build_letter_to_rank_lut() {
    std::array<std::uint8_t, 256> lut{};
    static_assert(seqan3::alphabet_size<p_alphabet> < 256u);
    for (std::size_t byte = 0; byte < lut.size(); ++byte) {
        const auto character = static_cast<char>(static_cast<unsigned char>(byte));
        const bool valid = bioseq::sequences::source_character_is_valid<p_alphabet>(character);
        if constexpr (p_policy == unknown_byte_policy::map_to_default)
            lut[byte] = static_cast<std::uint8_t>(seqan3::to_rank(
                seqan3::assign_char_to(character, p_alphabet{})));
        else lut[byte] = valid ? static_cast<std::uint8_t>(seqan3::to_rank(
            seqan3::assign_char_to(character, p_alphabet{}))) : std::uint8_t{0xFF};
    }
    return lut;
}

/**
 * Builds two 16-byte tables indexed by the low five bits of a character.
 *
 * DNA encoders use these tables to look up both ASCII cases with SIMD byte
 * shuffles. Unused entries follow p_policy, and U shares T's entry. This
 * lookup alone cannot validate input because other bytes can  share those bits.
 */
template <sequence_alphabet p_alphabet,
          unknown_byte_policy p_policy>
[[nodiscard]] consteval std::array<std::array<std::uint8_t, 16>, 2>
build_letter_to_rank_lut_32() {
    constexpr auto full = build_letter_to_rank_lut<p_alphabet, p_policy>();
    std::array<std::array<std::uint8_t, 16>, 2> halves{};
    for (auto& half : halves) half.fill(full[0]);
    for (unsigned char character = 'A'; character <= 'Z'; ++character) {
        const std::size_t index = character & 0x1Fu;
        halves[index / 16u][index % 16u] = full[character];
    }
    return halves;
}

/// Maps valid ranks to canonical alphabet characters. Unused table entries are zero.
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

// Scalar packing and decoding helpers, also used by the SIMD codecs.

/**
 * Appends ranks to the current record and writes completed packed words.
 *
 * Ranks equal to 0xFF are skipped. Other ranks must fit p_bits_per_character.
 * The output vector must be attached before calling, and a partial word stays
 * in state for the next chunk or finalize_encode_state().
 */
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

/**
 * Finishes the record by writing any partial word.
 *
 * Returns the number of output words since reset() and the unused letter slots
 * in the last word. Clears the letter count. Call once per record, then reset
 * the state before  starting the next record.
 */
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

/// Maps input bytes to ranks through the full byte lookup table.
/// Under drop, unrecognized bytes become 0xFF entries in dst.
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

/// Converts an input chunk to ranks and appends them to the current record
/// Used when the alphabet has no SIMD encoder.
template <sequence_alphabet p_alphabet,
          unknown_byte_policy p_policy,
          typename p_storage_unit,
          std::size_t p_bits_per_character,
          std::size_t p_letters_per_unit>
[[nodiscard]] inline sequence_encode_result encode_body_chunk_scalar(
    encode_state<p_storage_unit, p_bits_per_character, p_letters_per_unit>& state,
    const std::uint8_t* __restrict__ body_bytes,
    std::size_t body_size) {
    constexpr auto lookup = build_letter_to_rank_lut<p_alphabet, p_policy>();
    const auto initial_count = state.total_letters;
    for (std::size_t cursor = 0; cursor < body_size; ++cursor) {
        const auto rank = lookup[body_bytes[cursor]];
        if (rank == std::uint8_t{0xFF}) {
            if constexpr (p_policy == unknown_byte_policy::reject)
                return {sequence_encode_status::invalid_character, cursor,
                    state.total_letters - initial_count, body_bytes[cursor]};
            continue;
        }
        if (state.total_letters == (std::numeric_limits<std::size_t>::max)())
            return {sequence_encode_status::arithmetic_overflow, cursor,
                state.total_letters - initial_count, body_bytes[cursor]};
        if (state.output_storage->size() == state.output_storage->max_size())
            return {sequence_encode_status::capacity_overflow, cursor,
                state.total_letters - initial_count, body_bytes[cursor]};
        append_ranks<p_storage_unit, p_bits_per_character, p_letters_per_unit>(state, &rank, 1u);
    }
    return {sequence_encode_status::ok, body_size, state.total_letters - initial_count, 0u};
}

/// Decodes total_letters packed ranks to canonical alphabet characters.
/// dst must have room for that many bytes. Padding slots are omitted.
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

/**
 * Decodes packed ranks, inserting a newline between full output lines.
 *
 * wrap_width must be positive, and letters_in_current_line must be smaller
 * than it. dst needs room for the letters and inserted newlines. A  final full
 * llne has no trailing newline, so the return value can equal wrap_width.
 * Callers continuing the output must handle that full line before decoding more.
 */
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

/// Tests for dna4 packed as 32 two-bit ranks per uint64_t word.
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

/// Tests for dna5 packed as two three-bit ranks per uint8_t word.
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

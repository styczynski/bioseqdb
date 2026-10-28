/**
 * @file codec_kernels.cppm
 * @brief SIMD rank lookup, packing, and decoding for dna4 and dna5.
 */
module;

#include <hwy/highway.h>
#include <libassert/assert.hpp>

#include <algorithm>
#include <bit>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <span>
#include <limits>
#include <expected>
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
/** Look up arbitrary byte ranks without indexing a table outside its domain. */
template<class D>
[[nodiscard]] HWY_INLINE hn::Vec<D> lookup_byte_table(
        D tag, hn::Vec<D> bytes, const std::array<std::uint8_t, 256>& table,
        bool letters_and_stop) {
    if (letters_and_stop) {
        const auto ranks = lookup_ranks_via_32byte_lut(tag,
            hn::LoadDup128(tag, table.data() + 64u),
            hn::LoadDup128(tag, table.data() + 80u), bytes);
        const auto upper = hn::And(bytes, hn::Set(tag, std::uint8_t{0xDF}));
        const auto is_letter = hn::And(hn::Gt(upper, hn::Set(tag, std::uint8_t{'A' - 1})),
            hn::Lt(upper, hn::Set(tag, std::uint8_t{'Z' + 1})));
        return hn::IfThenElse(is_letter, ranks,
            hn::IfThenElse(hn::Eq(bytes, hn::Set(tag, std::uint8_t{'*'})),
                hn::Set(tag, table[static_cast<unsigned char>('*')]), hn::Set(tag, table[0])));
    }
    const auto low_bits = hn::And(bytes, hn::Set(tag, std::uint8_t{0x0F}));
    const auto high_bits = hn::And(bytes, hn::Set(tag, std::uint8_t{0xF0}));
    auto ranks = hn::Zero(tag);
    for (std::size_t bank = 0; bank < table.size(); bank += 16u) {
        const auto values = hn::TableLookupBytes(hn::LoadDup128(tag, table.data() + bank), low_bits);
        ranks = hn::IfThenElse(hn::Eq(high_bits, hn::Set(tag, static_cast<std::uint8_t>(bank))),
            values, ranks);
    }
    return ranks;
}

/** Prove that the compact ASCII table is equivalent to the complete native table. */
[[nodiscard]] consteval bool is_letter_and_stop_table(const std::array<std::uint8_t, 256>& table) {
    for (std::size_t byte = 0; byte < table.size(); ++byte) {
        const auto upper = byte >= 'a' && byte <= 'z' ? byte - 'a' + 'A' : byte;
        const auto expected = (upper >= 'A' && upper <= 'Z') || upper == '*'
            ? table[upper] : table[0];
        if (table[byte] != expected) return false;
    }
    return true;
}

/** Bounded packed words produced by one native SIMD rank tile. */
template<class Storage, std::size_t Capacity>
struct packed_native_rank_tile {
    std::array<Storage, Capacity> words{};
    std::size_t word_count{};
};

/** Fuse adjacent native rank bytes into packed pairs before assembling storage words. */
template<class D, class Storage, std::size_t Bits, std::size_t Letters>
[[nodiscard]] HWY_INLINE auto pack_native_rank_tile(D tag, std::span<const std::uint8_t> ranks) {
    packed_native_rank_tile<Storage, (hn::MaxLanes(D{}) + Letters - 1u) / Letters> result{};
    if constexpr (Bits == 2u && Letters >= std::numeric_limits<std::uint32_t>::digits / Bits
        && hn::MaxLanes(D{}) == std::numeric_limits<std::uint32_t>::digits / Bits) {
        std::array<std::uint8_t, hn::MaxLanes(tag)> padded{};
        std::copy(ranks.begin(), ranks.end(), padded.begin());
        result.words[0] = static_cast<Storage>(pack_16_dna4_to_u32(tag, hn::LoadU(tag, padded.data())));
        result.word_count = ranks.empty() ? 0u : 1u;
    } else if constexpr (Bits <= std::numeric_limits<std::uint8_t>::digits && Letters % 2u == 0u
        && hn::MaxLanes(D{}) >= 2u) {
        // Highway's byte-lane bitcast follows native endian order. Correct that
        // once for big-endian targets before packing adjacent logical symbols.
        const hn::Repartition<std::uint16_t, D> pair_tag;
        std::array<std::uint8_t, hn::MaxLanes(tag)> padded{};
        std::copy(ranks.begin(), ranks.end(), padded.begin());
        auto pairs = hn::BitCast(pair_tag, hn::LoadU(tag, padded.data()));
        if constexpr (std::endian::native == std::endian::big)
            pairs = hn::Or(hn::ShiftRight<8>(pairs), hn::ShiftLeft<8>(pairs));
        const auto lower = hn::And(pairs, hn::Set(pair_tag, std::uint16_t{0xFF}));
        const auto upper = hn::ShiftRight<8>(pairs);
        pairs = hn::Or(lower, hn::ShiftLeft<Bits>(upper));
        std::array<std::uint16_t, hn::MaxLanes(pair_tag)> packed_pairs{};
        hn::StoreU(pairs, pair_tag, packed_pairs.data());
        for (std::size_t offset = 0; offset < ranks.size(); offset += Letters) {
            const auto take = std::min(Letters, ranks.size() - offset);
            Storage word{};
            for (std::size_t slot = 0; slot < take; slot += 2u)
                word |= static_cast<Storage>(static_cast<Storage>(packed_pairs[(offset + slot) / 2u])
                    << (slot * Bits));
            result.words[result.word_count++] = word;
        }
    } else {
        for (std::size_t offset = 0; offset < ranks.size(); offset += Letters) {
            const auto take = std::min(Letters, ranks.size() - offset);
            Storage word{};
            for (std::size_t slot = 0; slot < take; ++slot)
                word |= static_cast<Storage>(static_cast<Storage>(ranks[offset + slot]) << (slot * Bits));
            result.words[result.word_count++] = word;
        }
    }
    return result;
}

/** Pack a bounded native-rank tile through the existing checked word sink. */
template<class D, bioseq::sequences::sequence_conversion_sink Writer>
[[nodiscard]] HWY_INLINE bioseq::sequences::rank_pack_status append_native_ranks_chunk(
        D tag, std::span<const std::uint8_t> ranks, Writer& writer) {
    using storage_unit = typename Writer::storage_unit;
    using traits = typename Writer::traits;
    for (const auto rank : ranks)
        if (rank >= traits::alphabet_size) return bioseq::sequences::rank_pack_status::invalid_rank;
    for (std::size_t offset = 0; offset < ranks.size();) {
        const auto tile_count = std::min(hn::Lanes(tag), ranks.size() - offset);
        const auto packed = pack_native_rank_tile<D, storage_unit, traits::bits_per_char,
            traits::letters_per_unit>(tag, ranks.subspan(offset, tile_count));
        std::size_t remaining = tile_count;
        for (std::size_t index = 0; index < packed.word_count; ++index) {
            const auto count = std::min(traits::letters_per_unit, remaining);
            const auto status = writer.append_prepacked_ranks(packed.words[index], count);
            if (status != bioseq::sequences::rank_pack_status::ok) return status;
            remaining -= count;
        }
        offset += tile_count;
    }
    return bioseq::sequences::rank_pack_status::ok;
}

/** Validate, map and pack one native-rank tile; errors retain the first logical position. */
template<class D, bioseq::sequences::sequence_conversion_sink Writer>
[[nodiscard]] HWY_INLINE std::expected<void, bioseq::sequences::sequence_conversion_error>
map_native_rank_chunk(D tag, std::span<const std::uint8_t> source,
        std::span<const std::uint8_t> destinations, std::span<const std::uint8_t> valid,
        Writer& writer, std::size_t logical_offset) {
    using namespace bioseq::sequences;
    std::array<std::uint8_t, hn::MaxLanes(tag)> mapped{};
    for (std::size_t lane = 0; lane < source.size(); ++lane) {
        const auto rank = source[lane];
        if (rank >= destinations.size()) return std::unexpected(sequence_conversion_error{
            sequence_conversion_error_code::invalid_source_rank, logical_offset + lane, {}, rank});
        if (!valid[rank]) return std::unexpected(sequence_conversion_error{
            sequence_conversion_error_code::unrepresentable_symbol, logical_offset + lane, {}, rank});
    }
    if (source.size() == hn::Lanes(tag) && destinations.size() <= 32u) {
        std::array<std::uint8_t, 32> table{};
        std::copy(destinations.begin(), destinations.end(), table.begin());
        const auto ranks = hn::LoadU(tag, source.data());
        hn::StoreU(lookup_ranks_via_32byte_lut(tag,
            hn::LoadDup128(tag, table.data()), hn::LoadDup128(tag, table.data() + 16u), ranks),
            tag, mapped.data());
    } else for (std::size_t lane = 0; lane < source.size(); ++lane)
        mapped[lane] = destinations[source[lane]];
    const auto status = append_native_ranks_chunk(tag,
        std::span<const std::uint8_t>{mapped.data(), source.size()}, writer);
    if (status != rank_pack_status::ok) return std::unexpected(sequence_conversion_error{
        status == rank_pack_status::capacity_overflow ? sequence_conversion_error_code::output_capacity
            : sequence_conversion_error_code::output_contract_violation,
        logical_offset, {}, {}, status});
    return {};
}

/** Append an already validated bounded tile to the shared streaming pack state. */
template<class D, class Storage, std::size_t Bits, std::size_t Letters>
[[nodiscard]] HWY_INLINE sequence_encode_status append_native_rank_state(D tag,
        std::span<const std::uint8_t> ranks, encode_state<Storage, Bits, Letters>& state) {
    if (ranks.size() > (std::numeric_limits<std::size_t>::max)() - state.total_letters)
        return sequence_encode_status::arithmetic_overflow;
    const auto units = (state.partial_letters + ranks.size()) / Letters;
    if (units > state.output_storage->max_size() - state.output_storage->size())
        return sequence_encode_status::capacity_overflow;
    const auto packed = pack_native_rank_tile<D, Storage, Bits, Letters>(tag, ranks);
    auto write_unit = [&state](Storage word) { state.output_storage->push_back(word); return true; };
    std::size_t remaining = ranks.size();
    for (std::size_t index = 0; index < packed.word_count; ++index) {
        const auto take = std::min(Letters, remaining);
        const bool appended = bioseq::sequences::rank_pack_detail::append_prepacked_ranks_lsb<
            Storage, Bits, Letters>(state.partial_word, state.partial_letters, state.total_letters,
                packed.words[index], take, write_unit);
        DEBUG_ASSERT(appended, "validated native tile must fit its output");
        remaining -= take;
    }
    return sequence_encode_status::ok;
}

/** Streaming multiline progress keeps physical line boundaries across input slabs. */
struct sequence_multiline_encode_result {
    sequence_encode_result encoded{};
    bool at_line_start{true};
    bool at_record_boundary{};
    std::size_t newline_bytes{};
};

/** Fused strict FASTA body lookup/packing; only a line-initial '>' ends a record. */
template<sequence_alphabet Alphabet, class Storage, std::size_t Bits, std::size_t Letters>
[[nodiscard]] HWY_INLINE sequence_multiline_encode_result encode_multiline_body_chunk(
        encode_state<Storage, Bits, Letters>& state, const std::uint8_t* input,
        std::size_t length, bool at_line_start) {
    const hn::ScalableTag<std::uint8_t> tag;
    static constexpr auto table = build_letter_to_rank_lut<Alphabet, unknown_byte_policy::reject>();
    constexpr bool compact_lookup = is_letter_and_stop_table(table);
    std::array<std::uint8_t, hn::MaxLanes(tag)> ranks{};
    sequence_multiline_encode_result result{{}, at_line_start, false, 0u};
    const auto initial_count = state.total_letters;
    for (std::size_t cursor = 0; cursor < length;) {
        const auto count = std::min(hn::Lanes(tag), length - cursor);
        if (count == hn::Lanes(tag))
            hn::StoreU(lookup_byte_table(tag, hn::LoadU(tag, input + cursor), table, compact_lookup), tag, ranks.data());
        else for (std::size_t lane = 0; lane < count; ++lane) ranks[lane] = table[input[cursor + lane]];
        std::size_t begin{};
        for (std::size_t lane = 0; lane <= count; ++lane) {
            if (lane < count && ranks[lane] != std::uint8_t{0xFF}) {
                result.at_line_start = false;
                continue;
            }
            const auto status = append_native_rank_state(tag,
                std::span<const std::uint8_t>{ranks.data() + begin, lane - begin}, state);
            if (status != sequence_encode_status::ok) {
                result.encoded = {status, cursor + begin, state.total_letters - initial_count, input[cursor + begin]};
                return result;
            }
            if (lane == count) break;
            const auto byte = input[cursor + lane];
            if (byte == '\n' || byte == '\r') {
                result.at_line_start = true;
                ++result.newline_bytes;
            } else if (byte == '>' && result.at_line_start) {
                result.at_record_boundary = true;
                result.encoded = {sequence_encode_status::ok, cursor + lane, state.total_letters - initial_count, 0u};
                return result;
            } else {
                result.encoded = {sequence_encode_status::invalid_character, cursor + lane,
                    state.total_letters - initial_count, byte};
                return result;
            }
            begin = lane + 1u;
        }
        cursor += count;
    }
    result.encoded = {sequence_encode_status::ok, length, state.total_letters - initial_count, 0u};
    return result;
}

/** Fused native byte lookup and bounded word packing for every registered alphabet. */
template<class D, sequence_alphabet Alphabet, unknown_byte_policy Policy,
         class Storage, std::size_t Bits, std::size_t Letters>
[[nodiscard]] HWY_INLINE sequence_encode_result encode_native_chunk(
        D tag, encode_state<Storage, Bits, Letters>& state,
        const std::uint8_t* HWY_RESTRICT input, std::size_t length) {
    static constexpr auto table = build_letter_to_rank_lut<Alphabet, Policy>();
    constexpr bool compact_lookup = is_letter_and_stop_table(table);
    const auto lanes = hn::Lanes(tag);
    std::array<std::uint8_t, hn::MaxLanes(tag)> ranks{};
    const std::size_t original_count = state.total_letters;
    std::size_t cursor{};
    auto write_unit = [&state](Storage word) {
        if (state.output_storage->size() == state.output_storage->max_size()) return false;
        state.output_storage->push_back(word);
        return true;
    };
    while (cursor < length) {
        const auto count = std::min(lanes, length - cursor);
        if (count == lanes)
            hn::StoreU(lookup_byte_table(tag, hn::LoadU(tag, input + cursor), table, compact_lookup),
                tag, ranks.data());
        else for (std::size_t lane = 0; lane < count; ++lane) ranks[lane] = table[input[cursor + lane]];
        const bool all_valid = std::find(ranks.begin(), ranks.begin() + count,
            std::uint8_t{0xFF}) == ranks.begin() + count;
        if (all_valid) {
            const auto status = append_native_rank_state(tag,
                std::span<const std::uint8_t>{ranks.data(), count}, state);
            if (status != sequence_encode_status::ok)
                return {status, cursor, state.total_letters - original_count, input[cursor]};
            cursor += count;
            continue;
        }
        std::size_t lane{};
        while (lane < count) {
            Storage packed_word{};
            std::size_t packed_count{};
            const std::size_t group_begin = lane;
            while (lane < count && packed_count < Letters) {
                const auto rank = ranks[lane];
                if (rank == std::uint8_t{0xFF}) {
                    if constexpr (Policy == unknown_byte_policy::reject) break;
                    ++lane;
                    continue;
                }
                packed_word |= static_cast<Storage>(static_cast<Storage>(rank) << (packed_count * Bits));
                ++packed_count;
                ++lane;
            }
            if (packed_count > (std::numeric_limits<std::size_t>::max)() - state.total_letters)
                return {sequence_encode_status::arithmetic_overflow, cursor + group_begin,
                    state.total_letters - original_count, input[cursor + group_begin]};
            const auto required_units = (state.partial_letters + packed_count) / Letters;
            if (required_units > state.output_storage->max_size() - state.output_storage->size())
                return {sequence_encode_status::capacity_overflow, cursor + group_begin,
                    state.total_letters - original_count, input[cursor + group_begin]};
            if (packed_count != 0u) {
                const bool appended = bioseq::sequences::rank_pack_detail::append_prepacked_ranks_lsb<
                    Storage, Bits, Letters>(state.partial_word, state.partial_letters,
                        state.total_letters, packed_word, packed_count, write_unit);
                DEBUG_ASSERT(appended, "validated native packing must fit its growing sink");
            }
            if constexpr (Policy == unknown_byte_policy::reject) {
                if (lane < count && ranks[lane] == std::uint8_t{0xFF})
                    return {sequence_encode_status::invalid_character, cursor + lane,
                        state.total_letters - original_count, input[cursor + lane]};
            }
        }
        cursor += count;
    }
    return {sequence_encode_status::ok, length, state.total_letters - original_count, 0u};
}

// Codec selection by alphabet, and packed layout.

/**
 * Appends a chunk using the codec for its alphabet and storage layout.
 *
 * Native SeqAn lookup, invalid-byte checks and packed-word emission share one
 * bounded SIMD traversal. State retains unfinished words across input chunks.
 */
template <sequence_alphabet p_alphabet,
          unknown_byte_policy p_policy = unknown_byte_policy::drop,
          typename p_storage_unit,
          std::size_t p_bits_per_character,
          std::size_t p_letters_per_unit>
[[nodiscard]] HWY_INLINE sequence_encode_result encode_body_chunk(
    encode_state<p_storage_unit, p_bits_per_character, p_letters_per_unit>& state,
    const std::uint8_t* HWY_RESTRICT body_bytes,
    std::size_t body_size) {
    return encode_native_chunk<hn::ScalableTag<std::uint8_t>, p_alphabet, p_policy>(
        hn::ScalableTag<std::uint8_t>{}, state, body_bytes, body_size);
}

/** Extract and decode native packed ranks in bounded SIMD tiles. */
template<class D, sequence_alphabet Alphabet, class Storage,
         std::size_t Bits, std::size_t Letters>
HWY_INLINE void decode_native_words(D tag, const Storage* HWY_RESTRICT words,
        std::uint8_t* HWY_RESTRICT output, std::size_t count, std::size_t source_begin = 0u) {
    static constexpr auto table = build_rank_to_letter_lut<Alphabet>();
    constexpr Storage mask = static_cast<Storage>(seqan3::alphabet_size<Alphabet> == 1u
        ? 0u : (Storage{1u} << Bits) - 1u);
    std::array<std::uint8_t, hn::MaxLanes(tag)> ranks{};
    const auto lanes = hn::Lanes(tag);
    for (std::size_t offset = 0; offset < count;) {
        const auto take = std::min(lanes, count - offset);
        for (std::size_t lane = 0; lane < take; ++lane) {
            const auto position = source_begin + offset + lane;
            ranks[lane] = static_cast<std::uint8_t>((words[position / Letters]
                >> ((position % Letters) * Bits)) & mask);
        }
        if (take == lanes) {
            const auto native = hn::LoadU(tag, ranks.data());
            const auto characters = lookup_ranks_via_32byte_lut(tag,
                hn::LoadDup128(tag, table.data()), hn::LoadDup128(tag, table.data() + 16u), native);
            if constexpr (seqan3::alphabet_size<Alphabet> <= 32u)
                hn::StoreU(characters, tag, output + offset);
            else hn::StoreU(lookup_byte_table(tag, native, table, false), tag, output + offset);
        } else for (std::size_t lane = 0; lane < take; ++lane) output[offset + lane] = table[ranks[lane]];
        offset += take;
    }
}

/**
 * Decodes packed ranks to ASCII using the matching storage layout.
 *
 * Existing DNA geometry specializations remain fast paths. Every other alphabet
 * uses bounded native extraction and the shared multi-bank SIMD character LUT.
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
        decode_native_words<hn::ScalableTag<std::uint8_t>, p_alphabet, p_storage_unit,
            p_bits_per_character, p_letters_per_unit>(hn::ScalableTag<std::uint8_t>{}, words, dst, total_letters);
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
        const hn::ScalableTag<std::uint8_t> tag;
        std::array<std::uint8_t, hn::MaxLanes(tag)> decoded{};
        std::size_t line_position = letters_in_current_line;
        std::size_t output_offset{};
        for (std::size_t offset = 0; offset < total_letters;) {
            const auto take = std::min(hn::Lanes(tag), total_letters - offset);
            decode_native_words<decltype(tag), p_alphabet, p_storage_unit,
                p_bits_per_character, p_letters_per_unit>(tag, words, decoded.data(), take, offset);
            for (std::size_t lane = 0; lane < take; ++lane) {
                if (line_position == wrap_width) { dst[output_offset++] = '\n'; line_position = 0u; }
                dst[output_offset++] = decoded[lane];
                ++line_position;
            }
            offset += take;
        }
        return line_position;
    }
}


}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

#undef BIOSEQ_SIMD_X86_PRE_AVX3_FIXED128

}  // export

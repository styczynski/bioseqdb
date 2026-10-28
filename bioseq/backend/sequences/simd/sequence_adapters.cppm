/**
 * @file sequence_adapters.cppm
 * @brief Encodes and decodes complete compact sequences.
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
#include <expected>
#include <span>

#include <boost/integer.hpp>

export module bioseq.sequences.simd:sequence_adapters;

export import :codec_kernels;
import bioseq.sequences;
import bioseq.storage.lazy_sequence;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {

/** Recode a checked borrowed native source without expanding the complete input. */
template<sequence_alphabet Source, bioseq::sequences::sequence_conversion_sink Writer>
[[nodiscard]] inline std::expected<bioseq::sequences::rank_pack_result,
    bioseq::sequences::sequence_conversion_error> recode_into(
        const bioseq::storage::borrowed_rank_window_source& source,
        std::size_t begin, std::size_t end, Writer& writer) {
    using namespace bioseq::sequences;
    using Destination = typename Writer::traits::alphabet_type;
    if constexpr (!((seqan3::nucleotide_alphabet<Source> && seqan3::nucleotide_alphabet<Destination>)
        || (seqan3::aminoacid_alphabet<Source> && seqan3::aminoacid_alphabet<Destination>))) {
        return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::incompatible_alphabets});
    } else {
        if (begin > end || end > source.symbol_count)
            return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::invalid_interval, begin});
        if (source.alphabet_code != supported_biological_alphabets::template index_of<Source>())
            return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::incompatible_alphabets});
        static constexpr auto table = make_native_rank_recode_table<Source, Destination>();
        const hn::ScalableTag<std::uint8_t> tag;
        std::array<std::uint8_t, hn::MaxLanes(tag)> scratch{};
        for (std::size_t offset = begin; offset < end;) {
            const auto take = std::min(hn::Lanes(tag), end - offset);
            const auto read = source.read_checked(offset, take, scratch);
            if (!read) return std::unexpected(sequence_conversion_error{
                sequence_conversion_error_code::source_read_failed, offset});
            const auto mapped = map_native_rank_chunk(tag, *read,
                std::span<const std::uint8_t>{table.destination_ranks},
                std::span<const std::uint8_t>{table.valid}, writer, offset);
            if (!mapped) return std::unexpected(mapped.error());
            offset += take;
        }
        const auto count = end - begin;
        const auto units = count / Writer::traits::letters_per_unit
            + static_cast<std::size_t>(count % Writer::traits::letters_per_unit != 0u);
        const auto packed = writer.finish(count, units);
        if (!packed) return std::unexpected(sequence_conversion_error{
            packed.status == rank_pack_status::capacity_overflow
                ? sequence_conversion_error_code::output_capacity
                : sequence_conversion_error_code::output_contract_violation,
            {}, {}, {}, packed.status});
        return packed;
    }
}

/** Adapt compact packed ranges to the shared checked native recoder. */
template<supports_compact_sequence_range R, bioseq::sequences::sequence_conversion_sink Writer>
[[nodiscard]] inline auto recode_into(const compact_sequence_range<R>& source,
        std::size_t begin, std::size_t end, Writer& writer) {
    using Source = typename compact_sequence_range<R>::traits::alphabet_type;
    return recode_into<Source>(bioseq::storage::make_borrowed_rank_source(source), begin, end, writer);
}

// Whole-sequence adapters for the streaming codecs.

/**
 * Encodes text into an owned packed sequence with the scalar codec.
 *
 * Native SeqAn assignment supplies defaults and aliases. Reject reports the
 * first invalid lexical byte; drop omits invalid bytes.
 * @param input Input character buffer.
 * @param length Number of bytes to read.
 * @return Packed sequence with its final padding recorded.
 */
template <class p_destination_configuration,
          unknown_byte_policy p_policy = unknown_byte_policy::map_to_default>
requires is_sequence_custom_alphabet<p_destination_configuration>
[[nodiscard]] std::expected<compact_sequence_range<p_destination_configuration>, sequence_encode_result>
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
    const auto units = length / k_letters_per_unit
        + static_cast<std::size_t>(length % k_letters_per_unit != 0u);
    if (units > packed.max_size()) return std::unexpected(sequence_encode_result{
        sequence_encode_status::capacity_overflow});
    packed.reserve(units);

    encode_state<
        storage_unit_type,
        k_bits_per_character,
        k_letters_per_unit> state{};
    state.output_storage = &packed;
    state.reset();
    const auto encoded = encode_body_chunk_scalar<
        alphabet_type,
        p_policy,
        storage_unit_type,
        k_bits_per_character,
        k_letters_per_unit>(
        state,
        reinterpret_cast<const std::uint8_t*>(input),
        length);
    if (!encoded) return std::unexpected(encoded);
    const auto [word_count, padding] =
        finalize_encode_state<
            storage_unit_type,
            k_bits_per_character,
            k_letters_per_unit>(state);
    (void)word_count;
    return sequence_type::from_packed(std::move(packed), padding);
}

/**
 * Encodes text into  an owned packed sequence using the selected codec.
 *
 * All alphabets use the shared bounded native lookup/packing core, including
 * strict invalid-byte detection before any rejected byte is emitted.
 * @param input Input character buffer
 * @param length Number of bytes to read.
 * @return Packed sequence with its final padding recorded.
 */
template <class p_destination_configuration,
          unknown_byte_policy p_policy = unknown_byte_policy::map_to_default>
requires is_sequence_custom_alphabet<p_destination_configuration>
[[nodiscard]] std::expected<compact_sequence_range<p_destination_configuration>, sequence_encode_result>
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
    const auto units = length / k_letters_per_unit
        + static_cast<std::size_t>(length % k_letters_per_unit != 0u);
    if (units > packed.max_size()) return std::unexpected(sequence_encode_result{
        sequence_encode_status::capacity_overflow});
    packed.reserve(units);

    encode_state<
        storage_unit_type,
        k_bits_per_character,
        k_letters_per_unit> state{};
    state.output_storage = &packed;
    state.reset();
    const auto encoded = encode_body_chunk<
        alphabet_type,
        p_policy,
        storage_unit_type,
        k_bits_per_character,
        k_letters_per_unit>(
        state,
        reinterpret_cast<const std::uint8_t*>(input),
        length);
    if (!encoded) return std::unexpected(encoded);
    const auto [word_count, padding] =
        finalize_encode_state<
            storage_unit_type,
            k_bits_per_character,
            k_letters_per_unit>(state);
    (void)word_count;
    return sequence_type::from_packed(std::move(packed), padding);
}

/**
 * Decodes a compact sequence to a string with the, scalar codec.
 *
 * Custom-alphabet and standard-layout instances expose their packed words
 * directly. Other sequence configurations are read through their rank iterator.
 * The returned  string contains only logical letters, without padding.
 */
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

/**
 * Decodes a compact sequence to a string using the selected codec.
 *
 * The SIMD decoder supports dna4 in uint64_t words and dna5 with two ranks
 * per byte. Other packed layouts use scalar decoding. Configurations without
 * direct packed storage are read through their rank iterator.
 */
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


}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

}  // export

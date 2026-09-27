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

#include <boost/integer.hpp>

export module bioseq.sequences.simd:sequence_adapters;

export import :codec_kernels;
import bioseq.sequences;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {

// Whole-sequence adapters for the streaming codecs.

/**
 * Encodes text into an owned packed sequence with the scalar codec.
 *
 * The default policy maps unrecognized bytes to N for dna5 and rank zero
 * for other alphabets. The drop policy omits them.
 * @param input Input character buffer.
 * @param length Number of bytes to read.
 * @return Packed sequence with its final padding recorded.
 */
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

/**
 * Encodes text into an owned packed sequence using the selected codec.
 *
 * dna4 and dna5 use SIMD lookup, which assumes DNA text and can mistake some
 * invalid bytes for letters. Other alphabets use the scalar codec. Unused lookup
 * entries default to N for dna5 and rank zero for other alphabets.
 * @param input Input character buffer.
 * @param length Number of bytes to read.
 * @return Packed sequence with its final padding recorded.
 */
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

/**
 * Decodes a compact sequence to a string with the scalar codec.
 *
 * Custom-alphabet and standard-layout instances expose their packed words
 * directly. Other sequence configurations are read through their rank iterator.
 * The returned string contains only logical letters, without padding.
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

/**
 * @file sequence_adapters.cppm
 * @brief One-shot sequence encode and decode adapters.
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
/// `(dna5, uint8, 3, 2)`. Everything else uses the scalar bit-walk.
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


}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

}  // export

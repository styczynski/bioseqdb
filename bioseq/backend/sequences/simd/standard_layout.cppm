/**
 * @file standard_layout.cppm
 * @brief Converts and compares standard-layout compact sequences.
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
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <functional>

#include <boost/integer.hpp>

export module bioseq.sequences.simd:standard_layout;

export import :sequence_adapters;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.storage.lazy_sequence;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {

// Comparing packed views with matching alphabets and storage-unit types.

/**
 * Compares two packed views in alphabet-rank order.
 *
 * Skips equal packed words, then checks ranks in the first unequal word.
 * Equal prefixes are ordered by sequence length. Both views must use the same
 * alphabt, storage  type, and packing layout. Padding does not affect the result.
 */
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

	// An empty common prefix leaves only the lengths to compare.
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

// These adapters share the codec target namespace so their templates can
// call its SIMD helpers. active_target names the selected namespace.

/**
 * @brief Encodes positional text into a rank-packed standard-layout sequence.
 *
 * Uses a temporary packed vector, then copies its words after the new header.
 * The requested storage width must be enabled for the header type
 * @param target_storage_byte_width Destination word width in bytes.
 * @param length Number of input characters.
 * @param input Character buffer.
 * @param allocator Allocates the header and packed payload from a byte count.
 * @return New  sequence, or nullptr if the destination allocator returns null.
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
    && compact_sequence_range<p_alphabet>::traits::is_bit_packed
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
            // Use fixed-width integer types accepted by Highway. A least-width alias
            // can name a different C++ type even when it has the same width.
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
            header->encoding = bioseq::sequence_encoding::rank_packed;
            header->space = bioseq::cigar_space::compatibility;
            header->alignment_padding = {};

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
 * @brief Converts a standard-layout sequence to another supported alphabet.
 *
 * Rank-packed alphabets decode to text and re-encode with p_target_alphabet;
 * unrecognized target characters use its default rank. Counted-run values
 * support only a same-alphabet codec-preserving copy at their native width.
 * @param source Input sequence.
 * @param target_byte_width Destination wrd width in bytes.
 * @param allocator Allocates the resulting header and payload.
 * @return New sequence, or nullptr for incompatible codecs or a null allocation.
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
    if (!p_sequence_header::supported_storage_unit_byte_widths_type::contains_runtime(
            target_byte_width)) {
        return nullptr;
    }
    using target_traits = typename compact_sequence_range<p_target_alphabet>::traits;
    if constexpr (!target_traits::is_bit_packed) {
        if (source.alphabet_code != p_sequence_header::supported_alphabets_type::template
                index_of<p_target_alphabet>()
            || source.encoding != bioseq::sequence_encoding::cigar_rle
            || !bioseq::valid_cigar_space(source.space)
            || source.storage_unit_byte_width != sizeof(typename target_traits::storage_unit)
            || target_byte_width != source.storage_unit_byte_width) {
            return nullptr;
        }
        return source.recast_storage_width(
            target_byte_width, std::forward<p_allocator>(allocator));
    } else {
        if (source.encoding != bioseq::sequence_encoding::rank_packed
            || source.space != bioseq::cigar_space::compatibility) {
            return nullptr;
        }
        std::string ascii;
        bool decoded = false;
        p_sequence_header::dispatch_instance(
            source.alphabet_code, source.storage_unit_byte_width,
            [&source, &ascii, &decoded]<class p_instance>() {
                if constexpr (p_instance::is_bit_packed) {
                    const compact_sequence_range<p_instance> view{source};
                    ascii = decode_sequence_simd<p_instance>(view);
                    decoded = true;
                }
            });
        if (!decoded) return nullptr;
        return stdc_convert_from_cstr<p_sequence_header, p_target_alphabet>(
            target_byte_width,
            static_cast<typename p_sequence_header::bp_size_t>(ascii.size()),
            ascii.data(),
            std::forward<p_allocator>(allocator));
    }
}

/**
 * @brief Changes packed word width while preserving alphabet and codec metadata.
 *
 * Copies bytes directly  when letters pack identically across widths. Other
 * alphabets are decoded and encoded again at the new width. Counted-run values
 * retain their encoded payload and support only their native record width.
 * @param source Input sequence.
 * @param target Required alphabet code and new word width.
 * @param allocator Allocates the resulting header and payload.
 * @return New sequence, or nullptr for incompatible metadata or null allocation.
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
    // Width conversion requires the source alphabet, including for callers
    // outside SQL that have not checked the requested type.
    if (target.alphabet_code != source.alphabet_code
        || target.encoding != source.encoding
        || target.space != source.space
        || !p_sequence_header::supported_storage_unit_byte_widths_type::contains_runtime(
            target.storage_unit_byte_width)) {
        return nullptr;
    }
    p_sequence_header* result = nullptr;
    p_sequence_header::supported_alphabets_type::dispatch_alphabet_by_code(
        source.alphabet_code,
        [&result, &source, target, &allocator]<class p_source_alphabet>() {
            using source_traits = typename compact_sequence_range<p_source_alphabet>::traits;
            // A byte copy preserves ranks when the letter width divides a
            // byte. Other packed alphabets must decode and pack again.
            constexpr std::size_t letter_bit_width =
                seqan3::detail::ceil_log2(seqan3::alphabet_size<p_source_alphabet>);
            if constexpr (source_traits::is_bit_packed && letter_bit_width != 0u
                && std::numeric_limits<std::uint8_t>::digits % letter_bit_width == 0u) {
                if (source.encoding != bioseq::sequence_encoding::rank_packed
                    || source.space != bioseq::cigar_space::compatibility) {
                    return;
                }
                result = source.recast_storage_width(
                    target.storage_unit_byte_width,
                    std::forward<p_allocator>(allocator));
            } else {
                result = stdc_recode_to<
                    p_sequence_header, p_source_alphabet>(
                    source,
                    target.storage_unit_byte_width,
                    std::forward<p_allocator>(allocator));
            }
        });
    return result;
}


}
HWY_AFTER_NAMESPACE();

namespace bioseq::simd {
/// Highway target namespace chosen when this modul is built.
namespace active_target = ::bioseq::simd::HWY_NAMESPACE;

/**
 * Changes packed word width through the selected Highway  target.
 *
 * The alphabet code must match the source. The allocator receives the required
 * byte count and returns the new header. A mismatch or null allocation returns
 * nullptr. See active_target::stdc_rewidth_to_type_params for packing rules.
 */
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

}  // export

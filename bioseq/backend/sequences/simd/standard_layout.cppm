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
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <functional>
#include <expected>
#include <span>

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
inline std::expected<p_sequence_header*, sequence_encode_result> stdc_convert_from_cstr(
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

    std::expected<p_sequence_header*, sequence_encode_result> result{nullptr};
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
                encode_sequence_simd<sequence_configuration, unknown_byte_policy::reject>(
                    input, static_cast<std::size_t>(length));
            if (!packed) { result = std::unexpected(packed.error()); return; }
            const std::size_t unit_count = packed->size();
            if (unit_count > ((std::numeric_limits<std::size_t>::max)()
                    - p_sequence_header::header_bytes() - sizeof(std::uint64_t)) / p_storage_byte_width) {
                result = std::unexpected(sequence_encode_result{sequence_encode_status::arithmetic_overflow});
                return;
            }
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
                static_cast<std::uint16_t>(packed->padding());
            header->bp_size =
                static_cast<typename p_sequence_header::bp_size_t>(
                    packed->bp_size());
            header->encoding = bioseq::sequence_encoding::rank_packed;
            header->space = bioseq::cigar_space::compatibility;
            header->alignment_padding = {};

            if (unit_count > 0) {
                std::memmove(
                    header->data,
                    packed->packed_units.data(),
                    payload_bytes);
            }
            result = header;
        });
    return result;
}

/**
 * @brief Converts a standard-layout sequence to another supported alphabet.
 *
 * Rank-packed alphabets map native ranks directly into checked destination words;
 * unrepresentable symbols reject the conversion. Counted-run values
 * support only a same-alphabet codec-preserving copy at their native width.
 * @param source Input sequence.
 * @param target_byte_width Destination wrd width in bytes.
 * @param allocator Allocates the resulting header and payload.
 * @param deallocator Reclaims an unpublished allocation; must not throw.
 * @return Owned result pointer, or a typed conversion/allocation failure.
 */
template<class p_sequence_header, sequence_alphabet p_target_alphabet,
         class p_allocator, class p_deallocator>
requires (is_compact_sequence_stdc<p_sequence_header>
    && alphabet_in_supported_set<typename p_sequence_header::supported_alphabets_type, p_target_alphabet>
    && stdc_allocator<p_allocator, p_sequence_header>
    && std::is_nothrow_invocable_v<p_deallocator&, p_sequence_header*>)
[[nodiscard]] inline std::expected<p_sequence_header*, bioseq::sequences::sequence_conversion_error>
stdc_recode_to(const p_sequence_header& source, std::uint8_t target_byte_width,
        p_allocator&& allocator, p_deallocator&& deallocator) {
    using namespace bioseq::sequences;
    if (!p_sequence_header::supported_storage_unit_byte_widths_type::contains_runtime(target_byte_width)
        || !p_sequence_header::supported_storage_unit_byte_widths_type::contains_runtime(source.storage_unit_byte_width)
        || source.alphabet_code >= p_sequence_header::supported_alphabets_type::alphabet_count)
        return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::incompatible_alphabets});
    using target_traits = typename compact_sequence_range<p_target_alphabet>::traits;
    if constexpr (!target_traits::is_bit_packed) {
        if (source.alphabet_code != p_sequence_header::supported_alphabets_type::template index_of<p_target_alphabet>()
            || source.encoding != bioseq::sequence_encoding::cigar_rle
            || !bioseq::valid_cigar_space(source.space)
            || source.storage_unit_byte_width != sizeof(typename target_traits::storage_unit)
            || target_byte_width != source.storage_unit_byte_width)
            return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::incompatible_alphabets});
        auto* copied = source.recast_storage_width(target_byte_width, std::forward<p_allocator>(allocator));
        if (copied == nullptr)
            return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::allocation_failure});
        return copied;
    } else {
        if (source.encoding != bioseq::sequence_encoding::rank_packed
            || source.space != bioseq::cigar_space::compatibility)
            return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::incompatible_alphabets});
        std::expected<p_sequence_header*, sequence_conversion_error> result = std::unexpected(
            sequence_conversion_error{sequence_conversion_error_code::incompatible_alphabets});
        p_sequence_header::supported_storage_unit_byte_widths_type::dispatch(target_byte_width,
            [&source, &result, &allocator, &deallocator]<std::size_t Width>() {
                using Storage = std::conditional_t<Width == 8u, std::uint64_t,
                    std::conditional_t<Width == 4u, std::uint32_t,
                    std::conditional_t<Width == 2u, std::uint16_t, std::uint8_t>>>;
                using Configuration = sequence_custom_alphabet<p_target_alphabet, Storage>;
                using Traits = typename compact_sequence_range<Configuration>::traits;
                const std::size_t count = source.bp_size;
                const std::size_t units = count / Traits::letters_per_unit
                    + static_cast<std::size_t>(count % Traits::letters_per_unit != 0u);
                if (units > ((std::numeric_limits<std::size_t>::max)()
                        - p_sequence_header::header_bytes() - sizeof(std::uint64_t)) / Width) {
                    result = std::unexpected(sequence_conversion_error{sequence_conversion_error_code::size_overflow});
                    return;
                }
                const std::size_t bytes = p_sequence_header::header_bytes() + round_up_to_uint64(units * Width);
                // The allocation-only stdc_allocator contract cannot reclaim a failed
                // conversion. Retain ownership at this cold boundary until publication.
                const auto release = [&deallocator](p_sequence_header* header) noexcept { deallocator(header); };
                std::unique_ptr<p_sequence_header, decltype(release)> owned{allocator(bytes), release};
                if (!owned) {
                    result = std::unexpected(sequence_conversion_error{sequence_conversion_error_code::allocation_failure});
                    return;
                }
                using UnitSink = fixed_rank_pack_unit_sink<Storage>;
                checked_rank_pack_sink<Configuration, invalid_rank_policy::reject,
                    pack_capacity_policy::fixed_span, UnitSink> writer{
                        UnitSink{std::span<Storage>{reinterpret_cast<Storage*>(owned->data), units}}};
                static_assert(sequence_conversion_sink<decltype(writer)>);
                std::expected<rank_pack_result, sequence_conversion_error> converted = std::unexpected(
                    sequence_conversion_error{sequence_conversion_error_code::incompatible_alphabets});
                p_sequence_header::dispatch_instance(source.alphabet_code, source.storage_unit_byte_width,
                    [&source, &writer, &converted]<class Instance>() {
                        using Source = typename Instance::alphabet_type;
                        if constexpr (Instance::is_bit_packed
                            && ((seqan3::nucleotide_alphabet<Source> && seqan3::nucleotide_alphabet<p_target_alphabet>)
                                || (seqan3::aminoacid_alphabet<Source> && seqan3::aminoacid_alphabet<p_target_alphabet>))) {
                            const compact_sequence_range<Instance> view{source};
                            converted = recode_into(view, 0u, view.bp_size(), writer);
                        }
                    });
                if (!converted) { result = std::unexpected(converted.error()); return; }
                owned->vl_len_ = p_sequence_header::encode_varlena_header(bytes);
                owned->alphabet_code = p_sequence_header::supported_alphabets_type::template index_of<p_target_alphabet>();
                owned->storage_unit_byte_width = Width;
                owned->padding_letters = converted->padding_letters;
                owned->bp_size = source.bp_size;
                owned->encoding = bioseq::sequence_encoding::rank_packed;
                owned->space = bioseq::cigar_space::compatibility;
                owned->alignment_padding = {};
                result = owned.release();
            });
        return result;
    }
}

/**
 * @brief Changes packed word width while preserving alphabet and codec metadata.
 *
 * Copies bytes directly  when letters pack identically across widths. Other
 * alphabets are repacked directly from native ranks at the new width. Counted-run values
 * retain their encoded payload and support only their native record width.
 * @param source Input sequence.
 * @param target Required alphabet code and new word width.
 * @param allocator Allocates the resulting header and payload.
 * @param deallocator Reclaims an unpublished allocation; must not throw.
 * @return Owned result pointer, or a typed metadata/conversion/allocation failure.
 */
template<class p_sequence_header, class p_allocator, class p_deallocator>
requires (is_compact_sequence_stdc<p_sequence_header>
    && stdc_allocator<p_allocator, p_sequence_header>
    && std::is_nothrow_invocable_v<p_deallocator&, p_sequence_header*>)
[[nodiscard]] inline std::expected<p_sequence_header*, bioseq::sequences::sequence_conversion_error>
stdc_rewidth_to_type_params(const p_sequence_header& source, compact_sequence_stdc_type_params target,
        p_allocator&& allocator, p_deallocator&& deallocator) {
    using namespace bioseq::sequences;
    if (target.alphabet_code != source.alphabet_code || target.encoding != source.encoding
        || target.space != source.space
        || source.alphabet_code >= p_sequence_header::supported_alphabets_type::alphabet_count
        || !p_sequence_header::supported_storage_unit_byte_widths_type::contains_runtime(source.storage_unit_byte_width)
        || !p_sequence_header::supported_storage_unit_byte_widths_type::contains_runtime(target.storage_unit_byte_width))
        return std::unexpected(sequence_conversion_error{sequence_conversion_error_code::incompatible_alphabets});
    std::expected<p_sequence_header*, sequence_conversion_error> result = std::unexpected(
        sequence_conversion_error{sequence_conversion_error_code::incompatible_alphabets});
    p_sequence_header::supported_alphabets_type::dispatch_alphabet_by_code(source.alphabet_code,
        [&result, &source, target, &allocator, &deallocator]<class Source>() {
            using traits = typename compact_sequence_range<Source>::traits;
            constexpr auto bits = seqan3::detail::ceil_log2(seqan3::alphabet_size<Source>);
            if constexpr (traits::is_bit_packed && bits != 0u
                && std::numeric_limits<std::uint8_t>::digits % bits == 0u) {
                if (source.encoding != bioseq::sequence_encoding::rank_packed
                    || source.space != bioseq::cigar_space::compatibility) return;
                auto* copied = source.recast_storage_width(target.storage_unit_byte_width,
                    std::forward<p_allocator>(allocator));
                if (copied == nullptr) result = std::unexpected(
                    sequence_conversion_error{sequence_conversion_error_code::allocation_failure});
                else result = copied;
            } else {
                result = stdc_recode_to<p_sequence_header, Source>(source, target.storage_unit_byte_width,
                    std::forward<p_allocator>(allocator), std::forward<p_deallocator>(deallocator));
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
 * byte count and returns the new header; the deallocator reclaims unpublished
 * output on failure. Typed errors distinguish allocation from input rejection.
 */
template<class p_sequence_header, class p_allocator, class p_deallocator>
requires (is_compact_sequence_stdc<p_sequence_header>
    && stdc_allocator<p_allocator, p_sequence_header>
    && std::is_nothrow_invocable_v<p_deallocator&, p_sequence_header*>)
[[nodiscard]] inline std::expected<p_sequence_header*, bioseq::sequences::sequence_conversion_error>
stdc_rewidth_to_type_params(const p_sequence_header& source, compact_sequence_stdc_type_params target,
        p_allocator&& allocator, p_deallocator&& deallocator) {
    return active_target::stdc_rewidth_to_type_params(source, target,
        std::forward<p_allocator>(allocator), std::forward<p_deallocator>(deallocator));
}

}

}  // export

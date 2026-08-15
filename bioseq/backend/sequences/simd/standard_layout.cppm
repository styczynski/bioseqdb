/**
 * @file standard_layout.cppm
 * @brief Standard-layout conversion, rewidth, and comparison operations.
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

export module bioseq.sequences.simd:standard_layout;

export import :sequence_adapters;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.storage.lazy_sequence;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {

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

}  // export

/**
 * @file packed_sequence.cppm
 * @brief Packed sequence storage, borrowed views, and symbol iteration.
 *
 * Exports alphabet settings, packing helpers, range adapters, and compact headers.
 * Packed words store the first symbol in their low bits. Compatible word-width
 * views keep the symbol count and, recalculate padding for the requested width.
 */
module;

#include <libassert/assert.hpp>

#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iterator>
#include <list>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <valarray>
#include <vector>
#include <algorithm>
#include <limits>

#include <boost/integer.hpp>
#include <boost/iterator/iterator_adaptor.hpp>

export module bioseq.sequences:packed_sequence;

import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.common.templating;
import bioseq.sequences.rank_pack_core;

/** Alphabet types, packed-word settings, and runtime type selection. */
export import :packed_sequence_core;
/** Rank extraction and views using a different  word width. */
export import :packed_sequence_packing;
/** Packed and unpacked sequence range implementations. */
export import :packed_sequence_range;
/** Compact headers, inline payloads, and allocation helpers. */
export import :packed_sequence_wire;

export {

/**
 * @brief Compares symbol sequences stored in different range configurations.
 * @param lhs First range.
 * @param rhs Second range.
 * @return True when both decoded symbol sequences are equal.
 */
template<
    typename p_left_configuration,
    typename p_right_configuration,
    std::enable_if_t<
        !std::is_same_v<p_left_configuration, p_right_configuration>, bool>
        p_configurations_are_distinct = true>
requires (
    requires (
        compact_sequence_range<p_left_configuration>& lhs,
        compact_sequence_range<p_right_configuration>& rhs) {
        {
            *(lhs.template iter<alphabet_symbol>(lhs.begin()))
                == *(rhs.template iter<alphabet_symbol>(rhs.begin()))
        };
    }
)
inline constexpr bool operator==(
    compact_sequence_range<p_left_configuration>& lhs,
    compact_sequence_range<p_right_configuration>& rhs
) {
    return std::equal(
        lhs.template iter<alphabet_symbol>(lhs.begin()),
        lhs.template iter<alphabet_symbol>(lhs.end()),
        rhs.template iter<alphabet_symbol>(rhs.begin()),
        rhs.template iter<alphabet_symbol>(rhs.end())
    );
}

static_assert(std::is_standard_layout_v<compact_sequence_range<seq::dna4>>);

/** Identifies wnich strand supplied the selected canonical k-mer value. */
enum class sequence_strand_mode : std::uint8_t {
    /** The forward-strand value was selected. */
    forward = 0,
    /** The reverse-complement value was selected. */
    reverse_complement = 1,
    /** Forward and reverse-complement values are equal. */
    equal = 2,
};

/**
 * @brief Checks the type requirements for viewing packed storage at another width.
 *
 * Keeps the alphabet and bits per symbol  unchanged.
 * @tparam p_range_configuration Source range configuration.
 * @tparam p_new_storage_unit_type Requested word type.
 */
template <typename p_range_configuration, typename p_new_storage_unit_type>
concept is_sequence_bit_castable_to = (
    supports_compact_sequence_range<p_range_configuration>
    &&
    is_sequence_custom_alphabet_bit_castable_to<
        sequence_custom_alphabet<
            typename compact_sequence_range<p_range_configuration>::
                traits::alphabet_type,
            typename compact_sequence_range<p_range_configuration>::
                traits::storage_unit,
            compact_sequence_range<p_range_configuration>::traits::bits_per_char>,
        sequence_custom_alphabet<
            typename compact_sequence_range<p_range_configuration>::
                traits::alphabet_type,
            p_new_storage_unit_type,
            compact_sequence_range<p_range_configuration>::traits::bits_per_char>
    >
);

static_assert(is_sequence_bit_castable_to<seq::dna4, uint64_t>);
static_assert(is_sequence_bit_castable_to<seq::dna4, uint32_t>);
static_assert(is_sequence_bit_castable_to<seq::dna4, uint16_t>);
static_assert(is_sequence_bit_castable_to<seq::dna4, uint8_t>);

/**
 * @brief Requires the read-only packed-view methods used by SIMD iteration
 *
 * Accepts a read_cptr result convertible to a const pointer of the view word type.
 * @tparam p_view_type Candidate view type.
 */
template <typename p_view_type>
concept supports_compact_sequence_bitcast_view =
    requires (
        p_view_type view,
        typename p_view_type::iterator& iterator,
        const typename p_view_type::iterator& const_iterator,
        size_t distance) {
        typename p_view_type::traits;
        typename p_view_type::traits::storage_unit;
        typename p_view_type::traits::alphabet_type;
        { view.bp_size() } -> std::convertible_to<size_t>;
        { view.byte_size() } -> std::convertible_to<size_t>;
        { view.size() } -> std::convertible_to<size_t>;
        { view.padding() } -> std::convertible_to<size_t>;
        { view.read_cptr(const_iterator) }
            -> std::convertible_to<
                const typename p_view_type::traits::storage_unit*>;
        { p_view_type::advance_iter(iterator, distance) };
        { p_view_type::view_distance(const_iterator, const_iterator) }
            -> std::convertible_to<size_t>;
    };

};

namespace fmt {

/**
 * @brief Disables generic range formatting for compact sequence ranges.
 * @tparam p_configuration Compact range backing configuration.
 * @tparam p_character_type Character type used by the formatting context.
 */
template<supports_compact_sequence_range p_configuration, typename p_character_type>
struct range_format_kind<
    compact_sequence_range<p_configuration>, p_character_type> {
    static constexpr auto value = range_format::disabled;
};

/**
 * @brief Formats sequence sizes, packing settings, and decoded symbols.
 *
 * Separates symbols from different packed words with |.
 */
template<supports_compact_sequence_range p_configuration>
struct formatter<compact_sequence_range<p_configuration>>
    : formatter<std::string> {
    /** Compact range type rendered by this formatter. */
    using range_type = compact_sequence_range<p_configuration>;

    /**
     * @brief Writes the sequence description to the fmt output context.
     * @param sequence Range to format.
     * @param context Destination context.
     * @return Output iterator from the string formatter.
     */
    template <typename p_format_context_type>
    auto format(
        compact_sequence_range<p_configuration> sequence,
        p_format_context_type& context) const {
        return formatter<std::string>::format(
            fmt::format(
                "sequence<bps={}|size={}|total={}B>({}->b{} +{})[{}]",
                sequence.bp_size(),
                sequence.size(),
                sequence.byte_size(),
                range_type::traits::letters_per_unit,
                range_type::traits::unit_width,
                sequence.padding(),
                sequence.to_string(true)),
            context);
    }
};


};

static_assert(fmt::has_formatter<compact_sequence_range<std::vector<seq::dna4>>, char>());

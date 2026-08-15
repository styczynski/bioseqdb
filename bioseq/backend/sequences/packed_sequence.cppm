/**
 * @file packed_sequence.cppm
 * @brief Packed sequence traits, iteration, dispatch, and storage views.
 *
 * The module keeps alphabet identity at the type level while exposing lazy,
 * zero-copy views over LSB-first packed storage. Width recasting preserves the
 * logical alphabet, padding, and sequence length.
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

export import :packed_sequence_core;
export import :packed_sequence_packing;
export import :packed_sequence_range;
export import :packed_sequence_wire;

export {

/**
 * @brief Compare compact ranges with different backing configurations.
 * @tparam p_left_configuration Left range backing configuration.
 * @tparam p_right_configuration Right range backing configuration.
 * @param lhs Left compact range.
 * @param rhs Right compact range.
 * @return `true` when both ranges contain the same logical symbols.
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

/** @brief Strand relation used by sequence hashing and ordering operations. */
enum class sequence_strand_mode : std::uint8_t {
    /** Traverse symbols in stored order. */
    forward = 0,
    /** Traverse the reverse complement of the stored symbols. */
    reverse_complement = 1,
    /** Compare strand-normalized values for equality. */
    equal = 2,
};

/**
 * @brief Tests whether a compact range can expose a different storage width.
 * @tparam p_range_configuration Compact-range backing configuration.
 * @tparam p_new_storage_unit_type Requested packed storage-unit type.
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
 * @brief Read-only packed-sequence interface accepted by SIMD iteration.
 *
 * The pointer requirement deliberately uses `convertible_to` so a view may
 * expose a more-qualified compatible pointer without materializing storage.
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

/** @brief Disables generic range formatting for compact sequence ranges. */
template<supports_compact_sequence_range p_configuration, typename p_character_type>
struct range_format_kind<
    compact_sequence_range<p_configuration>, p_character_type> {
    static constexpr auto value = range_format::disabled;
};

/**
 * @brief Formats packed range geometry and decoded unit-separated symbols.
 * @tparam p_configuration Compact range backing configuration.
 */
template<supports_compact_sequence_range p_configuration>
struct formatter<compact_sequence_range<p_configuration>>
    : formatter<std::string> {
    using range_type = compact_sequence_range<p_configuration>;

    /**
     * @brief Writes a compact range description to a formatting context.
     * @tparam p_format_context_type fmt output context.
     * @param sequence Compact sequence range to format.
     * @param context Destination formatting context.
     * @return Iterator returned by the string formatter.
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

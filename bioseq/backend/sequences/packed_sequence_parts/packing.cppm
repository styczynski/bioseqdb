/**
 * @file packing.cppm
 * @brief Extracts packed ranks and views the same bytes at another word width.
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

export module bioseq.sequences:packed_sequence_packing;

import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.common.templating;
import bioseq.sequences.rank_pack_core;
import :packed_sequence_core;

export {

// The first symbol occupies the low bits of a packed unit. Reading slots
// from low to high bits follows the sequence order.

namespace bit_pack_detail {

/**
 * @brief Builds an integer mask with the  selected bits set.
 * @tparam p_value_type Mask type.
 * @return Mask for the compile-time bit indices.
 */
template<typename p_value_type, std::size_t... p_bit_indices>
[[nodiscard]] constexpr p_value_type make_low_bit_mask(
    std::index_sequence<p_bit_indices...>) noexcept
{
    return ((static_cast<p_value_type>(1)
             << static_cast<p_value_type>(p_bit_indices)) | ...);
}

}  // namespace bit_pack_detail

/**
 * @brief Returns a low-bit mask for one packed symbol.
 * @tparam p_value_type Packed word type.
 * @tparam p_bits_per_letter Number of bits per symbol.
 */
template<typename p_value_type, std::size_t p_bits_per_letter>
[[nodiscard]] consteval p_value_type unit_rank_mask() noexcept {
    return bit_pack_detail::make_low_bit_mask<p_value_type>(
        std::make_index_sequence<p_bits_per_letter>{});
}

/**
 * @brief Reads one symbol rank from a packed word.
 * @tparam p_bits_per_letter Bits per symbol.
 * @tparam p_letter_index Slot counted from the low bits, starting at zero.
 * @param value Packed word.
 * @return Rank in the selected slot.
 */
template<typename p_value_type,
         std::size_t p_bits_per_letter,
         std::size_t p_letter_index>
[[always_inline]] [[nodiscard]] constexpr p_value_type extract_rank_from_unit(
    const p_value_type& value) noexcept
{
    static_assert(p_letter_index * p_bits_per_letter < sizeof(p_value_type) * 8,
        "rank slot exceeds the packed storage width");
    constexpr p_value_type bit_shift =
        static_cast<p_value_type>(p_bits_per_letter * p_letter_index);
    constexpr p_value_type mask =
        unit_rank_mask<p_value_type, p_bits_per_letter>() << bit_shift;
    return (value & mask) >> bit_shift;
}

namespace bit_pack_detail {

/**
 * @brief Reads selected symbol slots into a fixed array.
 *
 * Array order follows the supplied slot indices.
 * @param value Packed word
 * @return One rank per selected slot.
 */
template<typename p_value_type,
         std::size_t p_bits_per_letter,
         std::size_t p_letter_count,
         std::size_t... p_letter_indices>
[[always_inline]] [[nodiscard]] constexpr auto extract_ranks_from_unit_impl(
    std::index_sequence<p_letter_indices...>,
    const p_value_type& value) noexcept
    -> std::array<p_value_type, sizeof...(p_letter_indices)>
{
    // The caller supplies increasing  slot indices, so the array follows
    // sequence order from the low bits of the word.
    return std::array<p_value_type, sizeof...(p_letter_indices)>{
        extract_rank_from_unit<
            p_value_type, p_bits_per_letter, p_letter_indices>(value)...
    };
}

}  // namespace bit_pack_detail

/**
 * @brief Expands a packed word into ranks in sequence order.
 * @tparam p_bits_per_letter Bits per symbol.
 * @tparam p_letter_count Number of slots to read, starting frof the low bits.
 * @param value Packed word.
 * @return Fixed-size rank array.
 */
template<
    typename p_value_type,
    std::size_t p_bits_per_letter,
    std::size_t p_letter_count = (sizeof(p_value_type) * 8) / p_bits_per_letter
>
[[always_inline]] [[nodiscard]] constexpr auto extract_ranks_from_unit(
    const p_value_type& value) noexcept
    -> std::array<p_value_type, p_letter_count>
{
    return bit_pack_detail::extract_ranks_from_unit_impl<
        p_value_type, p_bits_per_letter, p_letter_count>(
            std::make_index_sequence<p_letter_count>{}, value);
}

/**
 * @brief Borrows packed bytes while reporting a different storage-word width.
 *
 * The alphabet and bits per symbol must match. Sizes and padding are calculated
 * for the requested width, while iterators still refer to the base range. The
 * caller must keep that range and its backing storage  alive.
 *
 * This view does not allocate storage or create objects of the new word type.
 * @tparam p_base_range Source range.
 * @tparam p_output_range Range type supplying the requested traits.
 */
template <typename p_base_range, typename p_output_range>
struct bit_casted_view {
public:
    using base_traits = p_base_range::traits;
    using traits = p_output_range::traits;
    using iterator = typename p_base_range::iterator;

private:
    const p_base_range& base_;
public:

    /**
     * @brief Borrows a range, whose storage will be viewed at another width.
     * @param base_container Range that must outlive this view.
     */
    explicit bit_casted_view(const p_base_range& base_container)
        : base_(base_container) {}

    // Downcasting uses words no wider than the source words, such as uint32_t
    // over uint64_t storage. Compare word sizes because wrapper sizes do not
    // describe the packed layout.
    static constexpr bool is_downcast =
        sizeof(typename traits::storage_unit) <=
        sizeof(typename base_traits::storage_unit);
    // Number of narrower words in one wider wwrd.
    static constexpr std::size_t cast_ratio = is_downcast
        ? (base_traits::unit_width / traits::unit_width)
        : (traits::unit_width / base_traits::unit_width);
    // Shift count for converting between  the two word widths.
    static constexpr std::size_t cast_ratio_shift =
        seqan3::detail::ceil_log2(cast_ratio);

    // Iterator conversion uses shifts, which require a power-of-two ratio
    static_assert(1u << cast_ratio_shift == cast_ratio);

    /** @return Logical-symbol count, unchanged by storage reinterpretation. */
    [[always_inline]] constexpr std::size_t bp_size() const {
        return base_.bp_size();
    };

    /** @return Borrowed byte count, unchanged by storage reinterpretation. */
    [[always_inline]] constexpr std::size_t byte_size() const {
        return base_.byte_size();
    };

    /** @return Packed units in the reinterpreted view. */
    [[always_inline]] constexpr std::size_t size() const {
        return view_units_for_letters();
    };

    /** @return Empty rank slots in the final reinterpreted unit. */
    [[always_inline]] constexpr std::size_t padding() const {
        // Count unused symbol slots using the view word width.
        return view_units_for_letters() * traits::letters_per_unit
            - base_.bp_size();
    };

private:
    /** @brief Returns the unit count required by the view storage width. */
    [[always_inline]] constexpr std::size_t view_units_for_letters() const {
        constexpr std::size_t letters_per_unit = traits::letters_per_unit;
        const std::size_t symbol_count = base_.bp_size();
        return symbol_count / letters_per_unit
            + (symbol_count % letters_per_unit != 0u);
    }
public:

    /**
     * @brief Returns the base iterator address cast to the view word type.
     * @param iterator Iterator into the base range.
     * @return Pointer to the same bytes.
     *
     * The cast does not make differently typed backing storage safe to dereference
     * as the new type. Callers reading such storage must use byte-safe loads.
     */
    template<typename p_iterator_type>
    requires(
        p_base_range::template valid_derived_iterator<p_iterator_type>::value)
    [[always_inline]] constexpr const typename traits::storage_unit* read_cptr(
        const p_iterator_type& iterator) const
    {
        return reinterpret_cast<const typename traits::storage_unit*>(
            base_.read_cptr(iterator));
    };

    /** @brief Returns the base range's constant begin iterator. */
    [[always_inline]] constexpr iterator cbegin() const {
        return base_.cbegin();
    }
    /** Returns the base range's constant begin iterator. */
    [[always_inline]] constexpr iterator begin() const {
        return base_.cbegin();
    }

    /**
     * @brief Advances a base iterator by a number of view words.
     * @param iterator Iterator to update.
     * @param value Number of view words. For  narrower views, this must be a multiple
     * of cast_ratio so the result stays on a base-word boundary.
     */
    template<typename p_iterator_type>
    requires(
        p_base_range::template valid_derived_iterator<p_iterator_type>::value)
    [[always_inline]] constexpr static void advance_iter(
        p_iterator_type& iterator, const std::size_t value) {
        // Narrower view words must advance in groups that fill one source word.
        // Wider view words each cover several source words.
        if constexpr (is_downcast) {
            DEBUG_ASSERT(value % cast_ratio == 0,
                fmt::format("bit_casted_view downcast (ratio={}) requires advance"
                    " value ({}) divisible by cast_ratio for zero-cost shift-as-divide",
                    cast_ratio, value));
            iterator += (value >> cast_ratio_shift);
        } else {
            iterator += (value << cast_ratio_shift);
        }
    };

    /**
     * @brief Measures a base-iterator interval in view words.
     * @param begin First base iterator.
     * @param end Last base iterator.
     * @return Converted distance. A wider view counts only complete view words.
     */
    template<typename p_iterator_type>
    [[always_inline]] constexpr static std::size_t view_distance(
        const p_iterator_type& begin,
        const p_iterator_type& end) {
        if constexpr (is_downcast) {
            return static_cast<std::size_t>(end - begin) << cast_ratio_shift;
        } else {
            return static_cast<std::size_t>(end - begin) >> cast_ratio_shift;
        }
    }
};

};

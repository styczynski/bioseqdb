/**
 * @file packing.cppm
 * @brief LSB-first packed-unit extraction and storage-width views.
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

// Packed units store the first logical symbol in the least-significant rank
// slot. Unit extraction therefore returns ranks from low to high bit slots.

namespace bit_pack_detail {

/**
 * @brief Builds a mask from a compile-time sequence of low bit indices.
 * @tparam p_value_type Unsigned mask type.
 * @return Value with every selected bit set.
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
 * @brief Build a mask covering one packed rank slot.
 * @tparam p_value_type Unsigned packed word type.
 * @tparam p_bits_per_letter Packed rank width.
 * @return Low-bit mask of `p_bits_per_letter` one bits.
 */
template<typename p_value_type, std::size_t p_bits_per_letter>
[[nodiscard]] consteval p_value_type unit_rank_mask() noexcept {
    return bit_pack_detail::make_low_bit_mask<p_value_type>(
        std::make_index_sequence<p_bits_per_letter>{});
}

/**
 * @brief Extract a single letter from a storage word.
 *
 * The zero index selects the least-significant slot that stores the first
 * logical symbol of a fully populated unit.
 *
 * @tparam p_value_type Unsigned packed word type.
 * @tparam p_bits_per_letter Packed rank width.
 * @tparam p_letter_index Zero-based rank slot from the least-significant end.
 * @param value Packed source word.
 * @return Rank stored in the selected slot.
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
 * @brief Expands selected packed rank slots into a fixed array.
 * @tparam p_value_type Unsigned packed word type.
 * @tparam p_bits_per_letter Packed rank width.
 * @tparam p_letter_count Number of logical slots requested by the caller.
 * @param value Packed source word.
 * @return Ranks ordered from low to high bit slots.
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
    // Index 0 reads from bit slot 0 (the LSB end). bioseq's encoding
    // puts the FIRST input letter at bit slot 0, the SECOND at slot 1,
    // etc., so walking the returned array in order matches the
    // input-sequence order — the packed iterator contract.
    return std::array<p_value_type, sizeof...(p_letter_indices)>{
        extract_rank_from_unit<
            p_value_type, p_bits_per_letter, p_letter_indices>(value)...
    };
}

}  // namespace bit_pack_detail

/**
 * @brief Decomposes one packed word into low-to-high rank slots.
 *
 * @tparam p_value_type Unsigned packed word type.
 * @tparam p_bits_per_letter Packed rank width.
 * @tparam p_letter_count Rank slots extracted from the word.
 * @param value Packed source word.
 * @return Fixed-size array ordered from least- to most-significant rank slot.
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
 * @brief Reinterprets packed bytes through another compatible storage width.
 *
 * Source and output ranges retain the same alphabet and rank width. Both unit
 * widths must contain complete rank slots so the byte stream stays unchanged.
 *
 * @tparam p_base_range Borrowed packed range owning the source iterators.
 * @tparam p_output_range Packed range configuration defining the view width.
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
     * @brief Binds a packed range whose bytes are reinterpreted.
     * @param base_container Borrowed packed range.
     */
    explicit bit_casted_view(const p_base_range& base_container)
        : base_(base_container) {}

    // If this is downcast or upcast. Downcast = we re-interpret a wide unit
    // as a narrower unit (e.g. uint64 → uint32); upcast = the reverse. The
    // sizeof comparison here is on the *storage_unit*, not on the wrapping
    // sequence type — the wrapping types are the same size (both wrap a
    // std::vector + size_t) regardless of the storage_unit.
    static constexpr bool is_downcast =
        sizeof(typename traits::storage_unit) <=
        sizeof(typename base_traits::storage_unit);
    // Ratio between source and target unit widths in bits — always >= 1.
    static constexpr std::size_t cast_ratio = is_downcast
        ? (base_traits::unit_width / traits::unit_width)
        : (traits::unit_width / base_traits::unit_width);
    // Log2 of cast_ratio (useful for shifts).
    static constexpr std::size_t cast_ratio_shift =
        seqan3::detail::ceil_log2(cast_ratio);

    // Verifies that cast_ratio is power of two, otherwise advance_iter's
    // shift-as-divide / shift-as-multiply lowering would be wrong.
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
        // Empty letter slots in the trailing view-unit: units*lpu - bps.
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
     * @brief Reinterpret an admitted base iterator as the view storage type.
     * @tparam p_iterator_type Iterator accepted by the base range.
     * @param iterator Base-range iterator.
     * @return Pointer into the same borrowed bytes, with view unit type.
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
    /** @brief Returns the base range's begin iterator. */
    [[always_inline]] constexpr iterator begin() const {
        return base_.cbegin();
    }

    /**
     * @brief Advances a base iterator by a number of view units.
     * @tparam p_iterator_type Iterator accepted by the base range.
     * @param iterator Iterator updated in place.
     * @param value Number of view units.
     */
    template<typename p_iterator_type>
    requires(
        p_base_range::template valid_derived_iterator<p_iterator_type>::value)
    [[always_inline]] constexpr static void advance_iter(
        p_iterator_type& iterator, const std::size_t value) {
        // This is interesting part:
        // - downcast: one real unit covers `cast_ratio` view units, so each
        //   `cast_ratio` view-unit steps maps to 1 real iter increment. We
        //   require `value` divisible by `cast_ratio` so the shift-as-divide
        //   below is exact (otherwise we'd need pointer arithmetic). The
        //   only call site today is the SIMD chunk loop which advances by
        //   `Lanes(d)` — a power-of-two that scales the same way as
        //   `cast_ratio`, so the divisibility holds by construction.
        // - upcast: each view-unit step covers `cast_ratio` real units, so
        //   we advance by `value * cast_ratio`. No divisibility constraint.
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
     * @brief Measures an iterator interval in view storage units.
     * @tparam p_iterator_type Iterator accepted by the base range.
     * @param begin First base iterator.
     * @param end Last base iterator.
     * @return Distance converted to the reinterpreted unit width.
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

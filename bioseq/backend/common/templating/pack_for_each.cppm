/**
 * @file pack_for_each.cppm
 * @brief Compile-time pack iteration and fixed-capacity array helpers.
 */
module;

#include <array>
#include <cstddef>
#include <hwy/highway.h>
#include <type_traits>
#include <utility>

export module bioseq.common.templating.pack_for_each;

export namespace bioseq::common::templating {

/**
 * @brief Invokes a callable once for every index in `[0, count)`.
 * @tparam count Number of compile-time indices.
 * @tparam function_type Callable type.
 * @param function Callable accepting an integral-constant index.
 *
 * Highway and compiler force-inline attributes are retained so target-specific
 * callers receive the same attribute propagation as a handwritten expansion.
 */
template <std::size_t count, class function_type>
[[gnu::always_inline]] HWY_INLINE
constexpr void for_each_index(function_type&& function)
{
    [&]<std::size_t... indices>(std::index_sequence<indices...>)
        [[gnu::always_inline]]
    {
        (function(std::integral_constant<std::size_t, indices>{}), ...);
    }(std::make_index_sequence<count>{});
}

/**
 * @brief Invokes a templated callable once for every type in a pack.
 * @tparam element_types Types to visit.
 * @tparam function_type Callable type.
 * @param function Callable exposing `operator()<element_type>()`.
 */
template <class... element_types, class function_type>
[[gnu::always_inline]] constexpr void for_each_type(function_type&& function)
{
    (function.template operator()<element_types>(), ...);
}

/**
 * @brief Invokes a templated callable once for every value in a pack.
 * @tparam values Structural values to visit.
 * @tparam function_type Callable type.
 * @param function Callable exposing `operator()<value>()`.
 */
template <auto... values, class function_type>
[[gnu::always_inline]] constexpr void for_each_value(function_type&& function)
{
    (function.template operator()<values>(), ...);
}

/**
 * @brief Appends a value to a fixed-capacity prefix when it is not present.
 * @tparam element_type Array element type.
 * @tparam capacity Array capacity.
 * @param buffer Destination array.
 * @param live_count Number of initialized prefix elements; incremented on append.
 * @param value Candidate value.
 *
 * A full buffer is left unchanged. The operation is intended for constant
 * evaluation where the caller owns capacity validation.
 */
template <class element_type, std::size_t capacity>
consteval void append_unique_value(
    std::array<element_type, capacity>& buffer,
    std::size_t& live_count,
    element_type value) noexcept
{
    for (std::size_t scan_index = 0; scan_index < live_count; ++scan_index) {
        if (buffer[scan_index] == value) return;
    }
    if (live_count < capacity) buffer[live_count++] = value;
}

/**
 * @brief Sorts an initialized array prefix in ascending order.
 * @tparam element_type Array element type.
 * @tparam capacity Array capacity.
 * @param buffer Array containing the prefix.
 * @param live_count Number of prefix elements to sort.
 */
template <class element_type, std::size_t capacity>
consteval void sort_in_place_ascending(
    std::array<element_type, capacity>& buffer,
    std::size_t live_count) noexcept
{
    for (std::size_t insertion_index = 1u;
         insertion_index < live_count;
         ++insertion_index) {
        for (std::size_t cursor = insertion_index;
             cursor > 0u && buffer[cursor] < buffer[cursor - 1u];
             --cursor) {
            const element_type carried_value = buffer[cursor];
            buffer[cursor] = buffer[cursor - 1u];
            buffer[cursor - 1u] = carried_value;
        }
    }
}

} // namespace bioseq::common::templating

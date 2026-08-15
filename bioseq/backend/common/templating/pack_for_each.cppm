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
 * @brief Invokes a callable once for every index in `[0, p_count)`.
 * @tparam p_count Number of compile-time indices.
 * @tparam p_function_type Callable type.
 * @param[in,out] function Callable accepting an integral-constant index.
 *
 * Highway and compiler force-inline attributes are retained so target-specific
 * callers receive the same attribute propagation as a handwritten expansion.
 */
template <std::size_t p_count, class p_function_type>
[[gnu::always_inline]] HWY_INLINE
constexpr void for_each_index(p_function_type&& function)
{
    [&function]<std::size_t... p_indices>(std::index_sequence<p_indices...>)
        [[gnu::always_inline]]
    {
        (function(std::integral_constant<std::size_t, p_indices>{}), ...);
    }(std::make_index_sequence<p_count>{});
}

/**
 * @brief Invokes a templated callable once for every type in a pack.
 * @tparam p_element_types Types to visit.
 * @tparam p_function_type Callable type.
 * @param[in,out] function Callable exposing `operator()<selected_type>()`.
 */
template <class... p_element_types, class p_function_type>
[[gnu::always_inline]] constexpr void for_each_type(
    p_function_type&& function)
{
    (function.template operator()<p_element_types>(), ...);
}

/**
 * @brief Invokes a templated callable once for every value in a pack.
 * @tparam p_values Structural values to visit.
 * @tparam p_function_type Callable type.
 * @param[in,out] function Callable exposing `operator()<selected_value>()`.
 */
template <auto... p_values, class p_function_type>
[[gnu::always_inline]] constexpr void for_each_value(
    p_function_type&& function)
{
    (function.template operator()<p_values>(), ...);
}

/**
 * @brief Appends a value to a fixed-capacity prefix when it is not present.
 * @tparam p_element_type Array element type.
 * @tparam p_capacity Array capacity.
 * @param[in,out] buffer Destination array.
 * @param[in,out] live_count Number of initialized prefix elements; incremented on append.
 * @param[in] value Candidate value.
 *
 * A full buffer is left unchanged. The operation is intended for constant
 * evaluation where the caller owns capacity validation.
 * @pre `live_count <= p_capacity`.
 */
template <class p_element_type, std::size_t p_capacity>
consteval void append_unique_value(
    std::array<p_element_type, p_capacity>& buffer,
    std::size_t& live_count,
    p_element_type value) noexcept
{
    for (std::size_t scan_index = 0; scan_index < live_count; ++scan_index) {
        if (buffer[scan_index] == value) return;
    }
    if (live_count < p_capacity) buffer[live_count++] = value;
}

/**
 * @brief Sorts an initialized array prefix in ascending order.
 * @tparam p_element_type Array element type.
 * @tparam p_capacity Array capacity.
 * @param[in,out] buffer Array containing the prefix.
 * @param[in] live_count Number of prefix elements to sort.
 * @pre `live_count <= p_capacity`.
 */
template <class p_element_type, std::size_t p_capacity>
consteval void sort_in_place_ascending(
    std::array<p_element_type, p_capacity>& buffer,
    std::size_t live_count) noexcept
{
    for (std::size_t insertion_index = 1u;
         insertion_index < live_count;
         ++insertion_index) {
        for (std::size_t cursor = insertion_index;
             cursor > 0u && buffer[cursor] < buffer[cursor - 1u];
             --cursor) {
            const p_element_type carried_value = buffer[cursor];
            buffer[cursor] = buffer[cursor - 1u];
            buffer[cursor - 1u] = carried_value;
        }
    }
}

} // namespace bioseq::common::templating

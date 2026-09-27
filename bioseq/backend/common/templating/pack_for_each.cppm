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
 * @brief Calls `function` once for each index in `[0, p_count)`.
 * @param[in,out] function Callable accepting an integral-constant index.
 *
 * Force-inlining lets CPU-specific callers expand these calls with their
 * Highway target settings.
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
 * @brief Calls `function.operator()<T>()` for each type in the pack.
 */
template <class... p_element_types, class p_function_type>
[[gnu::always_inline]] constexpr void for_each_type(
    p_function_type&& function)
{
    (function.template operator()<p_element_types>(), ...);
}

/**
 * @brief Calls `function.operator()<value>()` for each value in the pack.
 */
template <auto... p_values, class p_function_type>
[[gnu::always_inline]] constexpr void for_each_value(
    p_function_type&& function)
{
    (function.template operator()<p_values>(), ...);
}

/**
 * @brief Appends a value unless it is already present or the buffer is full.
 * @param[in,out] buffer Array whose first `live_count` elements are in use.
 * @param[in,out] live_count Increases when the value is appended.
 * @param[in] value Value to add.
 * @pre `live_count <= p_capacity`.
 *
 * This helper runs at compile time. The caller must check capacity if silently
 * leaving a full buffer unchanged would be an error.
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

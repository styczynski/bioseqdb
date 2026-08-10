/**
 * @file pack_search.cppm
 * @brief Compile-time membership, position, and uniqueness queries for packs.
 */
module;

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

export module bioseq.common.templating.pack_search;

export namespace bioseq::common::templating {

/**
 * @brief Reports whether a type occurs in a type pack.
 * @tparam p_needle_type Type to find.
 * @tparam p_haystack_types Types to search.
 */
template <class p_needle_type, class... p_haystack_types>
[[nodiscard]] consteval bool contains_type_pack() noexcept {
    return (std::is_same_v<p_needle_type, p_haystack_types> || ...);
}

/**
 * @brief Type trait reporting whether a tuple contains a type.
 * @tparam p_needle_type Type to find.
 * @tparam p_tuple_type Tuple type to search.
 */
template <class p_needle_type, class p_tuple_type>
struct contains_type_in_tuple : std::false_type {};

/// Specialization of `contains_type_in_tuple` for `std::tuple`.
template <class p_needle_type, class... p_element_types>
struct contains_type_in_tuple<
    p_needle_type,
    std::tuple<p_element_types...>>
    : std::bool_constant<
          (std::is_same_v<p_needle_type, p_element_types> || ...)> {};

/**
 * @brief Value form of `contains_type_in_tuple`.
 * @tparam p_needle_type Type to find.
 * @tparam p_tuple_type Tuple type to search.
 */
template <class p_needle_type, class p_tuple_type>
inline constexpr bool contains_type_v =
    contains_type_in_tuple<p_needle_type, p_tuple_type>::value;

/**
 * @brief Finds the first index of a type in a pack.
 * @tparam p_needle_type Type to find.
 * @tparam p_haystack_types Types to search.
 * @return First matching index, or the pack size when absent.
 */
template <class p_needle_type, class... p_haystack_types>
[[nodiscard]] consteval std::size_t index_of_type_pack() noexcept {
    std::size_t result = 0;
    bool found = false;
    ((std::is_same_v<p_needle_type, p_haystack_types> && !found
          ? (found = true, true)
          : (++result, false))
     || ...);
    return found ? result : sizeof...(p_haystack_types);
}

/**
 * @brief Finds the first index of a required type in a pack.
 * @tparam p_needle_type Type to find.
 * @tparam p_haystack_types Types to search.
 * @return First matching index.
 */
template <class p_needle_type, class... p_haystack_types>
[[nodiscard]] consteval std::size_t index_of_type_pack_checked() noexcept {
    static_assert(
        contains_type_pack<p_needle_type, p_haystack_types...>(),
        "index_of_type_pack_checked: needle type is absent");
    return index_of_type_pack<p_needle_type, p_haystack_types...>();
}

/**
 * @brief Type trait holding the first index of a type in a tuple.
 * @tparam p_needle_type Type to find.
 * @tparam p_tuple_type Tuple type to search.
 */
template <class p_needle_type, class p_tuple_type>
struct index_of_type_in_tuple;

/// Specialization of `index_of_type_in_tuple` for `std::tuple`.
template <class p_needle_type, class... p_element_types>
struct index_of_type_in_tuple<
    p_needle_type,
    std::tuple<p_element_types...>> {
    /// First matching tuple index.
    static constexpr std::size_t value =
        index_of_type_pack_checked<p_needle_type, p_element_types...>();
};

/**
 * @brief Value form of `index_of_type_in_tuple`.
 * @tparam p_needle_type Type to find.
 * @tparam p_tuple_type Tuple type to search.
 */
template <class p_needle_type, class p_tuple_type>
inline constexpr std::size_t index_of_type_v =
    index_of_type_in_tuple<p_needle_type, p_tuple_type>::value;

/**
 * @brief Reports whether a structural value occurs in a value pack.
 * @tparam p_needle_value Value to find.
 * @tparam p_haystack_values Values to search.
 */
template <auto p_needle_value, auto... p_haystack_values>
[[nodiscard]] consteval bool contains_value_pack() noexcept {
    return ((p_needle_value == p_haystack_values) || ...);
}

/**
 * @brief Finds the first index of a required structural value in a pack.
 * @tparam p_needle_value Value to find.
 * @tparam p_haystack_values Values to search.
 * @return First matching index.
 */
template <auto p_needle_value, auto... p_haystack_values>
[[nodiscard]] consteval std::size_t index_of_value_pack() noexcept {
    static_assert(
        contains_value_pack<p_needle_value, p_haystack_values...>(),
        "index_of_value_pack: needle value is absent");
    std::size_t result = 0;
    bool found = false;
    (((p_needle_value == p_haystack_values) && !found
          ? (found = true, true)
          : (++result, false))
     || ...);
    return result;
}

namespace detail_pack_search {

/** Reports whether one tuple element differs from every following element. */
template <std::size_t p_element_index, class p_tuple_type,
          std::size_t... p_following_offsets>
[[nodiscard]] consteval bool element_differs_from_following(
    std::index_sequence<p_following_offsets...>) noexcept
{
    using element_type =
        std::tuple_element_t<p_element_index, p_tuple_type>;
    return ((!std::is_same_v<
        element_type,
        std::tuple_element_t<
            p_element_index + 1u + p_following_offsets,
            p_tuple_type>>) && ...);
}

/** Reports whether all tuple elements have distinct types. */
template <class p_tuple_type, std::size_t... p_element_indices>
[[nodiscard]] consteval bool unique_types_in_tuple_impl(
    std::index_sequence<p_element_indices...>) noexcept
{
    constexpr std::size_t tuple_size = std::tuple_size_v<p_tuple_type>;
    return (element_differs_from_following<
        p_element_indices,
        p_tuple_type>(
        std::make_index_sequence<
            (tuple_size > p_element_indices + 1u
                ? tuple_size - p_element_indices - 1u
                : 0u)>{}) && ...);
}

} // namespace detail_pack_search

/**
 * @brief Reports whether every element of a tuple has a distinct type.
 * @tparam p_tuple_type Tuple type to inspect.
 */
template <class p_tuple_type>
inline constexpr bool unique_types_in_tuple_v =
    detail_pack_search::unique_types_in_tuple_impl<p_tuple_type>(
        std::make_index_sequence<std::tuple_size_v<p_tuple_type>>{});

/**
 * @brief Reports whether every value in a pack is distinct.
 * @tparam p_values Structural values to inspect.
 */
template <auto... p_values>
[[nodiscard]] consteval bool unique_values_pack() noexcept {
    if constexpr (sizeof...(p_values) <= 1u) {
        return true;
    } else {
        std::size_t position = 0;
        bool unique = true;
        (((index_of_value_pack<p_values, p_values...>() == position++)
              ? true
              : (unique = false)),
         ...);
        return unique;
    }
}

} // namespace bioseq::common::templating

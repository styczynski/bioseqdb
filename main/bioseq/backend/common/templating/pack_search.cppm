/**
 * @file pack_search.cppm
 * @brief Compile-time predicates over template parameter packs and tuples.
 *
 * Provides `contains_type` / `index_of_type` / `contains_value` /
 * `index_of_value` plus a `unique_types_in_tuple_v` invariant probe.
 * Used by `supported_alphabets<...>` and by `entry_for<A>`.
 *
 * `index_of_type_pack` (unchecked) returns `sizeof...(Hay)` when the
 * needle is absent — callers using the alias under a `requires` guard
 * never observe the out-of-bounds index because the guard removes the
 * specialisation from overload resolution first.
 *
 * `index_of_type_pack_checked` is the asserting wrapper for callers
 * where absence is a hard build failure.
 */
module;

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

export module bioseq.common.templating.pack_search;

export namespace bioseq::common::templating {

template <class Needle, class... Hay>
[[nodiscard]] consteval bool contains_type_pack() noexcept {
    return (std::is_same_v<Needle, Hay> || ...);
}

template <class Needle, class Tuple>
struct contains_type_in_tuple : std::false_type {};

template <class Needle, class... Ts>
struct contains_type_in_tuple<Needle, std::tuple<Ts...>>
    : std::bool_constant<(std::is_same_v<Needle, Ts> || ...)> {};

template <class Needle, class Tuple>
inline constexpr bool contains_type_v = contains_type_in_tuple<Needle, Tuple>::value;

template <class Needle, class... Hay>
[[nodiscard]] consteval std::size_t index_of_type_pack() noexcept {
    std::size_t result = 0;
    bool found = false;
    ((std::is_same_v<Needle, Hay> && !found
        ? (found = true, true)
        : (++result, false)) || ...);
    return found ? result : sizeof...(Hay);
}

template <class Needle, class... Hay>
[[nodiscard]] consteval std::size_t index_of_type_pack_checked() noexcept {
    static_assert(contains_type_pack<Needle, Hay...>(),
                  "index_of_type_pack_checked: Needle is not in Hay");
    return index_of_type_pack<Needle, Hay...>();
}

template <class Needle, class Tuple>
struct index_of_type_in_tuple;

template <class Needle, class... Ts>
struct index_of_type_in_tuple<Needle, std::tuple<Ts...>> {
    static constexpr std::size_t value = index_of_type_pack_checked<Needle, Ts...>();
};

template <class Needle, class Tuple>
inline constexpr std::size_t index_of_type_v =
    index_of_type_in_tuple<Needle, Tuple>::value;

template <auto Needle, auto... Hay>
[[nodiscard]] consteval bool contains_value_pack() noexcept {
    return ((Needle == Hay) || ...);
}

template <auto Needle, auto... Hay>
[[nodiscard]] consteval std::size_t index_of_value_pack() noexcept {
    static_assert(contains_value_pack<Needle, Hay...>(),
                  "index_of_value_pack: Needle is not in Hay");
    std::size_t result = 0;
    bool found = false;
    (((Needle == Hay) && !found
        ? (found = true, true)
        : (++result, false)) || ...);
    return result;
}

namespace detail_pack_search {

template <std::size_t I, class Tuple, std::size_t... Js>
[[nodiscard]] consteval bool ith_unique(std::index_sequence<Js...>) noexcept {
    using Ti = std::tuple_element_t<I, Tuple>;
    return ((!std::is_same_v<Ti, std::tuple_element_t<I + 1u + Js, Tuple>>) && ...);
}

template <class Tuple, std::size_t... Is>
[[nodiscard]] consteval bool unique_types_in_tuple_impl(
    std::index_sequence<Is...>) noexcept
{
    constexpr std::size_t N = std::tuple_size_v<Tuple>;
    return (ith_unique<Is, Tuple>(std::make_index_sequence<
              (N > Is + 1u ? N - Is - 1u : 0u)>{}) && ...);
}

} // namespace detail_pack_search

template <class Tuple>
inline constexpr bool unique_types_in_tuple_v =
    detail_pack_search::unique_types_in_tuple_impl<Tuple>(
        std::make_index_sequence<std::tuple_size_v<Tuple>>{});

// All-distinct check over a value pack — the value-side twin of
// unique_types_in_tuple_v. Reuses index_of_value_pack: a key is a duplicate iff
// its first-occurrence index does not equal its own position.
template <auto... Vs>
[[nodiscard]] consteval bool unique_values_pack() noexcept {
    if constexpr (sizeof...(Vs) <= 1u) {
        return true;
    } else {
        std::size_t pos = 0;
        bool ok = true;
        (((index_of_value_pack<Vs, Vs...>() == pos++) ? true : (ok = false)), ...);
        return ok;
    }
}

static_assert(contains_type_pack<int, char, short, int, long>());
static_assert(!contains_type_pack<int, char, short, long>());
static_assert(index_of_type_pack<int, char, short, int, long>() == 2u);
static_assert(contains_value_pack<3u, 1u, 2u, 3u>());
static_assert(index_of_value_pack<3u, 1u, 2u, 3u>() == 2u);
static_assert(unique_types_in_tuple_v<std::tuple<int, char, short>>);
static_assert(!unique_types_in_tuple_v<std::tuple<int, char, int>>);
static_assert(unique_values_pack<1u, 2u, 3u>());
static_assert(!unique_values_pack<1u, 2u, 1u>());

} // namespace

/**
 * @file pack_for_each.cppm
 * @brief Compile-time loops over packs plus consteval set-union primitives.
 *
 * Provides `for_each_index<N>(fn)`, `for_each_type<Ts...>(fn)`,
 * `for_each_value<Vs...>(fn)` for templated-lambda loops, and the
 * consteval helpers `append_unique_value` / `sort_in_place_ascending`
 * lifted out of `pipe.cppm`'s bucket-derivation code.
 *
 * `for_each_index` carries both `HWY_INLINE` and `[[gnu::always_inline]]`
 * so the existing SIMD callers (re-exported via `simd/utils.cppm`)
 * inherit Highway's SIMD-target attribute pass-through.
 */
module;

#include <array>
#include <cstddef>
#include <hwy/highway.h>
#include <type_traits>
#include <utility>

export module bioseq_common.templating.pack_for_each;

export namespace bioseq::common::templating {

template <std::size_t N, class F>
[[gnu::always_inline]] HWY_INLINE
constexpr void for_each_index(F&& fn)
{
    [&]<std::size_t... Is>(std::index_sequence<Is...>) [[gnu::always_inline]] {
        (fn(std::integral_constant<std::size_t, Is>{}), ...);
    }(std::make_index_sequence<N>{});
}

template <class... Ts, class F>
[[gnu::always_inline]] constexpr void for_each_type(F&& fn)
{
    (fn.template operator()<Ts>(), ...);
}

template <auto... Vs, class F>
[[gnu::always_inline]] constexpr void for_each_value(F&& fn)
{
    (fn.template operator()<Vs>(), ...);
}

template <class T, std::size_t Cap>
consteval void append_unique_value(
    std::array<T, Cap>& buf, std::size_t& live_count, T value) noexcept
{
    for (std::size_t scan_index = 0; scan_index < live_count; ++scan_index) {
        if (buf[scan_index] == value) return;
    }
    if (live_count < Cap) buf[live_count++] = value;
}

template <class T, std::size_t Cap>
consteval void sort_in_place_ascending(
    std::array<T, Cap>& buf, std::size_t live_count) noexcept
{
    for (std::size_t outer = 1u; outer < live_count; ++outer) {
        for (std::size_t cursor = outer;
             cursor > 0u && buf[cursor] < buf[cursor - 1u];
             --cursor) {
            const T carry           = buf[cursor];
            buf[cursor]             = buf[cursor - 1u];
            buf[cursor - 1u]        = carry;
        }
    }
}

} // namespace

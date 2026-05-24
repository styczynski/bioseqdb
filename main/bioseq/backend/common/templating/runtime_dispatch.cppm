/**
 * @file runtime_dispatch.cppm
 * @brief Runtime → compile-time fanout primitives.
 *
 * Provides the void-return primitive `dispatch_recursive_pack` and
 * the optional-return primitive `find_first_match_in_pack`, both
 * walking a type pack passed as a `type_list<...>` carrier and
 * routing the first match through a templated handler.
 *
 * Also exposes the enum/value dispatchers from DOC0256:
 *  - `enum_dispatch_pack` / `enum_dispatch_pack_void`
 *  - `value_dispatch_smallest_ge`
 *  - `value_dispatch_exact_with_default`
 *
 * Every public entry is `[[gnu::always_inline]]` and `constexpr` so the
 * cmov ladder produced at the call site matches the hand-written
 * recursive ladders the project used before extraction.
 */
module;

#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

export module bioseq_common.templating.runtime_dispatch;

import bioseq_common.templating.value_sequence;

export namespace bioseq::common::templating {

template <class... Ts>
struct type_list { static constexpr std::size_t size = sizeof...(Ts); };

namespace detail_runtime_dispatch {

template <class IndexT, class HandlerFn, class Probe,
          class Head, class... Tail>
[[gnu::always_inline]]
constexpr void dispatch_recursive_pack_impl(
    HandlerFn& handler, Probe& probe, IndexT cursor)
{
    if (probe.template matches<Head>(cursor)) {
        handler.template operator()<Head>();
        return;
    }
    if constexpr (sizeof...(Tail) > 0) {
        dispatch_recursive_pack_impl<IndexT, HandlerFn, Probe, Tail...>(
            handler, probe, static_cast<IndexT>(cursor + 1));
    } else {
        probe.on_no_match(cursor);
    }
}

template <class IndexT, class R, class Probe,
          class Head, class... Tail>
[[gnu::always_inline]]
constexpr std::optional<R> find_first_match_in_pack_impl(
    Probe& probe, IndexT cursor)
{
    if (auto opt = probe.template extract<Head>(cursor); opt.has_value()) {
        return opt;
    }
    if constexpr (sizeof...(Tail) > 0) {
        return find_first_match_in_pack_impl<IndexT, R, Probe, Tail...>(
            probe, static_cast<IndexT>(cursor + 1));
    } else {
        return std::nullopt;
    }
}

} // namespace detail_runtime_dispatch

template <class IndexT, class TypeList, class HandlerFn, class Probe>
[[gnu::always_inline]]
constexpr void dispatch_recursive_pack(HandlerFn&& handler, Probe&& probe) {
    [&]<class... Types>(type_list<Types...>) [[gnu::always_inline]] {
        if constexpr (sizeof...(Types) > 0) {
            detail_runtime_dispatch::dispatch_recursive_pack_impl<
                IndexT, std::remove_reference_t<HandlerFn>,
                std::remove_reference_t<Probe>, Types...>(
                    handler, probe, static_cast<IndexT>(0));
        } else {
            probe.on_no_match(IndexT{0});
        }
    }(TypeList{});
}

template <class IndexT, class R, class TypeList, class Probe>
[[gnu::always_inline]]
constexpr std::optional<R> find_first_match_in_pack(Probe&& probe) {
    return [&]<class... Types>(type_list<Types...>)
        [[gnu::always_inline]] -> std::optional<R> {
            if constexpr (sizeof...(Types) > 0) {
                return detail_runtime_dispatch::find_first_match_in_pack_impl<
                    IndexT, R, std::remove_reference_t<Probe>, Types...>(
                        probe, static_cast<IndexT>(0));
            } else {
                return std::nullopt;
            }
        }(TypeList{});
}

namespace detail_runtime_dispatch {

template <class R, class T, T DefaultValue, T V_head, T... V_rest,
          class F, class... Args>
[[gnu::always_inline]]
R enum_dispatch_search(T rt, F& f, Args&&... args)
{
    if (rt == V_head) {
        return f(std::integral_constant<T, V_head>{},
                 std::forward<Args>(args)...);
    }
    if constexpr (sizeof...(V_rest) > 0u) {
        return enum_dispatch_search<R, T, DefaultValue, V_rest...>(
            rt, f, std::forward<Args>(args)...);
    } else {
        return f(std::integral_constant<T, DefaultValue>{},
                 std::forward<Args>(args)...);
    }
}

template <class T, T DefaultValue, T V_head, T... V_rest,
          class F, class... Args>
[[gnu::always_inline]]
void enum_dispatch_search_void(T rt, F& f, Args&&... args)
{
    if (rt == V_head) {
        f(std::integral_constant<T, V_head>{}, std::forward<Args>(args)...);
        return;
    }
    if constexpr (sizeof...(V_rest) > 0u) {
        enum_dispatch_search_void<T, DefaultValue, V_rest...>(
            rt, f, std::forward<Args>(args)...);
    } else {
        f(std::integral_constant<T, DefaultValue>{}, std::forward<Args>(args)...);
    }
}

template <class R, class T, T V_head, T... V_rest, class F, class... Args>
[[gnu::always_inline]]
R value_dispatch_smallest_ge_search(T rt, F& f, Args&&... args)
{
    if (V_head >= rt) {
        return f(std::integral_constant<T, V_head>{},
                 std::forward<Args>(args)...);
    }
    if constexpr (sizeof...(V_rest) > 0u) {
        return value_dispatch_smallest_ge_search<R, T, V_rest...>(
            rt, f, std::forward<Args>(args)...);
    } else {
        return f(std::integral_constant<T, V_head>{},
                 std::forward<Args>(args)...);
    }
}

} // namespace detail_runtime_dispatch

template <class R, class T, T DefaultValue, T... Vs, class F, class... Args>
[[nodiscard, gnu::always_inline]]
R enum_dispatch_pack(value_sequence<T, Vs...>,
                     T rt, F&& f, Args&&... args)
{
    static_assert(sizeof...(Vs) > 0u, "enum_dispatch_pack: empty value pack");
    static_assert(((Vs == DefaultValue) || ...),
        "enum_dispatch_pack: DefaultValue must appear in the value pack");
    return detail_runtime_dispatch::enum_dispatch_search<
        R, T, DefaultValue, Vs...>(rt, f, std::forward<Args>(args)...);
}

template <class T, T DefaultValue, T... Vs, class F, class... Args>
[[gnu::always_inline]]
void enum_dispatch_pack_void(value_sequence<T, Vs...>,
                             T rt, F&& f, Args&&... args)
{
    static_assert(sizeof...(Vs) > 0u, "enum_dispatch_pack_void: empty value pack");
    static_assert(((Vs == DefaultValue) || ...),
        "enum_dispatch_pack_void: DefaultValue must appear in the value pack");
    detail_runtime_dispatch::enum_dispatch_search_void<
        T, DefaultValue, Vs...>(rt, f, std::forward<Args>(args)...);
}

template <class R, class T, T... Vs, class F, class... Args>
[[nodiscard, gnu::always_inline]]
R value_dispatch_smallest_ge(value_sequence<T, Vs...>,
                             T rt, F&& f, Args&&... args)
{
    static_assert(sizeof...(Vs) > 0u,
        "value_dispatch_smallest_ge: empty value pack");
    return detail_runtime_dispatch::value_dispatch_smallest_ge_search<
        R, T, Vs...>(rt, f, std::forward<Args>(args)...);
}

template <class R, class T, T DefaultValue, T... Vs, class F, class... Args>
[[nodiscard, gnu::always_inline]]
R value_dispatch_exact_with_default(value_sequence<T, Vs...>,
                                    T rt, F&& f, Args&&... args)
{
    return enum_dispatch_pack<R, T, DefaultValue, Vs...>(
        value_sequence<T, Vs...>{}, rt, std::forward<F>(f),
        std::forward<Args>(args)...);
}

} // namespace

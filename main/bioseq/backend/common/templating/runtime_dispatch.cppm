/**
 * @file runtime_dispatch.cppm
 * @brief Inline runtime-to-compile-time dispatch primitives.
 *
 * Type dispatch walks a `type_list` until a probe matches. Value dispatch
 * selects an exact compile-time value, a declared default, or the smallest
 * declared value that is not less than a runtime input.
 */
module;

#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

export module bioseq.common.templating.runtime_dispatch;

import bioseq.common.templating.value_sequence;

#ifndef BIOSEQ_TEMPLATE_DISPATCH_FORCE_INLINE
#define BIOSEQ_TEMPLATE_DISPATCH_FORCE_INLINE 1
#endif

#if BIOSEQ_TEMPLATE_DISPATCH_FORCE_INLINE
#define BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR [[gnu::always_inline]]
#define BIOSEQ_TEMPLATE_DISPATCH_NODISCARD_INLINE_ATTR \
    [[nodiscard, gnu::always_inline]]
#else
#define BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
#define BIOSEQ_TEMPLATE_DISPATCH_NODISCARD_INLINE_ATTR [[nodiscard]]
#endif

export namespace bioseq::common::templating {

/**
 * @brief Carries a pack of types for runtime dispatch.
 * @tparam element_types Types in dispatch order.
 */
template <class... element_types>
struct type_list {
    /// Number of types in the list.
    static constexpr std::size_t size = sizeof...(element_types);
};

namespace detail_runtime_dispatch {

/** Walks a non-empty type pack and invokes the handler for the first match. */
template <class index_type, class handler_type, class probe_type,
          class head_type, class... remaining_types>
BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
constexpr void dispatch_first_matching_type_impl(
    handler_type& handler,
    probe_type& probe,
    index_type cursor)
{
    if (probe.template is_match<head_type>(cursor)) {
        handler.template operator()<head_type>();
        return;
    }
    if constexpr (sizeof...(remaining_types) > 0u) {
        dispatch_first_matching_type_impl<
            index_type,
            handler_type,
            probe_type,
            remaining_types...>(
                handler,
                probe,
                static_cast<index_type>(cursor + 1));
    } else {
        probe.on_no_match(cursor);
    }
}

/** Walks a non-empty type pack and returns the first extracted value. */
template <class index_type, class result_type, class probe_type,
          class head_type, class... remaining_types>
BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
constexpr std::optional<result_type> find_first_matching_type_impl(
    probe_type& probe,
    index_type cursor)
{
    if (auto result = probe.template extract<head_type>(cursor);
        result.has_value()) {
        return result;
    }
    if constexpr (sizeof...(remaining_types) > 0u) {
        return find_first_matching_type_impl<
            index_type,
            result_type,
            probe_type,
            remaining_types...>(
                probe,
                static_cast<index_type>(cursor + 1));
    } else {
        return std::nullopt;
    }
}

/** Selects an exact value or a declared default from a non-empty value pack. */
template <class result_type, class value_type,
          value_type default_value,
          value_type head_value,
          value_type... remaining_values,
          class function_type,
          class... argument_types>
BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
result_type dispatch_exact_value_or_default_impl(
    value_type runtime_value,
    function_type& function,
    argument_types&&... arguments)
{
    if (runtime_value == head_value) {
        return function(
            std::integral_constant<value_type, head_value>{},
            std::forward<argument_types>(arguments)...);
    }
    if constexpr (sizeof...(remaining_values) > 0u) {
        return dispatch_exact_value_or_default_impl<
            result_type,
            value_type,
            default_value,
            remaining_values...>(
                runtime_value,
                function,
                std::forward<argument_types>(arguments)...);
    } else {
        return function(
            std::integral_constant<value_type, default_value>{},
            std::forward<argument_types>(arguments)...);
    }
}

/** Selects the first value not less than the input from an ordered value pack. */
template <class result_type, class value_type,
          value_type head_value,
          value_type... remaining_values,
          class function_type,
          class... argument_types>
BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
result_type dispatch_smallest_not_less_impl(
    value_type runtime_value,
    function_type& function,
    argument_types&&... arguments)
{
    if (head_value >= runtime_value) {
        return function(
            std::integral_constant<value_type, head_value>{},
            std::forward<argument_types>(arguments)...);
    }
    if constexpr (sizeof...(remaining_values) > 0u) {
        return dispatch_smallest_not_less_impl<
            result_type,
            value_type,
            remaining_values...>(
                runtime_value,
                function,
                std::forward<argument_types>(arguments)...);
    } else {
        return function(
            std::integral_constant<value_type, head_value>{},
            std::forward<argument_types>(arguments)...);
    }
}

} // namespace detail_runtime_dispatch

/**
 * @brief Invokes a handler for the first type accepted by a probe.
 * @tparam index_type Cursor type supplied to the probe.
 * @tparam type_pack `type_list` specialization in search order.
 * @tparam handler_type Templated handler type.
 * @tparam probe_type Probe type exposing `is_match` and `on_no_match`.
 * @param handler Handler invoked as `operator()<matched_type>()`.
 * @param probe Match policy receiving the zero-based cursor.
 */
template <class index_type, class type_pack,
          class handler_type, class probe_type>
BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
constexpr void dispatch_first_matching_type(
    handler_type&& handler,
    probe_type&& probe)
{
    [&]<class... element_types>(type_list<element_types...>)
        BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
    {
        if constexpr (sizeof...(element_types) > 0u) {
            detail_runtime_dispatch::dispatch_first_matching_type_impl<
                index_type,
                std::remove_reference_t<handler_type>,
                std::remove_reference_t<probe_type>,
                element_types...>(
                    handler,
                    probe,
                    static_cast<index_type>(0));
        } else {
            probe.on_no_match(index_type{0});
        }
    }(type_pack{});
}

/**
 * @brief Extracts the first value produced by a type probe.
 * @tparam index_type Cursor type supplied to the probe.
 * @tparam result_type Extracted value type.
 * @tparam type_pack `type_list` specialization in search order.
 * @tparam probe_type Probe type exposing `extract`.
 * @param probe Extraction policy receiving the zero-based cursor.
 * @return The first extracted value, or `std::nullopt` when none matches.
 */
template <class index_type, class result_type,
          class type_pack, class probe_type>
BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
constexpr std::optional<result_type> find_first_matching_type(probe_type&& probe)
{
    return [&]<class... element_types>(type_list<element_types...>)
        BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR -> std::optional<result_type>
    {
        if constexpr (sizeof...(element_types) > 0u) {
            return detail_runtime_dispatch::find_first_matching_type_impl<
                index_type,
                result_type,
                std::remove_reference_t<probe_type>,
                element_types...>(
                    probe,
                    static_cast<index_type>(0));
        } else {
            return std::nullopt;
        }
    }(type_pack{});
}

/**
 * @brief Dispatches an exact runtime value or a declared compile-time default.
 * @tparam result_type Callable result type.
 * @tparam value_type Dispatched value type.
 * @tparam default_value Fallback value, which must occur in `values`.
 * @tparam values Candidate values in probe order.
 * @param runtime_value Runtime value to match.
 * @param function Callable receiving the selected integral constant.
 * @param arguments Additional arguments forwarded to the callable.
 * @return Callable result for the selected value.
 */
template <class result_type, class value_type,
          value_type default_value,
          value_type... values,
          class function_type,
          class... argument_types>
BIOSEQ_TEMPLATE_DISPATCH_NODISCARD_INLINE_ATTR
result_type dispatch_exact_value_or_default(
    value_sequence<value_type, values...>,
    value_type runtime_value,
    function_type&& function,
    argument_types&&... arguments)
{
    static_assert(
        sizeof...(values) > 0u,
        "dispatch_exact_value_or_default: empty value pack");
    static_assert(
        ((values == default_value) || ...),
        "dispatch_exact_value_or_default: default value is absent");
    return detail_runtime_dispatch::dispatch_exact_value_or_default_impl<
        result_type,
        value_type,
        default_value,
        values...>(
            runtime_value,
            function,
            std::forward<argument_types>(arguments)...);
}

/**
 * @brief Void-returning exact runtime-value dispatch with a default.
 * @tparam value_type Dispatched value type.
 * @tparam default_value Fallback value, which must occur in `values`.
 * @tparam values Candidate values in probe order.
 * @param runtime_value Runtime value to match.
 * @param function Callable receiving the selected integral constant.
 * @param arguments Additional arguments forwarded to the callable.
 */
template <class value_type,
          value_type default_value,
          value_type... values,
          class function_type,
          class... argument_types>
BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
void dispatch_exact_value_or_default_void(
    value_sequence<value_type, values...>,
    value_type runtime_value,
    function_type&& function,
    argument_types&&... arguments)
{
    static_assert(
        sizeof...(values) > 0u,
        "dispatch_exact_value_or_default_void: empty value pack");
    static_assert(
        ((values == default_value) || ...),
        "dispatch_exact_value_or_default_void: default value is absent");
    detail_runtime_dispatch::dispatch_exact_value_or_default_impl<
        void,
        value_type,
        default_value,
        values...>(
            runtime_value,
            function,
            std::forward<argument_types>(arguments)...);
}

/**
 * @brief Dispatches the smallest ordered value not less than a runtime input.
 * @tparam result_type Callable result type.
 * @tparam value_type Dispatched value type.
 * @tparam values Ordered candidate values.
 * @param runtime_value Runtime lower bound.
 * @param function Callable receiving the selected integral constant.
 * @param arguments Additional arguments forwarded to the callable.
 * @return Callable result for the selected value. Inputs above the final value
 *         dispatch to that final value.
 */
template <class result_type, class value_type,
          value_type... values,
          class function_type,
          class... argument_types>
BIOSEQ_TEMPLATE_DISPATCH_NODISCARD_INLINE_ATTR
result_type dispatch_smallest_not_less(
    value_sequence<value_type, values...>,
    value_type runtime_value,
    function_type&& function,
    argument_types&&... arguments)
{
    static_assert(
        sizeof...(values) > 0u,
        "dispatch_smallest_not_less: empty value pack");
    return detail_runtime_dispatch::dispatch_smallest_not_less_impl<
        result_type,
        value_type,
        values...>(
            runtime_value,
            function,
            std::forward<argument_types>(arguments)...);
}

} // namespace bioseq::common::templating

#undef BIOSEQ_TEMPLATE_DISPATCH_NODISCARD_INLINE_ATTR
#undef BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR

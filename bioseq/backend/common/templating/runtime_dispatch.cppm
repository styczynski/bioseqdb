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
 * @tparam p_element_types Types in dispatch order.
 */
template <class... p_element_types>
struct type_list {
    /// Number of types in the list.
    static constexpr std::size_t size = sizeof...(p_element_types);
};

namespace detail_runtime_dispatch {

/** Walks a non-empty type pack and invokes the handler for the first match. */
template <class p_index_type, class p_handler_type, class p_probe_type,
          class p_head_type, class... p_remaining_types>
BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
constexpr void dispatch_first_matching_type_impl(
    p_handler_type& handler,
    p_probe_type& probe,
    p_index_type cursor)
{
    if (probe.template is_match<p_head_type>(cursor)) {
        handler.template operator()<p_head_type>();
        return;
    }
    if constexpr (sizeof...(p_remaining_types) > 0u) {
        dispatch_first_matching_type_impl<
            p_index_type,
            p_handler_type,
            p_probe_type,
            p_remaining_types...>(
                handler,
                probe,
                static_cast<p_index_type>(cursor + 1));
    } else {
        probe.on_no_match(cursor);
    }
}

/** Walks a non-empty type pack and returns the first extracted value. */
template <class p_index_type, class p_result_type, class p_probe_type,
          class p_head_type, class... p_remaining_types>
BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
constexpr std::optional<p_result_type> find_first_matching_type_impl(
    p_probe_type& probe,
    p_index_type cursor)
{
    if (auto result = probe.template extract<p_head_type>(cursor);
        result.has_value()) {
        return result;
    }
    if constexpr (sizeof...(p_remaining_types) > 0u) {
        return find_first_matching_type_impl<
            p_index_type,
            p_result_type,
            p_probe_type,
            p_remaining_types...>(
                probe,
                static_cast<p_index_type>(cursor + 1));
    } else {
        return std::nullopt;
    }
}

/** Selects an exact value or a declared default from a non-empty value pack. */
template <class p_result_type, class p_value_type,
          p_value_type p_default_value,
          p_value_type p_head_value,
          p_value_type... p_remaining_values,
          class p_function_type,
          class... p_argument_types>
BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
p_result_type dispatch_exact_value_or_default_impl(
    p_value_type runtime_value,
    p_function_type& function,
    p_argument_types&&... arguments)
{
    if (runtime_value == p_head_value) {
        return function(
            std::integral_constant<p_value_type, p_head_value>{},
            std::forward<p_argument_types>(arguments)...);
    }
    if constexpr (sizeof...(p_remaining_values) > 0u) {
        return dispatch_exact_value_or_default_impl<
            p_result_type,
            p_value_type,
            p_default_value,
            p_remaining_values...>(
                runtime_value,
                function,
                std::forward<p_argument_types>(arguments)...);
    } else {
        return function(
            std::integral_constant<p_value_type, p_default_value>{},
            std::forward<p_argument_types>(arguments)...);
    }
}

/** Selects the first value not less than the input from an ordered value pack. */
template <class p_result_type, class p_value_type,
          p_value_type p_head_value,
          p_value_type... p_remaining_values,
          class p_function_type,
          class... p_argument_types>
BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
p_result_type dispatch_smallest_not_less_impl(
    p_value_type runtime_value,
    p_function_type& function,
    p_argument_types&&... arguments)
{
    if (p_head_value >= runtime_value) {
        return function(
            std::integral_constant<p_value_type, p_head_value>{},
            std::forward<p_argument_types>(arguments)...);
    }
    if constexpr (sizeof...(p_remaining_values) > 0u) {
        return dispatch_smallest_not_less_impl<
            p_result_type,
            p_value_type,
            p_remaining_values...>(
                runtime_value,
                function,
                std::forward<p_argument_types>(arguments)...);
    } else {
        return function(
            std::integral_constant<p_value_type, p_head_value>{},
            std::forward<p_argument_types>(arguments)...);
    }
}

} // namespace detail_runtime_dispatch

/**
 * @brief Invokes a handler for the first type accepted by a probe.
 * @tparam p_index_type Cursor type supplied to the probe.
 * @tparam p_type_pack `type_list` specialization in search order.
 * @tparam p_handler_type Templated handler type.
 * @tparam p_probe_type Probe type exposing `is_match` and `on_no_match`.
 * @param handler Handler invoked as `operator()<matched_type>()`.
 * @param probe Match policy receiving the zero-based cursor.
 */
template <class p_index_type, class p_type_pack,
          class p_handler_type, class p_probe_type>
BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
constexpr void dispatch_first_matching_type(
    p_handler_type&& handler,
    p_probe_type&& probe)
{
    [&]<class... p_element_types>(type_list<p_element_types...>)
        BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
    {
        if constexpr (sizeof...(p_element_types) > 0u) {
            detail_runtime_dispatch::dispatch_first_matching_type_impl<
                p_index_type,
                std::remove_reference_t<p_handler_type>,
                std::remove_reference_t<p_probe_type>,
                p_element_types...>(
                    handler,
                    probe,
                    static_cast<p_index_type>(0));
        } else {
            probe.on_no_match(p_index_type{0});
        }
    }(p_type_pack{});
}

/**
 * @brief Extracts the first value produced by a type probe.
 * @tparam p_index_type Cursor type supplied to the probe.
 * @tparam p_result_type Extracted value type.
 * @tparam p_type_pack `type_list` specialization in search order.
 * @tparam p_probe_type Probe type exposing `extract`.
 * @param probe Extraction policy receiving the zero-based cursor.
 * @return The first extracted value, or `std::nullopt` when none matches.
 */
template <class p_index_type, class p_result_type,
          class p_type_pack, class p_probe_type>
BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
constexpr std::optional<p_result_type> find_first_matching_type(
    p_probe_type&& probe)
{
    return [&]<class... p_element_types>(type_list<p_element_types...>)
        BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR -> std::optional<p_result_type>
    {
        if constexpr (sizeof...(p_element_types) > 0u) {
            return detail_runtime_dispatch::find_first_matching_type_impl<
                p_index_type,
                p_result_type,
                std::remove_reference_t<p_probe_type>,
                p_element_types...>(
                    probe,
                    static_cast<p_index_type>(0));
        } else {
            return std::nullopt;
        }
    }(p_type_pack{});
}

/**
 * @brief Dispatches an exact runtime value or a declared compile-time default.
 * @tparam p_result_type Callable result type.
 * @tparam p_value_type Dispatched value type.
 * @tparam p_default_value Fallback value, which must occur in `p_values`.
 * @tparam p_values Candidate values in probe order.
 * @param runtime_value Runtime value to match.
 * @param function Callable receiving the selected integral constant.
 * @param arguments Additional arguments forwarded to the callable.
 * @return Callable result for the selected value.
 */
template <class p_result_type, class p_value_type,
          p_value_type p_default_value,
          p_value_type... p_values,
          class p_function_type,
          class... p_argument_types>
BIOSEQ_TEMPLATE_DISPATCH_NODISCARD_INLINE_ATTR
p_result_type dispatch_exact_value_or_default(
    value_sequence<p_value_type, p_values...>,
    p_value_type runtime_value,
    p_function_type&& function,
    p_argument_types&&... arguments)
{
    static_assert(
        sizeof...(p_values) > 0u,
        "dispatch_exact_value_or_default: empty value pack");
    static_assert(
        ((p_values == p_default_value) || ...),
        "dispatch_exact_value_or_default: default value is absent");
    return detail_runtime_dispatch::dispatch_exact_value_or_default_impl<
        p_result_type,
        p_value_type,
        p_default_value,
        p_values...>(
            runtime_value,
            function,
            std::forward<p_argument_types>(arguments)...);
}

/**
 * @brief Void-returning exact runtime-value dispatch with a default.
 * @tparam p_value_type Dispatched value type.
 * @tparam p_default_value Fallback value, which must occur in `p_values`.
 * @tparam p_values Candidate values in probe order.
 * @param runtime_value Runtime value to match.
 * @param function Callable receiving the selected integral constant.
 * @param arguments Additional arguments forwarded to the callable.
 */
template <class p_value_type,
          p_value_type p_default_value,
          p_value_type... p_values,
          class p_function_type,
          class... p_argument_types>
BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
void dispatch_exact_value_or_default_void(
    value_sequence<p_value_type, p_values...>,
    p_value_type runtime_value,
    p_function_type&& function,
    p_argument_types&&... arguments)
{
    static_assert(
        sizeof...(p_values) > 0u,
        "dispatch_exact_value_or_default_void: empty value pack");
    static_assert(
        ((p_values == p_default_value) || ...),
        "dispatch_exact_value_or_default_void: default value is absent");
    detail_runtime_dispatch::dispatch_exact_value_or_default_impl<
        void,
        p_value_type,
        p_default_value,
        p_values...>(
            runtime_value,
            function,
            std::forward<p_argument_types>(arguments)...);
}

/**
 * @brief Dispatches the smallest ordered value not less than a runtime input.
 * @tparam p_result_type Callable result type.
 * @tparam p_value_type Dispatched value type.
 * @tparam p_values Ordered candidate values.
 * @param runtime_value Runtime lower bound.
 * @param function Callable receiving the selected integral constant.
 * @param arguments Additional arguments forwarded to the callable.
 * @return Callable result for the selected value. Inputs above the final value
 *         dispatch to that final value.
 */
template <class p_result_type, class p_value_type,
          p_value_type... p_values,
          class p_function_type,
          class... p_argument_types>
BIOSEQ_TEMPLATE_DISPATCH_NODISCARD_INLINE_ATTR
p_result_type dispatch_smallest_not_less(
    value_sequence<p_value_type, p_values...>,
    p_value_type runtime_value,
    p_function_type&& function,
    p_argument_types&&... arguments)
{
    static_assert(
        sizeof...(p_values) > 0u,
        "dispatch_smallest_not_less: empty value pack");
    return detail_runtime_dispatch::dispatch_smallest_not_less_impl<
        p_result_type,
        p_value_type,
        p_values...>(
            runtime_value,
            function,
            std::forward<p_argument_types>(arguments)...);
}

} // namespace bioseq::common::templating

#undef BIOSEQ_TEMPLATE_DISPATCH_NODISCARD_INLINE_ATTR
#undef BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR

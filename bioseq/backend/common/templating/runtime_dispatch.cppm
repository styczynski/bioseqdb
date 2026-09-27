/**
 * @file runtime_dispatch.cppm
 * @brief Chooses a template specialization using runtime values.
 *
 * Type selection tests candidates in order. Value selection can match an
 * exact value or choose a size bucket, so callers can use precompiled
 * specializations for runtime input.
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
 * @brief Lists the types to test, in dispatch order.
 */
template <class... p_element_types>
struct type_list {
    /// Number of types in the list.
    static constexpr std::size_t size = sizeof...(p_element_types);
};

namespace detail_runtime_dispatch {

/**
 * @brief Tests types in order and calls the handler for the first match.
 *
 * `probe.matches<T>(cursor)` decides which type to use. If none matches,
 * `probe.on_no_match` receives the final index. The type pack must be non-empty.
 */
template <class p_index_type, class p_handler_type, class p_probe_type,
          class p_head_type, class... p_remaining_types>
BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
constexpr void dispatch_first_matching_type_impl(
    p_handler_type& handler,
    p_probe_type& probe,
    p_index_type cursor)
{
    if (probe.template matches<p_head_type>(cursor)) {
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

/**
 * @brief Returns the first value produced by `probe.extract<T>(cursor)`.
 *
 * The type pack must be non-empty. If every extraction returns an empty
 * optional, this returns `std::nullopt`.
 */
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

/**
 * @brief Calls the function with the matching compile-time value or the default.
 *
 * The selected value is passed as an integral constant, followed by the
 * forwarded arguments. The candidate pack must be non-empty.
 */
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

/**
 * @brief Selects the first bucket large enough for the runtime value.
 *
 * Candidates must be ordered and non-empty. Inputs above every candidate
 * select the last bucket. The function receives an integral constant followed
 * by the forwarded arguments.
 */
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
 * @brief Calls a handler for the first type accepted by the probe.
 * @param[in,out] handler Called as `operator()<matched_type>()`.
 * @param[in,out] probe Supplies `matches<T>(index)` and `on_no_match(index)`.
 *
 * Types are tested in `p_type_pack` order with zero-based indices. When no
 * type matches, the probe receives the final index, or zero for an empty list.
 */
template <class p_index_type, class p_type_pack,
          class p_handler_type, class p_probe_type>
BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
constexpr void dispatch_first_matching_type(
    p_handler_type&& handler,
    p_probe_type&& probe)
{
    [&handler, &probe]<class... p_element_types>(type_list<p_element_types...>)
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
 * @brief Returns the first value extracted from the listed types.
 * @param[in,out] probe Supplies `extract<T>(index)`, returning an optional.
 *
 * Types are tested in `p_type_pack` order with zero-based indices. This returns
 * `std::nullopt` for an empty list or when no extraction succeeds.
 */
template <class p_index_type, class p_result_type,
          class p_type_pack, class p_probe_type>
BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
constexpr std::optional<p_result_type> find_first_matching_type(
    p_probe_type&& probe)
{
    return [&probe]<class... p_element_types>(type_list<p_element_types...>)
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
 * @brief Calls the function with an exact template value or the default.
 * @param[in] values Non-empty list of candidate values.
 * @param[in] runtime_value Value to match.
 * @param[in,out] function Receives an integral constant and forwarded arguments.
 * @param[in,out] arguments Additional arguments for the function.
 * @tparam p_default_value Fallback value, which must appear in `values`.
 */
template <class p_result_type, class p_value_type,
          p_value_type p_default_value,
          p_value_type... p_values,
          class p_function_type,
          class... p_argument_types>
BIOSEQ_TEMPLATE_DISPATCH_NODISCARD_INLINE_ATTR
p_result_type dispatch_exact_value_or_default(
    value_sequence<p_value_type, p_values...> values,
    p_value_type runtime_value,
    p_function_type&& function,
    p_argument_types&&... arguments)
{
    static_cast<void>(values);
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
 * @brief Calls a void-returning function with an exact value or the default.
 * @param[in] values Non-empty list containing `p_default_value`.
 * @param[in] runtime_value Value to match.
 * @param[in,out] function Receives an integral constant and forwarded arguments.
 * @param[in,out] arguments Additional arguments for the function.
 */
template <class p_value_type,
          p_value_type p_default_value,
          p_value_type... p_values,
          class p_function_type,
          class... p_argument_types>
BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
void dispatch_exact_value_or_default_void(
    value_sequence<p_value_type, p_values...> values,
    p_value_type runtime_value,
    p_function_type&& function,
    p_argument_types&&... arguments)
{
    static_cast<void>(values);
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
 * @brief Calls the function with the smallest bucket that fits the input.
 * @param[in] values Non-empty list of candidates in ascending order.
 * @param[in] runtime_value Value whose bucket is needed.
 * @param[in,out] function Receives an integral constant and forwarded arguments.
 * @param[in,out] arguments Additional arguments for the function.
 *
 * Inputs above the largest bucket select that bucket. The caller must handle
 * any resulting capacity limit.
 */
template <class p_result_type, class p_value_type,
          p_value_type... p_values,
          class p_function_type,
          class... p_argument_types>
BIOSEQ_TEMPLATE_DISPATCH_NODISCARD_INLINE_ATTR
p_result_type dispatch_smallest_not_less(
    value_sequence<p_value_type, p_values...> values,
    p_value_type runtime_value,
    p_function_type&& function,
    p_argument_types&&... arguments)
{
    static_cast<void>(values);
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

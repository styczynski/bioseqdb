/**
 * @file dispatch_chain.cppm
 * @brief Chooses template arguments from runtime settings, one step at a time.
 *
 * Each step selects a value and passes it to the remaining steps. Once all
 * values are selected, the chain calls the matching leaf template. An inactive
 * step contributes its default value.
 */
module;

#include <cstddef>
#include <type_traits>
#include <utility>

export module bioseq.common.templating.dispatch_chain;

import bioseq.common.templating.runtime_dispatch;

#ifndef BIOSEQ_TEMPLATE_DISPATCH_FORCE_INLINE
#define BIOSEQ_TEMPLATE_DISPATCH_FORCE_INLINE 1
#endif

#if BIOSEQ_TEMPLATE_DISPATCH_FORCE_INLINE
#define BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR [[gnu::always_inline]]
#else
#define BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
#endif

export namespace bioseq::common::templating {

namespace detail_dispatch_chain {

/** @brief Reports whether every forwarded chain argument is a reference type. */
template <class... p_argument_types>
concept chain_arguments_are_references =
    (std::is_reference_v<p_argument_types> && ...);

/**
 * @brief Passes a selected integral constant to the next dispatch step.
 */
template <class p_result_type,
          class p_parameter_type,
          class p_continuation_type,
          class p_target_type>
struct continuation_adapter {
    /// Next stage in the dispatch chain.
    p_continuation_type continuation;

    /**
     * @brief Runs the remaining steps with this selected value.
     *
     * The shared parameters and argument references are passed to the final call.
     */
    template <p_target_type p_value, class... p_argument_types>
    BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
    p_result_type operator()(
        std::integral_constant<p_target_type, p_value>,
        const p_parameter_type& parameters,
        p_argument_types&&... arguments) const
    {
        return continuation.template run<p_value>(
            parameters,
            std::forward<p_argument_types>(arguments)...);
    }
};

} // namespace detail_dispatch_chain

/**
 * @brief Selects template arguments in order, then calls the leaf.
 * @tparam p_leaf_type Template whose `call` receives the selected values.
 * @tparam p_step_types Steps in selection order.
 */
template <class p_result_type,
          template <auto... p_leaf_values> class p_leaf_type,
          class p_parameter_type,
          class... p_step_types>
struct dispatch_chain;

/// End of the chain, where the selected leaf is called.
template <class p_result_type,
          template <auto... p_leaf_values> class p_leaf_type,
          class p_parameter_type>
struct dispatch_chain<p_result_type, p_leaf_type, p_parameter_type> {
    /**
     * @brief Calls the leaf with every selected template value.
     *
     * Additional arguments must be references and are forwarded to the leaf.
     */
    template <auto... p_pinned_values, class... p_argument_types>
        requires detail_dispatch_chain::chain_arguments_are_references<
            p_argument_types...>
    BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
    static p_result_type run(
        const p_parameter_type& parameters,
        p_argument_types&&... arguments)
    {
        return p_leaf_type<p_pinned_values...>::call(
            parameters,
            std::forward<p_argument_types>(arguments)...);
    }
};

/// Selects one template value before running the remaining steps.
template <class p_result_type,
          template <auto... p_leaf_values> class p_leaf_type,
          class p_parameter_type,
          class p_head_step,
          class... p_remaining_steps>
struct dispatch_chain<
    p_result_type,
    p_leaf_type,
    p_parameter_type,
    p_head_step,
    p_remaining_steps...> {
    /**
     * @brief Keeps the values selected by earlier steps.
     */
    template <auto... p_pinned_values>
    struct continuation {
        /**
         * @brief Adds this value to the selected arguments and continues.
         *
         * Parameters and argument references pass through to the final call.
         */
        template <auto p_value, class... p_argument_types>
        BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
        p_result_type run(
            const p_parameter_type& parameters,
            p_argument_types&&... arguments) const
        {
            return dispatch_chain<
                p_result_type,
                p_leaf_type,
                p_parameter_type,
                p_remaining_steps...>::template run<
                    p_pinned_values...,
                    p_value>(
                    parameters,
                    std::forward<p_argument_types>(arguments)...);
        }
    };

    /**
     * @brief Runs this step, or uses its default when the step is inactive.
     *
     * The selected value becomes the next template argument. Additional arguments
     * must be references and are forwarded through the chain.
     */
    template <auto... p_pinned_values, class... p_argument_types>
        requires detail_dispatch_chain::chain_arguments_are_references<
            p_argument_types...>
    BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
    static p_result_type run(
        const p_parameter_type& parameters,
        p_argument_types&&... arguments)
    {
        if constexpr (
            p_head_step::template active_v<p_pinned_values...>) {
            return p_head_step::template dispatch<
                p_result_type,
                p_parameter_type,
                p_pinned_values...>(
                    parameters,
                    continuation<p_pinned_values...>{},
                    std::forward<p_argument_types>(arguments)...);
        } else {
            return dispatch_chain<
                p_result_type,
                p_leaf_type,
                p_parameter_type,
                p_remaining_steps...>::template run<
                    p_pinned_values...,
                    p_head_step::default_value>(
                        parameters,
                        std::forward<p_argument_types>(arguments)...);
        }
    }
};

/**
 * @brief Selects a registered enum value, using the default for unknown values.
 * @tparam p_registry_type Provides `target_t`, `value_list`, `default_value`,
 *         and `read_field`.
 */
template <class p_registry_type>
struct enum_step {
    /// Type selected by this step.
    using target_t = typename p_registry_type::target_t;

    /// Value selected when the runtime input is not registered.
    static constexpr target_t default_value =
        p_registry_type::default_value;

    /// This step is enabled regardless of earlier selections.
    template <auto... p_prior_values>
    static constexpr bool active_v = true;

    /**
     * @brief Reads the enum setting and continues with its selected value.
     *
     * Unknown values use the registry default. Argument references pass through
     * to the next step.
     */
    template <class p_result_type,
              class p_parameter_type,
              auto... p_pinned_values,
              class p_continuation_type,
              class... p_argument_types>
    BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
    static p_result_type dispatch(
        const p_parameter_type& parameters,
        p_continuation_type continuation,
        p_argument_types&&... arguments)
    {
        using continuation_adapter_type = detail_dispatch_chain::continuation_adapter<
            p_result_type,
            p_parameter_type,
            p_continuation_type,
            target_t>;
        continuation_adapter_type adapter{continuation};
        if constexpr (std::is_same_v<p_result_type, void>) {
            dispatch_exact_value_or_default_void<target_t, default_value>(
                typename p_registry_type::value_list{},
                p_registry_type::read_field(parameters, arguments...),
                adapter,
                parameters,
                std::forward<p_argument_types>(arguments)...);
            return;
        } else {
            return dispatch_exact_value_or_default<
                p_result_type,
                target_t,
                default_value>(
                    typename p_registry_type::value_list{},
                    p_registry_type::read_field(parameters, arguments...),
                    adapter,
                    parameters,
                    std::forward<p_argument_types>(arguments)...);
        }
    }
};

/**
 * @brief Selects the first bucket at least as large as the runtime value.
 *
 * The registry must provide ordered `value_list` entries, `default_value`,
 * and `read_field`. Values above the last bucket select that last bucket.
 */
template <class p_registry_type>
struct bucket_step {
    /// Type selected by this step.
    using target_t = std::size_t;

    /// Value selected when a condition skips this step.
    static constexpr target_t default_value =
        p_registry_type::default_value;

    /// This step is enabled regardless of earlier selections.
    template <auto... p_prior_values>
    static constexpr bool active_v = true;

    /**
     * @brief Reads the runtime value and continues with the selected bucket.
     *
     * The last bucket also handles values larger than every registered bucket.
     */
    template <class p_result_type,
              class p_parameter_type,
              auto... p_pinned_values,
              class p_continuation_type,
              class... p_argument_types>
    BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
    static p_result_type dispatch(
        const p_parameter_type& parameters,
        p_continuation_type continuation,
        p_argument_types&&... arguments)
    {
        using continuation_adapter_type = detail_dispatch_chain::continuation_adapter<
            p_result_type,
            p_parameter_type,
            p_continuation_type,
            target_t>;
        continuation_adapter_type adapter{continuation};
        return dispatch_smallest_not_less<p_result_type, target_t>(
            typename p_registry_type::value_list{},
            p_registry_type::read_field(parameters, arguments...),
            adapter,
            parameters,
            std::forward<p_argument_types>(arguments)...);
    }
};

/**
 * @brief Selects a bucket using values chosen by earlier steps.
 * @tparam p_registry_type Provides `value_list_for`, `read_field_for`,
 *         and `default_value`.
 *
 * Place this step after the selections it needs. Both registry templates
 * receive those values. Bucket lists must be ordered, and inputs above the
 * last bucket select that bucket.
 */
template <class p_registry_type>
struct dependent_bucket_step {
    /// Type selected by this step.
    using target_t = std::size_t;

    /// Value selected when a condition skips this step.
    static constexpr target_t default_value =
        p_registry_type::default_value;

    /// This step is enabled regardless of earlier selections.
    template <auto... p_prior_values>
    static constexpr bool active_v = true;

    /**
     * @brief Selects a bucket from the list chosen by earlier steps.
     *
     * The registry reads the runtime value using the same earlier selections.
     * The chosen bucket and argument references pass to the next step.
     */
    template <class p_result_type,
              class p_parameter_type,
              auto... p_pinned_values,
              class p_continuation_type,
              class... p_argument_types>
    BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
    static p_result_type dispatch(
        const p_parameter_type& parameters,
        p_continuation_type continuation,
        p_argument_types&&... arguments)
    {
        using continuation_adapter_type = detail_dispatch_chain::continuation_adapter<
            p_result_type,
            p_parameter_type,
            p_continuation_type,
            target_t>;
        continuation_adapter_type adapter{continuation};
        return dispatch_smallest_not_less<p_result_type, target_t>(
            typename p_registry_type::template value_list_for<
                p_pinned_values...>{},
            p_registry_type::template read_field_for<p_pinned_values...>(
                parameters,
                arguments...),
            adapter,
            parameters,
            std::forward<p_argument_types>(arguments)...);
    }
};

/**
 * @brief Enables a dispatch step only when a compile-time condition is true.
 *
 * The chain uses the inner step's default when the condition is false.
 */
template <class p_inner_step, bool p_active_condition>
struct guarded_step {
    /// Value type selected by the wrapped step.
    using target_t = typename p_inner_step::target_t;

    /// Value selected when this step is disabled.
    static constexpr target_t default_value = p_inner_step::default_value;

    /// Whether `dispatch_chain` should run this step.
    template <auto... p_prior_values>
    static constexpr bool active_v = p_active_condition;

    /**
     * @brief Runs the wrapped step.
     *
     * `dispatch_chain` calls this only when the step is enabled.
     */
    template <class p_result_type,
              class p_parameter_type,
              auto... p_pinned_values,
              class p_continuation_type,
              class... p_argument_types>
    BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR
    static p_result_type dispatch(
        const p_parameter_type& parameters,
        p_continuation_type continuation,
        p_argument_types&&... arguments)
    {
        return p_inner_step::template dispatch<
            p_result_type,
            p_parameter_type,
            p_pinned_values...>(
                parameters,
                continuation,
                std::forward<p_argument_types>(arguments)...);
    }
};

} // namespace bioseq::common::templating

#undef BIOSEQ_TEMPLATE_DISPATCH_INLINE_ATTR

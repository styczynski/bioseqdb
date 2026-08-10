/**
 * @file dispatch_chain.cppm
 * @brief Composable runtime-to-template dispatch steps.
 *
 * A chain pins one compile-time value per step before invoking a leaf. Each
 * step supplies a target type, a default value, an activation predicate, and a
 * dispatch function that invokes the continuation with its selected value.
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

/** Reports whether every forwarded chain argument is a reference type. */
template <class... p_argument_types>
concept chain_arguments_are_references =
    (std::is_reference_v<p_argument_types> && ...);

/**
 * @brief Adapts an integral-constant dispatch callback to a chain continuation.
 */
template <class p_result_type,
          class p_parameter_type,
          class p_continuation_type,
          class p_target_type>
struct continuation_adapter {
    /// Next stage in the dispatch chain.
    p_continuation_type continuation;

    /** Forwards one selected compile-time value into the continuation. */
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
 * @brief Pins step values in order and invokes a templated leaf.
 * @tparam p_result_type Leaf result type.
 * @tparam p_leaf_type Leaf template exposing `call`.
 * @tparam p_parameter_type Shared runtime-parameter type.
 * @tparam p_step_types Ordered dispatch-step types.
 */
template <class p_result_type,
          template <auto... p_leaf_values> class p_leaf_type,
          class p_parameter_type,
          class... p_step_types>
struct dispatch_chain;

/// Terminal `dispatch_chain` specialization that invokes the leaf.
template <class p_result_type,
          template <auto... p_leaf_values> class p_leaf_type,
          class p_parameter_type>
struct dispatch_chain<p_result_type, p_leaf_type, p_parameter_type> {
    /**
     * @brief Invokes the leaf with all previously selected values.
     * @tparam p_pinned_values Values selected by preceding steps.
     * @param parameters Shared runtime parameters.
     * @param arguments Reference arguments forwarded to the leaf.
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

/// Recursive `dispatch_chain` specialization that selects the next value.
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
    /** Carries previously selected values into the remainder of the chain. */
    template <auto... p_pinned_values>
    struct continuation {
        /** Selects one value and resumes the remaining steps. */
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
     * @brief Selects the head-step value and resumes the chain.
     * @tparam p_pinned_values Values selected by preceding steps.
     * @param parameters Shared runtime parameters.
     * @param arguments Reference arguments forwarded through every step.
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
 * @brief Dispatch step selecting an exact enumeration value or its default.
 * @tparam p_registry_type Registry exposing `target_t`, `value_list`,
 *         `default_value`, and `read_field`.
 */
template <class p_registry_type>
struct enum_step {
    /// Type selected by this step.
    using target_t = typename p_registry_type::target_t;

    /// Value selected when the runtime input is not registered.
    static constexpr target_t default_value =
        p_registry_type::default_value;

    /// Enumeration dispatch is active for every preceding selection.
    template <auto... p_prior_values>
    static constexpr bool active_v = true;

    /**
     * @brief Dispatches the runtime field and resumes the chain.
     * @param parameters Shared runtime parameters.
     * @param continuation Next chain stage.
     * @param arguments Reference arguments forwarded through the chain.
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
        using adapter_type = detail_dispatch_chain::continuation_adapter<
            p_result_type,
            p_parameter_type,
            p_continuation_type,
            target_t>;
        adapter_type adapter{continuation};
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
 * @brief Dispatch step selecting the first registered bucket not below a field.
 * @tparam p_registry_type Registry exposing `value_list`, `default_value`, and
 *         `read_field`.
 */
template <class p_registry_type>
struct bucket_step {
    /// Type selected by this step.
    using target_t = std::size_t;

    /// Value selected when the step is inactive in a guarded chain.
    static constexpr target_t default_value =
        p_registry_type::default_value;

    /// Bucket dispatch is active for every preceding selection.
    template <auto... p_prior_values>
    static constexpr bool active_v = true;

    /**
     * @brief Dispatches the runtime field and resumes the chain.
     * @param parameters Shared runtime parameters.
     * @param continuation Next chain stage.
     * @param arguments Reference arguments forwarded through the chain.
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
        using adapter_type = detail_dispatch_chain::continuation_adapter<
            p_result_type,
            p_parameter_type,
            p_continuation_type,
            target_t>;
        adapter_type adapter{continuation};
        return dispatch_smallest_not_less<p_result_type, target_t>(
            typename p_registry_type::value_list{},
            p_registry_type::read_field(parameters, arguments...),
            adapter,
            parameters,
            std::forward<p_argument_types>(arguments)...);
    }
};

/**
 * @brief Bucket step whose candidates and field depend on prior selections.
 * @tparam p_registry_type Registry exposing `value_list_for`, `read_field_for`,
 *         and `default_value`.
 *
 * Place this step after the values on which its bucket set depends. The
 * selected values are forwarded to both registry templates.
 */
template <class p_registry_type>
struct dependent_bucket_step {
    /// Type selected by this step.
    using target_t = std::size_t;

    /// Value selected when the step is inactive in a guarded chain.
    static constexpr target_t default_value =
        p_registry_type::default_value;

    /// Dependent bucket dispatch is active for every preceding selection.
    template <auto... p_prior_values>
    static constexpr bool active_v = true;

    /**
     * @brief Dispatches against the bucket set for prior selected values.
     * @param parameters Shared runtime parameters.
     * @param continuation Next chain stage.
     * @param arguments Reference arguments forwarded through the chain.
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
        using adapter_type = detail_dispatch_chain::continuation_adapter<
            p_result_type,
            p_parameter_type,
            p_continuation_type,
            target_t>;
        adapter_type adapter{continuation};
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
 * @brief Applies a compile-time activation condition to another step.
 * @tparam p_inner_step Step used when active.
 * @tparam p_active_condition Whether the inner step participates in the chain.
 */
template <class p_inner_step, bool p_active_condition>
struct guarded_step {
    /// Type selected by the guarded step.
    using target_t = typename p_inner_step::target_t;

    /// Value pinned when the guarded step is inactive.
    static constexpr target_t default_value = p_inner_step::default_value;

    /// Compile-time activation predicate consumed by `dispatch_chain`.
    template <auto... p_prior_values>
    static constexpr bool active_v = p_active_condition;

    /**
     * @brief Delegates active dispatch to the wrapped step.
     * @param parameters Shared runtime parameters.
     * @param continuation Next chain stage.
     * @param arguments Reference arguments forwarded through the chain.
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

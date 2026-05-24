/**
 * @file dispatch_chain.cppm
 * @brief N-step generic dispatch chain (lifted from pipe.cppm).
 *
 * Each step has:
 *   - `target_t`         — the NTTP type the step pins
 *   - `default_value`    — pinned when active_v<Pinned...> is false
 *   - `active_v<...>`    — compile-time predicate
 *   - `dispatch<R, Params, Pinned..., K>(params, k, args...)` —
 *     runtime dispatch that invokes the continuation with the pinned
 *     value.
 *
 * The chain's only continuation type is
 * `dispatch_chain<R, Leaf, Params, Head, Tail...>::continuation<Pinned...>`
 * — a stateless POD with one templated member function. No `[&]`
 * lambdas anywhere; the chain and the steps are all pure compile-time
 * machinery.
 *
 * `Params` is a template parameter so the chain is reusable from any
 * pipeline. `pipe.cppm` keeps the per-pipeline aliases that pin
 * `Params = Minimap2Params`.
 */
module;

#include <type_traits>
#include <utility>

export module bioseq_common.templating.dispatch_chain;

import bioseq_common.templating.value_sequence;
import bioseq_common.templating.runtime_dispatch;

export namespace bioseq::common::templating {

namespace detail_dispatch_chain {

template <class... Args>
concept chain_args_are_references = (std::is_reference_v<Args> && ...);

} // namespace detail_dispatch_chain

template <class R, template <auto...> class Leaf, class Params, class... Steps>
struct dispatch_chain;

template <class R, template <auto...> class Leaf, class Params>
struct dispatch_chain<R, Leaf, Params> {
    template <auto... Pinned, class... Args>
        requires detail_dispatch_chain::chain_args_are_references<Args...>
    [[gnu::always_inline]] static R run(const Params& params, Args&&... args) {
        return Leaf<Pinned...>::call(params, std::forward<Args>(args)...);
    }
};

template <class R, template <auto...> class Leaf, class Params,
          class Head, class... Tail>
struct dispatch_chain<R, Leaf, Params, Head, Tail...> {
    template <auto... Pinned>
    struct continuation {
        template <auto V, class... Args>
        [[gnu::always_inline]] R run(const Params& params, Args&&... args) const {
            return dispatch_chain<R, Leaf, Params, Tail...>
                ::template run<Pinned..., V>(params, std::forward<Args>(args)...);
        }
    };

    template <auto... Pinned, class... Args>
        requires detail_dispatch_chain::chain_args_are_references<Args...>
    [[gnu::always_inline]] static R run(const Params& params, Args&&... args) {
        if constexpr (Head::template active_v<Pinned...>) {
            return Head::template dispatch<R, Params, Pinned...>(
                params, continuation<Pinned...>{}, std::forward<Args>(args)...);
        } else {
            return dispatch_chain<R, Leaf, Params, Tail...>::template run<
                Pinned..., Head::default_value>(
                    params, std::forward<Args>(args)...);
        }
    }
};

template <class Registry>
struct enum_step {
    using target_t = typename Registry::target_t;
    static constexpr target_t default_value = Registry::default_value;

    template <auto... /*Pinned*/>
    static constexpr bool active_v = true;

    template <class R, class Params, class Continuation>
    struct adapter {
        Continuation continuation;
        template <target_t V, class... A>
        [[gnu::always_inline]] R operator()(
            std::integral_constant<target_t, V>,
            const Params& params, A&&... forwarded_args) const
        {
            return continuation.template run<V>(
                params, std::forward<A>(forwarded_args)...);
        }
    };

    template <class R, class Params, auto... Pinned,
              class Continuation, class... Args>
    [[gnu::always_inline]] static R dispatch(
        const Params& params, Continuation continuation, Args&&... args)
    {
        adapter<R, Params, Continuation> step_adapter{continuation};
        if constexpr (std::is_same_v<R, void>) {
            enum_dispatch_pack_void<target_t, default_value>(
                typename Registry::value_list{},
                Registry::read_field(params, args...),
                step_adapter, params, std::forward<Args>(args)...);
            return;
        } else {
            return enum_dispatch_pack<R, target_t, default_value>(
                typename Registry::value_list{},
                Registry::read_field(params, args...),
                step_adapter, params, std::forward<Args>(args)...);
        }
    }
};

template <class Registry>
struct bucket_step {
    using target_t = std::size_t;
    static constexpr std::size_t default_value = Registry::default_value;

    template <auto... /*Pinned*/>
    static constexpr bool active_v = true;

    template <class R, class Params, class Continuation>
    struct adapter {
        Continuation continuation;
        template <std::size_t V, class... A>
        [[gnu::always_inline]] R operator()(
            std::integral_constant<std::size_t, V>,
            const Params& params, A&&... forwarded_args) const
        {
            return continuation.template run<V>(
                params, std::forward<A>(forwarded_args)...);
        }
    };

    template <class R, class Params, auto... Pinned,
              class Continuation, class... Args>
    [[gnu::always_inline]] static R dispatch(
        const Params& params, Continuation continuation, Args&&... args)
    {
        adapter<R, Params, Continuation> step_adapter{continuation};
        return value_dispatch_smallest_ge<R, std::size_t>(
            typename Registry::value_list{},
            Registry::read_field(params, args...),
            step_adapter, params, std::forward<Args>(args)...);
    }
};

template <class Inner, bool ActiveCondition>
struct guarded_step {
    using target_t = typename Inner::target_t;
    static constexpr target_t default_value = Inner::default_value;

    template <auto... /*Pinned*/>
    static constexpr bool active_v = ActiveCondition;

    template <class R, class Params, auto... Pinned,
              class Continuation, class... Args>
    [[gnu::always_inline]] static R dispatch(
        const Params& params, Continuation continuation, Args&&... args)
    {
        return Inner::template dispatch<R, Params, Pinned...>(
            params, continuation, std::forward<Args>(args)...);
    }
};

} // namespace

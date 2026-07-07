#pragma once

#include <memory>
#include <type_traits>
#include <utility>

#include "function.hpp"
#include "imports.h"

namespace cppgres {

template <class T, class... Args>
concept aggregate = requires(T t, Args &&...args) {
  { t.update(args...) };
};

template <class T, class... Args>
concept finalizable_aggregate = aggregate<T, Args...> && requires(T t) {
  { t.finalize() } -> convertible_into_datum;
};

template <class T, class... Args>
concept serializable_aggregate = aggregate<T, Args...> && requires(T t, bytea &ba) {
  { t.serialize() } -> std::same_as<bytea>;
  { T(ba) } -> std::same_as<T>;
};

template <class T, class... Args>
concept combinable_aggregate = aggregate<T, Args...> && requires(T &&t, T &&t1) {
  { T(t, t1) } -> std::same_as<T>;
};

template <class Agg, typename... Args>
Agg *construct_aggregate_state(abstract_memory_context &ctx, Args &&...args) {
  auto *state = ctx.template alloc<Agg>();
  std::construct_at(state, std::forward<Args>(args)...);
  if constexpr (!std::is_trivially_destructible_v<Agg>) {
    ctx.register_reset_callback([](void *arg) { std::destroy_at(static_cast<Agg *>(arg)); }, state);
  }
  return state;
}

template <class Agg, typename... Args>
Agg *construct_aggregate_state(abstract_memory_context &&ctx, Args &&...args) {
  return construct_aggregate_state<Agg>(ctx, std::forward<Args>(args)...);
}

template <class Agg, typename... InTs> datum aggregate_sfunc(value state, InTs... args) {

  MemoryContext aggctx;
  if (!ffi_guard{::AggCheckCallContext}(current_postgres_function::call_info().operator*(),
                                        &aggctx)) {
    report(ERROR, "not aggregate context");
  }

  if constexpr (!convertible_into_datum<Agg> && finalizable_aggregate<Agg, InTs...>) {
    Agg *state0;
    if (state.get_nullable_datum().is_null()) {
      state0 = construct_aggregate_state<Agg>(memory_context(aggctx));
    } else {
      state0 = reinterpret_cast<Agg *>(
          from_nullable_datum<void *>(state.get_nullable_datum(), state.get_type().oid));
    }

    state0->update(args...);

    return datum_conversion<void *>::into_datum(reinterpret_cast<void *>(state0));
  } else if constexpr (convertible_into_datum<Agg>) {
    Agg state0 = datum_conversion<Agg>::from_nullable_datum(state.get_nullable_datum(), ANYOID);
    state0.update(args...);
    return datum_conversion<Agg>::into_datum(state0);
  }
  report(ERROR, "not supported");
  __builtin_unreachable();
}

template <class Agg, typename... InTs> nullable_datum aggregate_ffunc(value state) {
  if constexpr (finalizable_aggregate<Agg, InTs...>) {
    Agg *state0;
    if (state.get_nullable_datum().is_null()) {
      state0 = construct_aggregate_state<Agg>(memory_context());
    } else {
      state0 = reinterpret_cast<Agg *>(
          from_nullable_datum<void *>(state.get_nullable_datum(), state.get_type().oid));
    }
    return into_nullable_datum(state0->finalize());
  } else {
    report(ERROR, "this aggregate does not support final function");
    __builtin_unreachable();
  }
}

template <class Agg, typename... InTs> bytea aggregate_serial(value state) {
  if constexpr (serializable_aggregate<Agg, InTs...>) {
    if (state.get_type().oid == INTERNALOID) {
      Agg *state0;
      state0 = reinterpret_cast<Agg *>(
          from_nullable_datum<void *>(state.get_nullable_datum(), state.get_type().oid));
      bytea ba = state0->serialize();
      return ba;
    }
  }
  report(ERROR, "this aggregate does not support serialize");
  __builtin_unreachable();
}

template <class Agg, typename... InTs> datum aggregate_deserial(bytea ba, value) {
  if constexpr (serializable_aggregate<Agg, InTs...>) {
    MemoryContext aggctx;
    if (!ffi_guard{::AggCheckCallContext}(current_postgres_function::call_info().operator*(),
                                          &aggctx)) {
      report(ERROR, "not aggregate context");
    }
    Agg *state0 = construct_aggregate_state<Agg>(memory_context(aggctx), ba);
    return datum_conversion<void *>::into_datum(reinterpret_cast<void *>(state0));
  }
  report(ERROR, "this aggregate does not support serialize");
  __builtin_unreachable();
}

template <class Agg, typename... InTs> datum aggregate_combine(value state, value other) {
  MemoryContext aggctx;
  if (!ffi_guard{::AggCheckCallContext}(current_postgres_function::call_info().operator*(),
                                        &aggctx)) {
    report(ERROR, "not aggregate context");
  }
  if constexpr (combinable_aggregate<Agg, InTs...>) {
    if constexpr (!convertible_into_datum<Agg> && finalizable_aggregate<Agg, InTs...>) {
      Agg *state0;
      if (state.get_nullable_datum().is_null()) {
        state0 = construct_aggregate_state<Agg>(memory_context(aggctx));
      } else {
        state0 = reinterpret_cast<Agg *>(
            from_nullable_datum<void *>(state.get_nullable_datum(), state.get_type().oid));
      }

      Agg *state1;
      if (other.get_nullable_datum().is_null()) {
        state1 = construct_aggregate_state<Agg>(memory_context(aggctx));
      } else {
        state1 = reinterpret_cast<Agg *>(
            from_nullable_datum<void *>(other.get_nullable_datum(), other.get_type().oid));
      }

      Agg *newstate = construct_aggregate_state<Agg>(memory_context(aggctx), *state0, *state1);

      return datum_conversion<void *>::into_datum(reinterpret_cast<void *>(newstate));
    } else if constexpr (convertible_into_datum<Agg>) {
      Agg state0 = datum_conversion<Agg>::from_nullable_datum(state.get_nullable_datum(), ANYOID);
      Agg state1 = datum_conversion<Agg>::from_nullable_datum(other.get_nullable_datum(), ANYOID);
      return datum_conversion<Agg>::into_datum(Agg(state0, state1));
    }
  }
  report(ERROR, "not supported");
  __builtin_unreachable();
}

} // namespace cppgres

#define declare_aggregate(name, typname, ...)                                                      \
  static_assert(::cppgres::aggregate<typname, ##__VA_ARGS__>);                                     \
  static_assert(::cppgres::convertible_into_datum<typname> ||                                      \
                    ::cppgres::finalizable_aggregate<typname, ##__VA_ARGS__>,                   \
                "must be convertible to datum or have finalize()");                                \
  postgres_function(name##_sfunc, (cppgres::aggregate_sfunc<typname, ##__VA_ARGS__>));             \
  postgres_function(name##_ffunc, (cppgres::aggregate_ffunc<typname, ##__VA_ARGS__>));             \
  postgres_function(name##_serial, (cppgres::aggregate_serial<typname, ##__VA_ARGS__>));           \
  postgres_function(name##_deserial, (cppgres::aggregate_deserial<typname, ##__VA_ARGS__>));       \
  postgres_function(name##_combine, (cppgres::aggregate_combine<typname, ##__VA_ARGS__>));

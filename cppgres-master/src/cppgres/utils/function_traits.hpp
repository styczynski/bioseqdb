/**
* \file
 */
#pragma once

namespace cppgres::utils::function_traits {
// Primary template (will be specialized below)
template <typename T> struct function_traits;

// Specialization for function pointers.
template <typename R, typename... Args> struct function_traits<R (*)(Args...)> {
  using argument_types = std::tuple<Args...>;
  static constexpr std::size_t arity = sizeof...(Args);
};

// Specialization for function references.
template <typename R, typename... Args> struct function_traits<R (&)(Args...)> {
  using argument_types = std::tuple<Args...>;
  static constexpr std::size_t arity = sizeof...(Args);
};

// Specialization for function types themselves.
template <typename R, typename... Args> struct function_traits<R(Args...)> {
  using argument_types = std::tuple<Args...>;
  static constexpr std::size_t arity = sizeof...(Args);
};

// Specialization for member function pointers (e.g. for lambdas' operator())
template <typename C, typename R, typename... Args>
struct function_traits<R (C:: *)(Args...) const> {
  using argument_types = std::tuple<Args...>;
  static constexpr std::size_t arity = sizeof...(Args);
};

// Fallback for functors/lambdas that are not plain function pointers.
// This will delegate to the member function pointer version.
template <typename T> struct function_traits : function_traits<decltype(&T::operator())> {};

// Primary template (left undefined)
template <typename Func, typename Tuple> struct invoke_result_from_tuple;

// Partial specialization for when Tuple is a std::tuple<Args...>
template <typename Func, typename... Args>
struct invoke_result_from_tuple<Func, std::tuple<Args...>> {
  using type = std::invoke_result_t<Func, Args...>;
};

// Convenience alias template.
template <typename Func, typename Tuple>
using invoke_result_from_tuple_t = typename invoke_result_from_tuple<Func, Tuple>::type;

} // namespace cppgres::utils::function_traits

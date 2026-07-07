/**
* \file
 */
#pragma once

#include <optional>
#include <string_view>
#include <tuple>
#include <utility>

#include "pfr.hpp"
#define CPPGRES_USE_BOOST_PFR 1
/*
// This has been commented out while Boost.PFR is embedded with Cppgres

#if __has_include(<boost/pfr.hpp>)
#include <boost/pfr.hpp>
#define CPPGRES_USE_BOOST_PFR 1
#else
#define CPPGRES_USE_BOOST_PFR 0
#endif
*/
namespace cppgres::utils {

// Primary template: if T is not an optional, just yield T.
template <typename T> struct remove_optional {
  using type = T;
};

// Partial specialization for std::optional.
template <typename T> struct remove_optional<std::optional<T>> {
  using type = T;
};

// Convenience alias template.
template <typename T> using remove_optional_t = typename utils::remove_optional<T>::type;

template <typename T>
concept is_optional =
    requires { typename T::value_type; } && std::same_as<T, std::optional<typename T::value_type>>;

template <typename T> constexpr std::string_view type_name() {
#ifdef __clang__
  constexpr std::string_view p = __PRETTY_FUNCTION__;
  constexpr std::string_view key = "T = ";
  const auto start = p.find(key) + key.size();
  const auto end = p.find(']', start);
  return p.substr(start, end - start);
#elif defined(__GNUC__)
  constexpr std::string_view p = __PRETTY_FUNCTION__;
  constexpr std::string_view key = "T = ";
  const auto start = p.find(key) + key.size();
  const auto end = p.find(';', start);
  return p.substr(start, end - start);
#elif defined(_MSC_VER)
  constexpr std::string_view p = __FUNCSIG__;
  constexpr std::string_view key = "type_name<";
  const auto start = p.find(key) + key.size();
  const auto end = p.find(">(void)");
  return p.substr(start, end - start);
#else
  return "Unsupported compiler";
#endif
}

template <typename T, typename = void> struct tuple_traits_impl {
  using tuple_size_type = std::integral_constant<std::size_t, 1>;

  template <std::size_t I, typename U = T> static constexpr decltype(auto) get(U &&t) noexcept {
    return std::forward<U>(t);
  }

  struct tuple_element_t {
    using type = T;
  };

  template <std::size_t I> using tuple_element = tuple_element_t;
};

// Primary implementation: for types that already have a tuple-like interface
template <typename T>
struct tuple_traits_impl<T, std::void_t<decltype(std::tuple_size<T>::value)>> {
  using tuple_size_type = std::tuple_size<T>;

  template <std::size_t I, typename U = T> static constexpr decltype(auto) get(U &&t) noexcept {
    return std::get<I>(std::forward<U>(t));
  }

  template <std::size_t I> using tuple_element = std::tuple_element<I, T>;
};

// Specialization: for aggregates that Boost.PFR can handle.
// This specialization is enabled if the type is an aggregate
#if CPPGRES_USE_BOOST_PFR
template <typename T> struct tuple_traits_impl<T, std::enable_if_t<std::is_aggregate_v<T>>> {
  using tuple_size_type = boost::pfr::tuple_size<T>;

  template <std::size_t I, typename U = T> static constexpr decltype(auto) get(U &&t) noexcept {
    return boost::pfr::get<I>(std::forward<U>(t));
  }

  template <std::size_t I> using tuple_element = boost::pfr::tuple_element<I, T>;
};

#endif

template <typename T> using tuple_size = typename tuple_traits_impl<T>::tuple_size_type;

template <typename T> constexpr std::size_t tuple_size_v = tuple_size<T>::value;

template <std::size_t I, typename T>
using tuple_element = typename tuple_traits_impl<T>::template tuple_element<I>;

template <std::size_t I, typename T> using tuple_element_t = typename tuple_element<I, T>::type;

template <std::size_t I, typename T> constexpr decltype(auto) get(T &&t) noexcept {
  return tuple_traits_impl<std::remove_cv_t<std::remove_reference_t<T>>>::template get<I>(
      std::forward<T>(t));
}

template <typename T> struct is_std_tuple : std::false_type {};

template <typename... Ts> struct is_std_tuple<std::tuple<Ts...>> : std::true_type {};

template <typename T>
concept std_tuple = is_std_tuple<T>::value;

template <typename T> decltype(auto) tie(T &val) {
#if CPPGRES_USE_BOOST_PFR == 1
  if constexpr (std::is_aggregate_v<T>) {
    return boost::pfr::structure_tie(val);
  } else
#endif
      if constexpr (std_tuple<T>) {
    return val;
  } else {
    return std::tuple(val);
  }
}

} // namespace cppgres::utils

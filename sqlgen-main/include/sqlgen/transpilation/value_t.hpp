#ifndef SQLGEN_TRANSPILATION_VALUET_HPP_
#define SQLGEN_TRANSPILATION_VALUET_HPP_

#include <functional>
#include <ranges>
#include <type_traits>

#include "../Range.hpp"

namespace sqlgen::transpilation {

template <class ContainerType>
struct ValueType;

template <class T>
  requires(!std::ranges::input_range<T>)
struct ValueType<T> {
  using Type = std::remove_cvref_t<T>;
};

template <class ContainerType>
  requires std::ranges::input_range<ContainerType>
struct ValueType<ContainerType> {
  using Type = std::remove_cvref_t<typename ContainerType::value_type>;
};

template <class T>
struct ValueType<Range<T>> {
  using Type = std::remove_cvref_t<typename Range<T>::value_type::value_type>;
};

template <class T>
struct ValueType<std::reference_wrapper<T>> {
  using Type = typename ValueType<std::remove_cvref_t<T>>::Type;
};

template <class T>
using value_t = typename ValueType<std::remove_cvref_t<T>>::Type;

}  // namespace sqlgen::transpilation

#endif

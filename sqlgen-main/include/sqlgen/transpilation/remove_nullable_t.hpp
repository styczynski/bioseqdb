#ifndef SQLGEN_TRANSPILATION_REMOVENULLABLET_HPP_
#define SQLGEN_TRANSPILATION_REMOVENULLABLET_HPP_

#include <optional>
#include <type_traits>

#include "is_nullable.hpp"

namespace sqlgen::transpilation {

template <class T>
struct RemoveNullable;

template <class T>
struct RemoveNullable {
  using Type = T;
};

template <class T>
  requires is_ptr<T>::value
struct RemoveNullable<T> {
  using Type =
      typename RemoveNullable<std::remove_cvref_t<typename T::elem_type>>::Type;
};

template <class T>
struct RemoveNullable<std::optional<T>> {
  using Type = typename RemoveNullable<std::remove_cvref_t<T>>::Type;
};

template <class T>
using remove_nullable_t = typename RemoveNullable<std::remove_cvref_t<T>>::Type;

}  // namespace sqlgen::transpilation

#endif

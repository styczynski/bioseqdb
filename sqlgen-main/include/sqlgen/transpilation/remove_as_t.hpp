#ifndef SQLGEN_TRANSPILATION_REMOVEAST_HPP_
#define SQLGEN_TRANSPILATION_REMOVEAST_HPP_

#include <rfl.hpp>
#include <type_traits>

#include "As.hpp"

namespace sqlgen::transpilation {

template <class T>
struct RemoveAs;

template <class T>
struct RemoveAs {
  using Type = T;
};

template <class T, rfl::internal::StringLiteral _new_name>
struct RemoveAs<As<T, _new_name>> {
  using Type = typename RemoveAs<std::remove_cvref_t<T>>::Type;
};

template <class T>
using remove_as_t = typename RemoveAs<std::remove_cvref_t<T>>::Type;

}  // namespace sqlgen::transpilation

#endif

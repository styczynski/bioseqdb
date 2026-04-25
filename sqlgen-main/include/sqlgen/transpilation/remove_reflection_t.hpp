#ifndef SQLGEN_TRANSPILATION_REMOVEREFLECTIONT_HPP_
#define SQLGEN_TRANSPILATION_REMOVEREFLECTIONT_HPP_

#include <memory>
#include <optional>
#include <rfl.hpp>

#include "has_reflection_method.hpp"

namespace sqlgen::transpilation {

template <class T>
struct RemoveReflection;

template <class T>
struct RemoveReflection {
  using Type = T;
};

template <rfl::internal::StringLiteral _format>
struct RemoveReflection<rfl::Timestamp<_format>> {
  using Type = rfl::Timestamp<_format>;
};

template <class T>
  requires has_reflection_method<T>
struct RemoveReflection<T> {
  using Type = typename RemoveReflection<typename T::ReflectionType>::Type;
};

template <class T>
struct RemoveReflection<std::optional<T>> {
  using Type = std::optional<typename RemoveReflection<T>::Type>;
};

template <class T>
struct RemoveReflection<std::shared_ptr<T>> {
  using Type = std::shared_ptr<typename RemoveReflection<T>::Type>;
};

template <class T>
struct RemoveReflection<std::unique_ptr<T>> {
  using Type = std::unique_ptr<typename RemoveReflection<T>::Type>;
};

template <class T>
using remove_reflection_t =
    typename RemoveReflection<std::remove_cvref_t<T>>::Type;

}  // namespace sqlgen::transpilation

#endif

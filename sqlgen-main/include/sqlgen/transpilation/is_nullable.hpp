#ifndef SQLGEN_TRANSPILATION_IS_NULLABLE_HPP_
#define SQLGEN_TRANSPILATION_IS_NULLABLE_HPP_

#include <memory>
#include <optional>
#include <type_traits>

#include "has_reflection_method.hpp"

namespace sqlgen::transpilation {

template <class T>
class is_ptr;

template <class T>
class is_ptr : public std::false_type {};

template <class T>
class is_ptr<std::shared_ptr<T>> : public std::true_type {};

template <class T>
class is_ptr<std::unique_ptr<T>> : public std::true_type {};

template <class T>
class is_optional;

template <class T>
class is_optional : public std::false_type {};

template <class T>
class is_optional<std::optional<T>> : public std::true_type {};

template <class T>
consteval bool is_nullable() {
  if constexpr (has_reflection_method<T>) {
    return is_nullable<typename T::ReflectionType>();
  } else {
    return is_ptr<std::remove_cvref_t<T>>::value ||
           is_optional<std::remove_cvref_t<T>>::value;
  }
}

template <class T>
constexpr bool is_nullable_v = is_nullable<std::remove_cvref_t<T>>();

}  // namespace sqlgen::transpilation

#endif

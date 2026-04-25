#ifndef SQLGEN_TRANSPILATION_TO_VALUE_HPP_
#define SQLGEN_TRANSPILATION_TO_VALUE_HPP_

#include <rfl.hpp>
#include <type_traits>

#include "../dynamic/Type.hpp"
#include "../dynamic/Value.hpp"
#include "Value.hpp"
#include "has_reflection_method.hpp"
#include "is_nullable.hpp"

namespace sqlgen::transpilation {

template <class T>
struct ToValue;

template <class T>
struct ToValue {
  dynamic::Value operator()(const T& _t) const {
    using Type = std::remove_cvref_t<T>;
    if constexpr (is_nullable_v<Type>) {
      if (!_t) {
        return dynamic::Value{dynamic::Null{}};
      }
      return ToValue<std::remove_cvref_t<decltype(*_t)>>{}(*_t);

    } else if constexpr (std::is_same_v<Type, std::nullopt_t> ||
                         std::is_same_v<Type, std::nullptr_t>) {
      return dynamic::Value{dynamic::Null{}};

    } else if constexpr (std::is_floating_point_v<Type>) {
      return dynamic::Value{dynamic::Float{.val = static_cast<double>(_t)}};

    } else if constexpr (std::is_same_v<Type, bool>) {
      return dynamic::Value{dynamic::Boolean{.val = _t}};

    } else if constexpr (std::is_integral_v<Type>) {
      return dynamic::Value{dynamic::Integer{.val = static_cast<int64_t>(_t)}};

    } else if constexpr (std::is_convertible_v<Type, std::string>) {
      return dynamic::Value{dynamic::String{.val = std::string(_t)}};

    } else if constexpr (has_reflection_method<Type>) {
      return ToValue<typename Type::ReflectionType>{}(_t.reflection());

    } else if constexpr (std::is_enum_v<Type>) {
      return dynamic::Value{dynamic::String{.val = rfl::enum_to_string(_t)}};

    } else {
      static_assert(rfl::always_false_v<T>, "Unsupported type");
    }
  }
};

template <class T>
dynamic::Value to_value(const T& _t) {
  return ToValue<std::remove_cvref_t<T>>{}(_t);
}

}  // namespace sqlgen::transpilation

#endif

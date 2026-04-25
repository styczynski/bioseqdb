#ifndef SQLGEN_TRANSPILATION_HAS_REFLECTION_TYPE_HPP_
#define SQLGEN_TRANSPILATION_HAS_REFLECTION_TYPE_HPP_

#include <concepts>

namespace sqlgen::transpilation {

template <typename T>
concept has_reflection_method = requires(T _t) {
  { _t.reflection() } -> std::convertible_to<typename T::ReflectionType>;
};

}  // namespace sqlgen::transpilation

#endif

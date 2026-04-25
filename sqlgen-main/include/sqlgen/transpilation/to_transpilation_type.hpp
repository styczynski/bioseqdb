#ifndef SQLGEN_TRANSPILATION_TO_TRANSPILATION_TYPE_HPP_
#define SQLGEN_TRANSPILATION_TO_TRANSPILATION_TYPE_HPP_

#include <string>
#include <type_traits>

#include "Value.hpp"

namespace sqlgen::transpilation {

template <class T>
struct ToTranspilationType;

template <class T>
struct ToTranspilationType {
  using Type = Value<T>;

  Type operator()(const T& _val) const noexcept { return make_value(_val); }
};

template <>
struct ToTranspilationType<const char*> {
  using Type = Value<std::string>;

  Type operator()(const char* _val) const noexcept { return make_value(_val); }
};

template <int _length>
struct ToTranspilationType<char[_length]> {
  using Type = Value<std::string>;

  Type operator()(const char* _val) const noexcept { return make_value(_val); }
};

template <class T>
auto to_transpilation_type(const T& _t) {
  return ToTranspilationType<std::remove_cvref_t<T>>{}(_t);
}

inline auto to_transpilation_type(const char* _t) {
  return ToTranspilationType<const char*>{}(_t);
}

}  // namespace sqlgen::transpilation

#endif

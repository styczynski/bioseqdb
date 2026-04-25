#ifndef SQLGEN_TRANSPILATION_VALUE_HPP_
#define SQLGEN_TRANSPILATION_VALUE_HPP_

#include <string>
#include <type_traits>

namespace sqlgen::transpilation {

template <class T>
struct Value {
  T val;
};

template <class T>
auto make_value(T&& _t) {
  return Value<std::remove_cvref_t<T>>{.val = _t};
}

inline auto make_value(const char* _str) {
  return Value<std::string>{.val = _str};
}
}  // namespace sqlgen::transpilation

#endif

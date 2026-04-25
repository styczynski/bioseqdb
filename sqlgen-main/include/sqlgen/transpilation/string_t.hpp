#ifndef SQLGEN_TRANSPILATION_STRINGT_HPP_
#define SQLGEN_TRANSPILATION_STRINGT_HPP_

#include <string>
#include <type_traits>

namespace sqlgen::transpilation {

template <class T>
struct StringType;

template <class T>
struct StringType {
  using Type = T;
};

template <class T>
  requires std::is_convertible_v<T, std::string>
struct StringType<T> {
  using Type = std::string;
};

template <class T>
using string_t = typename StringType<std::remove_cvref_t<T>>::Type;

}  // namespace sqlgen::transpilation

#endif

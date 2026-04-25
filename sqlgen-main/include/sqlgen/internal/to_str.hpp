#ifndef SQLGEN_INTERNAL_TO_STR_HPP_
#define SQLGEN_INTERNAL_TO_STR_HPP_

#include <optional>
#include <string>
#include <type_traits>

#include "../parsing/Parser.hpp"

namespace sqlgen::internal {

template <class T>
std::optional<std::string> to_str(const T& _val) {
  using Type = std::remove_cvref_t<T>;
  return parsing::Parser<Type>::write(_val);
}

}  // namespace sqlgen::internal

#endif

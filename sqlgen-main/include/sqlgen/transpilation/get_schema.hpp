#ifndef SQLGEN_TRANSPILATION_GET_SCHEMA_HPP_
#define SQLGEN_TRANSPILATION_GET_SCHEMA_HPP_

#include <optional>
#include <rfl.hpp>
#include <type_traits>

#include "has_schema.hpp"

namespace sqlgen::transpilation {

template <class T>
std::optional<std::string> get_schema() noexcept {
  using Type = std::remove_cvref_t<T>;
  if constexpr (has_schema<Type>) {
    return std::string(Type::schema);
  } else {
    return std::nullopt;
  }
}

}  // namespace sqlgen::transpilation

#endif

#ifndef SQLGEN_TRANSPILATION_HAS_SCHEMA_HPP_
#define SQLGEN_TRANSPILATION_HAS_SCHEMA_HPP_

#include <concepts>
#include <string>

namespace sqlgen::transpilation {

template <typename T>
concept has_schema = requires() {
  { T::schema } -> std::convertible_to<std::string>;
};

}  // namespace sqlgen::transpilation

#endif

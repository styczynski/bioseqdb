#ifndef SQLGEN_TRANSPILATION_TO_ALIAS_HPP_
#define SQLGEN_TRANSPILATION_TO_ALIAS_HPP_

#include <optional>
#include <rfl.hpp>
#include <string>
#include <type_traits>

#include "../Literal.hpp"
#include "../Result.hpp"

namespace sqlgen::transpilation {

template <class T>
struct ToAlias;

template <>
struct ToAlias<Nothing> {
  std::optional<std::string> operator()() const { return std::nullopt; }
};

template <>
struct ToAlias<Literal<"">> {
  std::optional<std::string> operator()() const { return std::nullopt; }
};

template <rfl::internal::StringLiteral _alias>
struct ToAlias<Literal<_alias>> {
  std::optional<std::string> operator()() const {
    return Literal<_alias>().str();
  }
};

template <class T>
std::optional<std::string> to_alias() {
  return ToAlias<std::remove_cvref_t<T>>{}();
}

}  // namespace sqlgen::transpilation

#endif

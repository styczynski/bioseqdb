#ifndef SQLGEN_PARSING_PARSER_OPTIONAL_HPP_
#define SQLGEN_PARSING_PARSER_OPTIONAL_HPP_

#include <optional>
#include <string>
#include <type_traits>

#include "../Result.hpp"
#include "../dynamic/Type.hpp"
#include "Parser_base.hpp"

namespace sqlgen::parsing {

template <class T>
struct Parser<std::optional<T>> {
  static Result<std::optional<T>> read(
      const std::optional<std::string>& _str) noexcept {
    if (!_str) {
      return std::optional<T>();
    }
    return Parser<std::remove_cvref_t<T>>::read(_str).transform(
        [](auto&& _t) -> std::optional<T> {
          return std::make_optional<T>(std::move(_t));
        });
  }

  static std::optional<std::string> write(const std::optional<T>& _o) noexcept {
    if (!_o) {
      return std::nullopt;
    }
    return Parser<std::remove_cvref_t<T>>::write(*_o);
  }

  static dynamic::Type to_type() noexcept {
    return Parser<std::remove_cvref_t<T>>::to_type().visit(
        [](auto _t) -> dynamic::Type {
          _t.properties.nullable = true;
          return _t;
        });
  }
};

}  // namespace sqlgen::parsing

#endif

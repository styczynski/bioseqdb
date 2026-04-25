#ifndef SQLGEN_PARSING_PARSER_UNIQUE_PTR_HPP_
#define SQLGEN_PARSING_PARSER_UNIQUE_PTR_HPP_

#include <memory>
#include <string>
#include <type_traits>

#include "../Result.hpp"
#include "../dynamic/Type.hpp"
#include "Parser_base.hpp"

namespace sqlgen::parsing {

template <class T>
struct Parser<std::unique_ptr<T>> {
  static Result<std::unique_ptr<T>> read(
      const std::optional<std::string>& _str) noexcept {
    if (!_str) {
      return std::unique_ptr<T>();
    }
    return Parser<std::remove_cvref_t<T>>::read(_str).transform(
        [](auto&& _t) -> std::unique_ptr<T> {
          return std::make_unique<T>(std::move(_t));
        });
  }

  static std::optional<std::string> write(
      const std::unique_ptr<T>& _ptr) noexcept {
    if (!_ptr) {
      return std::nullopt;
    }
    return Parser<std::remove_cvref_t<T>>::write(*_ptr);
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

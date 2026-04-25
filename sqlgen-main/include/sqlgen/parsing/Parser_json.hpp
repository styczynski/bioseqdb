#ifndef SQLGEN_PARSING_PARSER_JSON_HPP_
#define SQLGEN_PARSING_PARSER_JSON_HPP_

#include <rfl/json.hpp>
#include <string>
#include <type_traits>

#include "../JSON.hpp"
#include "../Result.hpp"
#include "../dynamic/Type.hpp"
#include "Parser_base.hpp"

namespace sqlgen::parsing {

template <class T>
struct Parser<JSON<T>> {
  static Result<JSON<T>> read(const std::optional<std::string>& _str) noexcept {
    if (!_str) {
      return error("NULL value encounted: JSON value cannot be NULL.");
    }
    return rfl::json::read<T>(*_str).transform(
        [](auto&& _t) { return JSON<T>(std::move(_t)); });
  }

  static std::optional<std::string> write(const JSON<T>& _j) noexcept {
    return rfl::json::write(_j.value());
  }

  static dynamic::Type to_type() noexcept { return dynamic::types::JSON{}; }
};

}  // namespace sqlgen::parsing

#endif

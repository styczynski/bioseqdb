#ifndef SQLGEN_PARSING_PARSER_STRING_HPP_
#define SQLGEN_PARSING_PARSER_STRING_HPP_

#include <string>
#include <type_traits>

#include "../Result.hpp"
#include "../dynamic/Type.hpp"
#include "../dynamic/types.hpp"
#include "Parser_base.hpp"

namespace sqlgen::parsing {

template <>
struct Parser<std::string> {
  static Result<std::string> read(
      const std::optional<std::string>& _str) noexcept {
    if (!_str) {
      return error("NULL value encounted: String value cannot be NULL.");
    }
    return *_str;
  }

  static std::optional<std::string> write(const std::string& _str) noexcept {
    return _str;
  }

  static dynamic::Type to_type() noexcept { return dynamic::types::Text{}; }
};

}  // namespace sqlgen::parsing

#endif

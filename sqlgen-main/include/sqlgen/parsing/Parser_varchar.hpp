#ifndef SQLGEN_PARSING_PARSER_VARCHAR_HPP_
#define SQLGEN_PARSING_PARSER_VARCHAR_HPP_

#include <string>
#include <type_traits>

#include "../Result.hpp"
#include "../Varchar.hpp"
#include "../dynamic/Type.hpp"
#include "../dynamic/types.hpp"
#include "Parser_base.hpp"
#include "Parser_default.hpp"

namespace sqlgen::parsing {

template <size_t _size>
struct Parser<Varchar<_size>> {
  static Result<Varchar<_size>> read(
      const std::optional<std::string>& _str) noexcept {
    return Parser<std::string>::read(_str).and_then(
        [](auto&& _t) -> Result<Varchar<_size>> {
          return Varchar<_size>::make(std::move(_t));
        });
  }

  static std::optional<std::string> write(const Varchar<_size>& _v) noexcept {
    return Parser<std::string>::write(_v.value());
  }

  static dynamic::Type to_type() noexcept {
    return dynamic::types::VarChar{.length = _size};
  }
};

}  // namespace sqlgen::parsing

#endif

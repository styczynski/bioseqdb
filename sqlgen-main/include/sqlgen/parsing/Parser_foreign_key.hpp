#ifndef SQLGEN_PARSING_PARSER_FOREIGN_KEY_HPP_
#define SQLGEN_PARSING_PARSER_FOREIGN_KEY_HPP_

#include <string>
#include <type_traits>

#include "../ForeignKey.hpp"
#include "../Result.hpp"
#include "../dynamic/Type.hpp"
#include "../transpilation/get_tablename.hpp"
#include "Parser_base.hpp"

namespace sqlgen::parsing {

template <class T, class _ForeignTableType,
          rfl::internal::StringLiteral _col_name>
struct Parser<ForeignKey<T, _ForeignTableType, _col_name>> {
  static Result<ForeignKey<T, _ForeignTableType, _col_name>> read(
      const std::optional<std::string>& _str) noexcept {
    return Parser<std::remove_cvref_t<T>>::read(_str).transform([](auto&& _t) {
      return ForeignKey<T, _ForeignTableType, _col_name>(std::move(_t));
    });
  }

  static std::optional<std::string> write(
      const ForeignKey<T, _ForeignTableType, _col_name>& _f) noexcept {
    return Parser<std::remove_cvref_t<T>>::write(_f.value());
  }

  static dynamic::Type to_type() noexcept {
    return Parser<std::remove_cvref_t<T>>::to_type().visit(
        [](auto _t) -> dynamic::Type {
          _t.properties.foreign_key_reference =
              dynamic::types::ForeignKeyReference{
                  .table = transpilation::get_tablename<_ForeignTableType>(),
                  .column = _col_name.str()};
          return _t;
        });
  }
};

}  // namespace sqlgen::parsing

#endif

#ifndef SQLGEN_DUCKDB_PARSING_PARSER_OPTIONAL_HPP_
#define SQLGEN_DUCKDB_PARSING_PARSER_OPTIONAL_HPP_

#include <duckdb.h>

#include <optional>
#include <string>
#include <type_traits>

#include "../../Result.hpp"
#include "Parser_base.hpp"

namespace sqlgen::duckdb::parsing {

template <class T>
struct Parser<std::optional<T>> {
  using Type = std::remove_cvref_t<T>;
  using ResultingType = typename Parser<Type>::ResultingType;

  static Result<std::optional<T>> read(const ResultingType* _r) noexcept {
    if (!_r) {
      return std::optional<T>();
    }
    return Parser<std::remove_cvref_t<T>>::read(_r).transform(
        [](auto&& _t) -> std::optional<T> {
          return std::make_optional<T>(std::move(_t));
        });
  }

  static Result<Nothing> write(const std::optional<T>& _o,
                               duckdb_appender _appender) noexcept {
    if (!_o) {
      return duckdb_append_null(_appender) != DuckDBError
                 ? Result<Nothing>(Nothing{})
                 : Result<Nothing>(error("Could not append null value."));
    }
    return Parser<std::remove_cvref_t<T>>::write(*_o, _appender);
  }
};

}  // namespace sqlgen::duckdb::parsing

#endif

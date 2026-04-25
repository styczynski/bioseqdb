#ifndef SQLGEN_DUCKDB_PARSING_PARSER_SMART_PTR_HPP_
#define SQLGEN_DUCKDB_PARSING_PARSER_SMART_PTR_HPP_

#include <duckdb.h>

#include <string>
#include <type_traits>

#include "../../Result.hpp"
#include "../../transpilation/is_nullable.hpp"
#include "Parser_base.hpp"

namespace sqlgen::duckdb::parsing {

template <class T>
  requires transpilation::is_nullable_v<std::remove_cvref_t<T>>
struct Parser<T> {
  using Type = std::remove_cvref_t<T>;
  using ResultingType =
      typename Parser<typename Type::value_type>::ResultingType;

  static Result<T> read(const ResultingType* _r) noexcept {
    if (!_r) {
      return T();
    }
    return Parser<typename Type::value_type>::read(_r).transform(
        [](auto&& _u) -> T {
          using U = std::remove_cvref<decltype(_u)>;
          return T(new U(std::move(_u)));
        });
  }

  static Result<Nothing> write(const T& _ptr,
                               duckdb_appender _appender) noexcept {
    if (!_ptr) {
      return duckdb_append_null(_appender) != DuckDBError
                 ? Result<Nothing>(Nothing{})
                 : Result<Nothing>(error("Could not append null value."));
    }
    return Parser<std::remove_cvref_t<T>>::write(*_ptr, _appender);
  }
};

}  // namespace sqlgen::duckdb::parsing

#endif

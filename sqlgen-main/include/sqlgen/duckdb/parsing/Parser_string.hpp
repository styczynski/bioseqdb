#ifndef SQLGEN_DUCKDB_PARSING_PARSER_STRING_HPP_
#define SQLGEN_DUCKDB_PARSING_PARSER_STRING_HPP_

#include <duckdb.h>

#include <rfl.hpp>
#include <string>

#include "../../Result.hpp"
#include "Parser_base.hpp"

namespace sqlgen::duckdb::parsing {

template <>
struct Parser<std::string> {
  using ResultingType = duckdb_string_t;

  static Result<std::string> read(const ResultingType* _r) noexcept {
    if (!_r) {
      return error("String value cannot be NULL.");
    }
    if (duckdb_string_is_inlined(*_r)) {
      return std::string(_r->value.inlined.inlined, _r->value.inlined.length);
    } else {
      return std::string(_r->value.pointer.ptr, _r->value.pointer.length);
    }
  }

  static Result<Nothing> write(const std::string& _t,
                               duckdb_appender _appender) noexcept {
    return duckdb_append_varchar_length(_appender, _t.c_str(), _t.length()) !=
                   DuckDBError
               ? Result<Nothing>(Nothing{})
               : Result<Nothing>(error("Could not append string value."));
  }
};

}  // namespace sqlgen::duckdb::parsing

#endif

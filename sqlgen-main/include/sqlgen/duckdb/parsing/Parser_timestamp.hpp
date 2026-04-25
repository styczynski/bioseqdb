#ifndef SQLGEN_DUCKDB_PARSING_PARSER_TIMESTAMP_HPP_
#define SQLGEN_DUCKDB_PARSING_PARSER_TIMESTAMP_HPP_

#include <duckdb.h>

#include <rfl.hpp>
#include <rfl/internal/StringLiteral.hpp>
#include <string>

#include "../../Result.hpp"
#include "Parser_base.hpp"

namespace sqlgen::duckdb::parsing {

template <rfl::internal::StringLiteral _format>
struct Parser<rfl::Timestamp<_format>> {
  using ResultingType = duckdb_timestamp;

  static Result<rfl::Timestamp<_format>> read(
      const ResultingType* _r) noexcept {
    if (!_r) {
      return error("Timestamp value cannot be NULL.");
    }
    return rfl::Timestamp<_format>(static_cast<time_t>(_r->micros / 1000000));
  }

  static Result<Nothing> write(const rfl::Timestamp<_format>& _t,
                               duckdb_appender _appender) noexcept {
    return duckdb_append_timestamp(
               _appender,
               duckdb_timestamp{.micros = static_cast<int64_t>(_t.to_time_t()) *
                                          1000000}) != DuckDBError
               ? Result<Nothing>(Nothing{})
               : Result<Nothing>(error("Could not append timestamp value."));
  }
};

}  // namespace sqlgen::duckdb::parsing

#endif

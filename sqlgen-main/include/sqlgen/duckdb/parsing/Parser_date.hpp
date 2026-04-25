#ifndef SQLGEN_DUCKDB_PARSING_PARSER_DATE_HPP_
#define SQLGEN_DUCKDB_PARSING_PARSER_DATE_HPP_

#include <duckdb.h>

#include <rfl.hpp>
#include <rfl/internal/StringLiteral.hpp>
#include <string>

#include "../../Result.hpp"
#include "../../Timestamp.hpp"
#include "Parser_base.hpp"

namespace sqlgen::duckdb::parsing {

template <>
struct Parser<Date> {
  using ResultingType = duckdb_date;

  static constexpr time_t seconds_per_day = 24 * 60 * 60;

  static Result<Date> read(const ResultingType* _r) noexcept {
    if (!_r) {
      return error("Date value cannot be NULL.");
    }
    return Date(static_cast<time_t>(_r->days) * seconds_per_day);
  }

  static Result<Nothing> write(const Date& _t,
                               duckdb_appender _appender) noexcept {
    return duckdb_append_date(
               _appender, duckdb_date{.days = static_cast<int32_t>(
                                          _t.to_time_t() / seconds_per_day)}) !=
                   DuckDBError
               ? Result<Nothing>(Nothing{})
               : Result<Nothing>(error("Could not append date value."));
  }
};

}  // namespace sqlgen::duckdb::parsing

#endif

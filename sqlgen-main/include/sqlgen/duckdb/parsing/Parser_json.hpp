#ifndef SQLGEN_DUCKDB_PARSING_PARSER_JSON_HPP_
#define SQLGEN_DUCKDB_PARSING_PARSER_JSON_HPP_

#include <duckdb.h>

#include <rfl/json.hpp>
#include <string>
#include <type_traits>

#include "../../JSON.hpp"
#include "../../Result.hpp"
#include "Parser_base.hpp"
#include "Parser_string.hpp"

namespace sqlgen::duckdb::parsing {

template <class T>
struct Parser<JSON<T>> {
  using ResultingType = duckdb_string_t;

  static Result<JSON<T>> read(const ResultingType* _r) noexcept {
    return Parser<std::string>::read(_r).and_then(
        [&](const auto& _str) { return rfl::json::read<T>(_str); });
  }

  static Result<Nothing> write(const JSON<T>& _t,
                               duckdb_appender _appender) noexcept {
    try {
      return Parser<std::string>::write(rfl::json::write(_t.value()),
                                        _appender);
    } catch (const std::exception& e) {
      return error(e.what());
    }
  }
};

}  // namespace sqlgen::duckdb::parsing

#endif

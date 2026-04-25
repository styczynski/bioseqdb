#ifndef SQLGEN_DUCKDB_PARSING_PARSER_DEFAULT_HPP_
#define SQLGEN_DUCKDB_PARSING_PARSER_DEFAULT_HPP_

#include <duckdb.h>

#include <rfl.hpp>
#include <string>
#include <type_traits>

#include "../../Result.hpp"
#include "Parser_base.hpp"

namespace sqlgen::duckdb::parsing {

template <class T>
struct Parser {
  using Type = std::remove_cvref_t<T>;
  using ResultingType = Type;

  static Result<T> read(const ResultingType* _r) noexcept {
    if (!_r) {
      return error("Numeric or boolean value cannot be NULL.");
    }
    return Type(*_r);
  }

  static Result<Nothing> write(const T& _t,
                               duckdb_appender _appender) noexcept {
    if constexpr (std::is_same_v<Type, bool>) {
      return duckdb_append_bool(_appender, _t) != DuckDBError
                 ? Result<Nothing>(Nothing{})
                 : Result<Nothing>(error("Could not append boolean value."));

    } else if constexpr (std::is_same_v<Type, char> ||
                         std::is_same_v<Type, int8_t>) {
      return duckdb_append_int8(_appender, _t) != DuckDBError
                 ? Result<Nothing>(Nothing{})
                 : Result<Nothing>(error("Could not append int8 value."));

    } else if constexpr (std::is_same_v<Type, uint8_t>) {
      return duckdb_append_uint8(_appender, _t) != DuckDBError
                 ? Result<Nothing>(Nothing{})
                 : Result<Nothing>(error("Could not append uint8 value."));

    } else if constexpr (std::is_same_v<Type, int16_t>) {
      return duckdb_append_int16(_appender, _t) != DuckDBError
                 ? Result<Nothing>(Nothing{})
                 : Result<Nothing>(error("Could not append int16 value."));

    } else if constexpr (std::is_same_v<Type, uint16_t>) {
      return duckdb_append_uint16(_appender, _t) != DuckDBError
                 ? Result<Nothing>(Nothing{})
                 : Result<Nothing>(error("Could not append uint16 value."));

    } else if constexpr (std::is_same_v<Type, int32_t>) {
      return duckdb_append_int32(_appender, _t) != DuckDBError
                 ? Result<Nothing>(Nothing{})
                 : Result<Nothing>(error("Could not append int32 value."));

    } else if constexpr (std::is_same_v<Type, uint32_t>) {
      return duckdb_append_uint32(_appender, _t) != DuckDBError
                 ? Result<Nothing>(Nothing{})
                 : Result<Nothing>(error("Could not append uint32 value."));

    } else if constexpr (std::is_same_v<Type, int64_t>) {
      return duckdb_append_int64(_appender, _t) != DuckDBError
                 ? Result<Nothing>(Nothing{})
                 : Result<Nothing>(error("Could not append int64 value."));

    } else if constexpr (std::is_same_v<Type, uint64_t>) {
      return duckdb_append_uint64(_appender, _t) != DuckDBError
                 ? Result<Nothing>(Nothing{})
                 : Result<Nothing>(error("Could not append uint64 value."));

    } else if constexpr (std::is_same_v<Type, float>) {
      return duckdb_append_float(_appender, _t) != DuckDBError
                 ? Result<Nothing>(Nothing{})
                 : Result<Nothing>(error("Could not append float value."));

    } else if constexpr (std::is_same_v<Type, double>) {
      return duckdb_append_double(_appender, _t) != DuckDBError
                 ? Result<Nothing>(Nothing{})
                 : Result<Nothing>(error("Could not append double value."));

    } else {
      static_assert(rfl::always_false_v<T>, "Unsupported type.");
      return error("Unsupported type.");
    }
  }
};

}  // namespace sqlgen::duckdb::parsing

#endif

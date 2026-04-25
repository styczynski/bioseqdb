#ifndef SQLGEN_DUCKDB_CHECKDUCKDBTYPE_HPP_
#define SQLGEN_DUCKDB_CHECKDUCKDBTYPE_HPP_

#include <duckdb.h>

#include <rfl.hpp>
#include <type_traits>

#include "../Result.hpp"

namespace sqlgen::duckdb {

template <class T>
bool check_duckdb_type(duckdb_type _t) {
  using Type = std::remove_cvref_t<T>;

  switch (_t) {
    case DUCKDB_TYPE_BOOLEAN:
      return std::is_same_v<Type, bool>;

    case DUCKDB_TYPE_TINYINT:
      return std::is_same_v<Type, char> || std::is_same_v<Type, int8_t>;

    case DUCKDB_TYPE_ENUM:
    case DUCKDB_TYPE_UTINYINT:
      return std::is_same_v<Type, uint8_t>;

    case DUCKDB_TYPE_SMALLINT:
      return std::is_same_v<Type, int16_t>;

    case DUCKDB_TYPE_USMALLINT:
      return std::is_same_v<Type, uint16_t>;

    case DUCKDB_TYPE_INTEGER:
      return std::is_same_v<Type, int32_t>;

    case DUCKDB_TYPE_UINTEGER:
      return std::is_same_v<Type, uint32_t>;

    case DUCKDB_TYPE_BIGINT:
      return std::is_same_v<Type, int64_t>;

    case DUCKDB_TYPE_UBIGINT:
      return std::is_same_v<Type, uint64_t>;

    case DUCKDB_TYPE_FLOAT:
      return std::is_same_v<Type, float>;

    case DUCKDB_TYPE_DOUBLE:
      return std::is_same_v<Type, double>;

    case DUCKDB_TYPE_DATE:
      return std::is_same_v<Type, duckdb_date>;

    case DUCKDB_TYPE_VARCHAR:
      return std::is_same_v<Type, duckdb_string_t>;

    case DUCKDB_TYPE_TIMESTAMP:
      return std::is_same_v<Type, duckdb_timestamp>;

    default:
      return false;
  }
}

}  // namespace sqlgen::duckdb

#endif

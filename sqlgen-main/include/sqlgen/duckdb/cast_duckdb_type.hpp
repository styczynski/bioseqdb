#ifndef SQLGEN_DUCKDB_CASTDUCKDBTYPE_HPP_
#define SQLGEN_DUCKDB_CASTDUCKDBTYPE_HPP_

#include <duckdb.h>

#include <rfl.hpp>
#include <type_traits>

#include "../Ref.hpp"
#include "../Result.hpp"

namespace sqlgen::duckdb {

template <class T, class U>
Ref<std::vector<T>> cast_as_vector(const size_t _size, U* _ptr) {
  constexpr int64_t microseconds_per_day =
      static_cast<int64_t>(24 * 60 * 60) * static_cast<int64_t>(1000000);

  auto vec = Ref<std::vector<T>>::make(_size);
  for (size_t i = 0; i < _size; ++i) {
    if constexpr (std::is_same_v<U, duckdb_hugeint>) {
      (*vec)[i] = static_cast<T>(duckdb_hugeint_to_double(_ptr[i]));

    } else if constexpr (std::is_same_v<T, duckdb_timestamp> &&
                         std::is_same_v<U, duckdb_date>) {
      (*vec)[i] = duckdb_timestamp{
          .micros = static_cast<int64_t>(_ptr[i].days) * microseconds_per_day};

    } else if constexpr (std::is_same_v<T, duckdb_date> &&
                         std::is_same_v<U, duckdb_timestamp>) {
      (*vec)[i] = duckdb_date{
          .days = static_cast<int32_t>(_ptr[i].micros / microseconds_per_day)};

    } else {
      (*vec)[i] = static_cast<T>(_ptr[i]);
    }
  }
  return vec;
}

template <class T>
Result<Ref<std::vector<T>>> cast_duckdb_type(const duckdb_type _type,
                                             const size_t _size,
                                             void* _raw_ptr) {
  if constexpr (std::is_same_v<T, duckdb_timestamp>) {
    if (_type == DUCKDB_TYPE_DATE) {
      return cast_as_vector<T>(_size, static_cast<duckdb_date*>(_raw_ptr));
    }
    return error("Could not cast");

  } else if constexpr (std::is_same_v<T, duckdb_date>) {
    if (_type == DUCKDB_TYPE_TIMESTAMP) {
      return cast_as_vector<T>(_size, static_cast<duckdb_timestamp*>(_raw_ptr));
    }
    return error("Could not cast");

  } else if constexpr (!std::is_floating_point_v<T> && !std::is_integral_v<T>) {
    return error("Could not cast");

  } else {
    switch (_type) {
      case DUCKDB_TYPE_TINYINT:
        return cast_as_vector<T>(_size, static_cast<int8_t*>(_raw_ptr));

      case DUCKDB_TYPE_UTINYINT:
        return cast_as_vector<T>(_size, static_cast<uint8_t*>(_raw_ptr));

      case DUCKDB_TYPE_SMALLINT:
        return cast_as_vector<T>(_size, static_cast<int16_t*>(_raw_ptr));

      case DUCKDB_TYPE_USMALLINT:
        return cast_as_vector<T>(_size, static_cast<uint16_t*>(_raw_ptr));

      case DUCKDB_TYPE_INTEGER:
        return cast_as_vector<T>(_size, static_cast<int32_t*>(_raw_ptr));

      case DUCKDB_TYPE_UINTEGER:
        return cast_as_vector<T>(_size, static_cast<uint32_t*>(_raw_ptr));

      case DUCKDB_TYPE_BIGINT:
        return cast_as_vector<T>(_size, static_cast<int64_t*>(_raw_ptr));

      case DUCKDB_TYPE_UBIGINT:
        return cast_as_vector<T>(_size, static_cast<uint64_t*>(_raw_ptr));

      case DUCKDB_TYPE_FLOAT:
        return cast_as_vector<T>(_size, static_cast<float*>(_raw_ptr));

      case DUCKDB_TYPE_DOUBLE:
        return cast_as_vector<T>(_size, static_cast<double*>(_raw_ptr));

      case DUCKDB_TYPE_HUGEINT:
        return cast_as_vector<T>(_size, static_cast<duckdb_hugeint*>(_raw_ptr));

      default:
        return error("Could not cast");
    }
  }
}

}  // namespace sqlgen::duckdb

#endif

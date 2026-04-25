#ifndef SQLGEN_DUCKDB_COLUMNDATA_HPP_
#define SQLGEN_DUCKDB_COLUMNDATA_HPP_

#include <duckdb.h>

#include <memory>
#include <rfl.hpp>
#include <rfl/internal/StringLiteral.hpp>
#include <vector>

namespace sqlgen::duckdb {

template <class T, class _ColName>
struct ColumnData {
  using ColName = _ColName;

  duckdb_vector vec;
  T *data;
  uint64_t *validity;

  // This is only needed if the data returned by DuckDB is not of the
  // same type as T, but can be converted to T. In this case,
  // data actually points to ptr->data(). Otherwise, ptr is a nullptr.
  std::shared_ptr<std::vector<T>> ptr;

  bool is_not_null(idx_t _i) const {
    return (validity == nullptr) || duckdb_validity_row_is_valid(validity, _i);
  }
};

}  // namespace sqlgen::duckdb

#endif

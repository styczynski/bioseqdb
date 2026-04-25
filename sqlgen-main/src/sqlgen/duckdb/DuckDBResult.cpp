#include "sqlgen/duckdb/DuckDBResult.hpp"

namespace sqlgen::duckdb {

Result<Ref<DuckDBResult>> DuckDBResult::make(const std::string& _query,
                                             const ConnPtr& _conn) {
  try {
    return Ref<DuckDBResult>::make(_query, _conn);
  } catch (const std::exception& e) {
    return error(e.what());
  }
}

DuckDBResult::DuckDBResult(const std::string& _query, const ConnPtr& _conn)
    : destroy_(false) {
  if (duckdb_query(_conn->conn(), _query.c_str(), &res_) == DuckDBError) {
    throw std::runtime_error(duckdb_result_error(&res_));
  }
  destroy_ = true;
}

DuckDBResult::~DuckDBResult() {
  if (destroy_) {
    duckdb_destroy_result(&res_);
  }
}

DuckDBResult& DuckDBResult::operator=(DuckDBResult&& _other) {
  if (this == &_other) {
    return *this;
  }
  destroy_ = _other.destroy_;
  res_ = _other.res_;
  _other.destroy_ = false;
  return *this;
}

}  // namespace sqlgen::duckdb

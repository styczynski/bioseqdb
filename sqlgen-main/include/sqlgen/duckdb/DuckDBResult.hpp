#ifndef SQLGEN_DUCKDB_DUCKDBRESULT_HPP_
#define SQLGEN_DUCKDB_DUCKDBRESULT_HPP_

#include <duckdb.h>

#include <string>

#include "../sqlgen_api.hpp"
#include "DuckDBConnection.hpp"

namespace sqlgen::duckdb {

class SQLGEN_API DuckDBResult {
  using ConnPtr = Ref<DuckDBConnection>;

 public:
  static Result<Ref<DuckDBResult>> make(const std::string& _query,
                                        const ConnPtr& _conn);

  DuckDBResult(const std::string& _query, const ConnPtr& _conn);

  ~DuckDBResult();

  DuckDBResult(const DuckDBResult& _other) = delete;

  DuckDBResult(DuckDBResult&& _other)
      : destroy_(_other.destroy_), res_(_other.res_) {
    _other.destroy_ = false;
  }

  DuckDBResult& operator=(const DuckDBResult& _other) = delete;

  DuckDBResult& operator=(DuckDBResult&& _other);

  duckdb_result& res() { return res_; }

 private:
  bool destroy_;

  duckdb_result res_;
};

}  // namespace sqlgen::duckdb

#endif

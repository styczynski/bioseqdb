#ifndef SQLGEN_DUCKDB_DUCKDBAPPENDER_HPP_
#define SQLGEN_DUCKDB_DUCKDBAPPENDER_HPP_

#include <duckdb.h>

#include <string>

#include "../sqlgen_api.hpp"
#include "DuckDBConnection.hpp"

namespace sqlgen::duckdb {

class SQLGEN_API DuckDBAppender {
  using ConnPtr = Ref<DuckDBConnection>;

 public:
  static Result<Ref<DuckDBAppender>> make(
      const std::string& _sql, const ConnPtr& _conn,
      const std::vector<const char*>& _columns,
      const std::vector<duckdb_logical_type>& _types);

  DuckDBAppender(const std::string& _sql, const ConnPtr& _conn,
                 std::vector<const char*> _columns,
                 std::vector<duckdb_logical_type> _types);

  ~DuckDBAppender();

  DuckDBAppender(const DuckDBAppender& _other) = delete;

  DuckDBAppender(DuckDBAppender&& _other)
      : destroy_(_other.destroy_), appender_(_other.appender_) {
    _other.destroy_ = false;
  }

  DuckDBAppender& operator=(const DuckDBAppender& _other) = delete;

  DuckDBAppender& operator=(DuckDBAppender&& _other);

  duckdb_appender& appender() { return appender_; }

  Result<Nothing> close();

 private:
  bool destroy_;

  duckdb_appender appender_;
};

}  // namespace sqlgen::duckdb

#endif

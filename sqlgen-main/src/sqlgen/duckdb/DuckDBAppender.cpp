#include "sqlgen/duckdb/DuckDBAppender.hpp"

namespace sqlgen::duckdb {

Result<Ref<DuckDBAppender>> DuckDBAppender::make(
    const std::string& _sql, const ConnPtr& _conn,
    const std::vector<const char*>& _columns,
    const std::vector<duckdb_logical_type>& _types) {
  try {
    return Ref<DuckDBAppender>::make(_sql, _conn, _columns, _types);
  } catch (const std::exception& e) {
    return error(e.what());
  }
}

DuckDBAppender::DuckDBAppender(const std::string& _sql, const ConnPtr& _conn,
                               std::vector<const char*> _columns,
                               std::vector<duckdb_logical_type> _types)
    : destroy_(false) {
  if (duckdb_appender_create_query(
          _conn->conn(), _sql.c_str(), static_cast<idx_t>(_columns.size()),
          _types.data(), "sqlgen_appended_data", _columns.data(),
          &appender_) == DuckDBError) {
    throw std::runtime_error("Could not create appender.");
  }
  destroy_ = true;
}

DuckDBAppender::~DuckDBAppender() {
  if (destroy_) {
    duckdb_appender_destroy(&appender_);
  }
}

DuckDBAppender& DuckDBAppender::operator=(DuckDBAppender&& _other) {
  if (this == &_other) {
    return *this;
  }
  destroy_ = _other.destroy_;
  appender_ = _other.appender_;
  _other.destroy_ = false;
  return *this;
}

Result<Nothing> DuckDBAppender::close() {
  const auto state = duckdb_appender_close(appender_);
  if (state == DuckDBError) {
    return error(duckdb_appender_error(appender_));
  }
  return Nothing{};
}

}  // namespace sqlgen::duckdb

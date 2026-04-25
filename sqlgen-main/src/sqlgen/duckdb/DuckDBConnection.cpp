#include "sqlgen/duckdb/DuckDBConnection.hpp"

namespace sqlgen::duckdb {

Result<Ref<DuckDBConnection>> DuckDBConnection::make(
    const std::optional<std::string>& _fname) {
  duckdb_database db = NULL;

  const auto res_db =
      _fname ? duckdb_open(_fname->c_str(), &db) : duckdb_open(NULL, &db);

  if (res_db == DuckDBError) {
    duckdb_close(&db);
    return error("Could not open database.");
  }

  duckdb_connection conn = NULL;

  const auto res_conn = duckdb_connect(db, &conn);

  if (res_conn == DuckDBError) {
    duckdb_disconnect(&conn);
    duckdb_close(&db);
    return error("Could not connect to database.");
  }

  return Ref<DuckDBConnection>::make(conn, db);
}

DuckDBConnection& DuckDBConnection::operator=(DuckDBConnection&& _other) {
  if (this == &_other) {
    return *this;
  }
  duckdb_disconnect(&conn_);
  duckdb_close(&db_);
  conn_ = _other.conn_;
  db_ = _other.db_;
  _other.conn_ = NULL;
  _other.db_ = NULL;
  return *this;
}

}  // namespace sqlgen::duckdb

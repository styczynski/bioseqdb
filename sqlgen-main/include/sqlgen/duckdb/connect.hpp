#ifndef SQLGEN_DUCKDB_CONNECT_HPP_
#define SQLGEN_DUCKDB_CONNECT_HPP_

#include <string>

#include "Connection.hpp"

namespace sqlgen::duckdb {

inline auto connect(const std::string& _fname = ":memory:") {
  return Connection::make(_fname);
}

}  // namespace sqlgen::duckdb

#endif

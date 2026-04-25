#ifndef SQLGEN_SQLITE_CONNECT_HPP_
#define SQLGEN_SQLITE_CONNECT_HPP_

#include <string>

#include "Connection.hpp"

namespace sqlgen::sqlite {

inline auto connect(const std::string& _fname = ":memory:") {
  return Connection::make(_fname);
}

}  // namespace sqlgen::sqlite

#endif

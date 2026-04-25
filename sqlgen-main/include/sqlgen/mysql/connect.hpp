#ifndef SQLGEN_MYSQL_CONNECT_HPP_
#define SQLGEN_MYSQL_CONNECT_HPP_

#include <string>

#include "Connection.hpp"
#include "Credentials.hpp"

namespace sqlgen::mysql {

inline auto connect(const Credentials& _credentials) {
  return Connection::make(_credentials);
}

}  // namespace sqlgen::mysql

#endif

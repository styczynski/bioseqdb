#ifndef SQLGEN_POSTGRES_CONNECT_HPP_
#define SQLGEN_POSTGRES_CONNECT_HPP_

#include <string>

#include "Connection.hpp"
#include "Credentials.hpp"

namespace sqlgen::postgres {

inline auto connect(const Credentials& _credentials) {
  return Connection::make(_credentials);
}

}  // namespace sqlgen::postgres

#endif

#ifndef SQLGEN_TESTS_MYSQL_TEST_HELPERS_HPP_
#define SQLGEN_TESTS_MYSQL_TEST_HELPERS_HPP_

#include <cstdlib>
#include <sqlgen/mysql.hpp>

namespace sqlgen::mysql::test {

/// Creates test credentials for MySQL.
/// Set SQLGEN_TEST_PASSWORD environment variable to override the default
/// password ("password"). This allows running tests against an existing
/// database without modifying source code.
inline Credentials make_credentials() {
  const char* env_password = std::getenv("SQLGEN_TEST_PASSWORD");
  return Credentials{.host = "localhost",
                     .user = "sqlgen",
                     .password = env_password ? env_password : "password",
                     .dbname = "mysql"};
}

}

#endif

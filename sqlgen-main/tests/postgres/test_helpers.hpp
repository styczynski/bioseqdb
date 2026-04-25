#ifndef SQLGEN_TESTS_POSTGRES_TEST_HELPERS_HPP_
#define SQLGEN_TESTS_POSTGRES_TEST_HELPERS_HPP_

#include <cstdlib>
#include <sqlgen/postgres.hpp>

namespace sqlgen::postgres::test {

/// Creates test credentials for PostgreSQL.
/// Set SQLGEN_TEST_PASSWORD environment variable to override the default
/// password ("password"). This allows running tests against an existing
/// database without modifying source code.
inline Credentials make_credentials() {
  const char* env_password = std::getenv("SQLGEN_TEST_PASSWORD");
  return Credentials{.user = "postgres",
                     .password = env_password ? env_password : "password",
                     .host = "localhost",
                     .dbname = "postgres"};
}

}

#endif

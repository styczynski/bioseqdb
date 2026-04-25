#ifndef SQLGEN_BUILD_DRY_TESTS_ONLY

#include <gtest/gtest.h>

#include <optional>
#include <sqlgen/postgres.hpp>
#include <string>

namespace test_execute_params {

TEST(postgres, execute_with_string_params) {
  const auto credentials = sqlgen::postgres::Credentials{
      .user = "postgres",
      .password = "password",
      .host = "localhost",
      .dbname = "postgres"};
  auto conn_result = sqlgen::postgres::connect(credentials);
  ASSERT_TRUE(conn_result);
  auto conn = conn_result.value();

  // Create a test table
  auto create_result = conn->execute(R"(
    CREATE TABLE IF NOT EXISTS test_execute_params (
      id SERIAL PRIMARY KEY,
      name TEXT,
      value INTEGER
    );
  )");
  ASSERT_TRUE(create_result) << create_result.error().what();

  // Clean up any existing data
  auto truncate_result = conn->execute("TRUNCATE test_execute_params;");
  ASSERT_TRUE(truncate_result) << truncate_result.error().what();

  // Insert using parameterized execute
  auto insert_result = conn->execute(
      "INSERT INTO test_execute_params (name, value) VALUES ($1, $2);",
      std::string("test_name"), 42);
  ASSERT_TRUE(insert_result) << insert_result.error().what();

  // Clean up
  auto drop_result = conn->execute("DROP TABLE test_execute_params;");
  ASSERT_TRUE(drop_result) << drop_result.error().what();
}

TEST(postgres, execute_with_null_param) {
  const auto credentials = sqlgen::postgres::Credentials{
      .user = "postgres",
      .password = "password",
      .host = "localhost",
      .dbname = "postgres"};
  auto conn_result = sqlgen::postgres::connect(credentials);
  ASSERT_TRUE(conn_result);
  auto conn = conn_result.value();

  // Create a test table
  auto create_result = conn->execute(R"(
    CREATE TABLE IF NOT EXISTS test_execute_null (
      id SERIAL PRIMARY KEY,
      name TEXT
    );
  )");
  ASSERT_TRUE(create_result) << create_result.error().what();

  // Insert with null parameter using std::optional
  std::optional<std::string> null_val = std::nullopt;
  auto insert_result = conn->execute(
      "INSERT INTO test_execute_null (name) VALUES ($1);", null_val);
  ASSERT_TRUE(insert_result) << insert_result.error().what();

  // Clean up
  auto drop_result = conn->execute("DROP TABLE test_execute_null;");
  ASSERT_TRUE(drop_result) << drop_result.error().what();
}

TEST(postgres, execute_with_numeric_params) {
  const auto credentials = sqlgen::postgres::Credentials{
      .user = "postgres",
      .password = "password",
      .host = "localhost",
      .dbname = "postgres"};
  auto conn_result = sqlgen::postgres::connect(credentials);
  ASSERT_TRUE(conn_result);
  auto conn = conn_result.value();

  // Create a test table
  auto create_result = conn->execute(R"(
    CREATE TABLE IF NOT EXISTS test_execute_numeric (
      id SERIAL PRIMARY KEY,
      int_val INTEGER,
      float_val DOUBLE PRECISION,
      bool_val BOOLEAN
    );
  )");
  ASSERT_TRUE(create_result) << create_result.error().what();

  // Insert with various numeric types
  auto insert_result = conn->execute(
      "INSERT INTO test_execute_numeric (int_val, float_val, bool_val) "
      "VALUES ($1, $2, $3);",
      123, 3.14159, true);
  ASSERT_TRUE(insert_result) << insert_result.error().what();

  // Clean up
  auto drop_result = conn->execute("DROP TABLE test_execute_numeric;");
  ASSERT_TRUE(drop_result) << drop_result.error().what();
}

TEST(postgres, execute_call_function) {
  const auto credentials = sqlgen::postgres::Credentials{
      .user = "postgres",
      .password = "password",
      .host = "localhost",
      .dbname = "postgres"};
  auto conn_result = sqlgen::postgres::connect(credentials);
  ASSERT_TRUE(conn_result);
  auto conn = conn_result.value();

  // Create a simple test function
  auto create_fn_result = conn->execute(R"(
    CREATE OR REPLACE FUNCTION test_add(a INTEGER, b INTEGER)
    RETURNS INTEGER AS $$
    BEGIN
      RETURN a + b;
    END;
    $$ LANGUAGE plpgsql;
  )");
  ASSERT_TRUE(create_fn_result) << create_fn_result.error().what();

  // Call the function with parameters
  auto call_result = conn->execute("SELECT test_add($1, $2);", 5, 3);
  ASSERT_TRUE(call_result) << call_result.error().what();

  // Clean up
  auto drop_fn_result = conn->execute("DROP FUNCTION test_add;");
  ASSERT_TRUE(drop_fn_result) << drop_fn_result.error().what();
}

}  // namespace test_execute_params

#endif

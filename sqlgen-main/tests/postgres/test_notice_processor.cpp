#ifndef SQLGEN_BUILD_DRY_TESTS_ONLY

#include <gtest/gtest.h>

#include <mutex>
#include <sqlgen/postgres.hpp>
#include <string>
#include <vector>

namespace test_notice_processor {

TEST(postgres, notice_processor_captures_raise_notice) {
  std::vector<std::string> captured_notices;
  std::mutex mtx;

  const auto credentials = sqlgen::postgres::Credentials{
      .user = "postgres",
      .password = "password",
      .host = "localhost",
      .dbname = "postgres",
      .notice_handler = [&](const char* msg) {
        std::lock_guard<std::mutex> lock(mtx);
        captured_notices.push_back(msg);
      }};

  auto conn_result = sqlgen::postgres::connect(credentials);
  ASSERT_TRUE(conn_result);
  auto conn = conn_result.value();

  // Execute a DO block that raises a notice
  auto result = conn->execute(R"(
    DO $$
    BEGIN
      RAISE NOTICE 'Hello from PL/pgSQL';
    END
    $$;
  )");
  ASSERT_TRUE(result) << result.error().what();

  // Verify notice was captured
  std::lock_guard<std::mutex> lock(mtx);
  ASSERT_EQ(captured_notices.size(), 1);
  EXPECT_TRUE(captured_notices[0].find("Hello from PL/pgSQL") !=
              std::string::npos);
}

TEST(postgres, no_notice_handler_works) {
  // Default behavior - no handler, should not crash
  const auto credentials = sqlgen::postgres::Credentials{
      .user = "postgres",
      .password = "password",
      .host = "localhost",
      .dbname = "postgres"};

  auto conn_result = sqlgen::postgres::connect(credentials);
  ASSERT_TRUE(conn_result);
  auto conn = conn_result.value();

  auto result = conn->execute(R"(
    DO $$
    BEGIN
      RAISE NOTICE 'This goes to stderr by default';
    END
    $$;
  )");
  ASSERT_TRUE(result);
}

TEST(postgres, notice_processor_multiple_notices) {
  std::vector<std::string> captured_notices;
  std::mutex mtx;

  const auto credentials = sqlgen::postgres::Credentials{
      .user = "postgres",
      .password = "password",
      .host = "localhost",
      .dbname = "postgres",
      .notice_handler = [&](const char* msg) {
        std::lock_guard<std::mutex> lock(mtx);
        captured_notices.push_back(msg);
      }};

  auto conn_result = sqlgen::postgres::connect(credentials);
  ASSERT_TRUE(conn_result);
  auto conn = conn_result.value();

  // Execute a DO block that raises multiple notices
  auto result = conn->execute(R"(
    DO $$
    BEGIN
      RAISE NOTICE 'First notice';
      RAISE NOTICE 'Second notice';
      RAISE NOTICE 'Third notice';
    END
    $$;
  )");
  ASSERT_TRUE(result) << result.error().what();

  // Verify all notices were captured
  std::lock_guard<std::mutex> lock(mtx);
  ASSERT_EQ(captured_notices.size(), 3);
  EXPECT_TRUE(captured_notices[0].find("First notice") != std::string::npos);
  EXPECT_TRUE(captured_notices[1].find("Second notice") != std::string::npos);
  EXPECT_TRUE(captured_notices[2].find("Third notice") != std::string::npos);
}

}  // namespace test_notice_processor

#endif

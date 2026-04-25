#include <gtest/gtest.h>

#include <sqlgen.hpp>
#include <sqlgen/dynamic/Insert.hpp>
#include <sqlgen/sqlite.hpp>
#include <sqlgen/transpilation/to_insert_or_write.hpp>

namespace test_to_insert {

struct TestTable {
  std::string field1;
  int32_t field2;
  sqlgen::PrimaryKey<uint32_t> id;
  std::optional<std::string> nullable;
};

TEST(sqlite, test_to_insert) {
  const auto insert_stmt =
      sqlgen::transpilation::to_insert_or_write<TestTable,
                                                sqlgen::dynamic::Insert>();
  const auto conn = sqlgen::sqlite::connect().value();
  const auto expected =
      R"(INSERT INTO "TestTable" ("field1", "field2", "id", "nullable") VALUES (?, ?, ?, ?);)";

  EXPECT_EQ(conn->to_sql(insert_stmt), expected);
}
}  // namespace test_to_insert

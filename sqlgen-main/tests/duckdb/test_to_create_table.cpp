#include <gtest/gtest.h>

#include <sqlgen.hpp>
#include <sqlgen/duckdb.hpp>
#include <sqlgen/transpilation/to_create_table.hpp>

namespace test_to_create_table {

struct TestTable {
  std::string field1;
  int32_t field2;
  sqlgen::PrimaryKey<uint32_t> id;
  std::optional<std::string> nullable;
};

TEST(duckdb, test_to_create_table) {
  const auto create_table_stmt =
      sqlgen::transpilation::to_create_table<TestTable>();
  const auto conn = sqlgen::duckdb::connect().value();
  const auto expected =
      R"(CREATE TABLE IF NOT EXISTS "TestTable"("field1" TEXT NOT NULL, "field2" INTEGER NOT NULL, "id" UINTEGER NOT NULL, "nullable" TEXT, PRIMARY KEY ("id"));)";

  EXPECT_EQ(conn->to_sql(create_table_stmt), expected);
}
}  // namespace test_to_create_table

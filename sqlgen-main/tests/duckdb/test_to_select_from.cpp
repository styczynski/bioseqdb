#include <gtest/gtest.h>

#include <sqlgen.hpp>
#include <sqlgen/duckdb.hpp>

namespace test_to_select_from {

struct TestTable {
  std::string field1;
  int32_t field2;
  sqlgen::PrimaryKey<uint32_t> id;
  std::optional<std::string> nullable;
};

TEST(duckdb, test_to_select_from) {
  const auto select_from_stmt =
      sqlgen::transpilation::read_to_select_from<TestTable>();
  const auto conn = sqlgen::duckdb::connect().value();
  const auto expected =
      R"(SELECT "field1", "field2", "id", "nullable" FROM "TestTable")";

  EXPECT_EQ(conn->to_sql(select_from_stmt), expected);
}
}  // namespace test_to_select_from

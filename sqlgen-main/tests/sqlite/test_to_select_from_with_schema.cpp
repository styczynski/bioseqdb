#include <gtest/gtest.h>

#include <sqlgen.hpp>
#include <sqlgen/sqlite.hpp>

namespace test_to_select_from_with_schema {

struct TestTable {
  constexpr static const char* tablename = "test_table";
  constexpr static const char* schema = "my_schema";

  std::string field1;
  int32_t field2;
  sqlgen::PrimaryKey<uint32_t> id;
  std::optional<std::string> nullable;
};

TEST(sqlite, test_to_select_from_with_schema) {
  const auto select_from_stmt =
      sqlgen::transpilation::read_to_select_from<TestTable>();
  const auto conn = sqlgen::sqlite::connect().value();
  const auto expected =
      R"(SELECT "field1", "field2", "id", "nullable" FROM "my_schema"."test_table")";

  EXPECT_EQ(conn->to_sql(select_from_stmt), expected);
}
}  // namespace test_to_select_from_with_schema

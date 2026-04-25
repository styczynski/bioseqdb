#include <gtest/gtest.h>

#include <sqlgen.hpp>
#include <sqlgen/duckdb.hpp>
#include <sqlgen/dynamic/Insert.hpp>
#include <sqlgen/transpilation/to_insert_or_write.hpp>

namespace test_to_insert_or_replace {

struct TestTable {
  std::string field1;
  int32_t field2;
  sqlgen::Unique<std::string> field3;
  sqlgen::PrimaryKey<uint32_t> id;
  std::optional<std::string> nullable;
};

TEST(duckdb, test_to_insert_or_replace) {
  static_assert(sqlgen::internal::has_constraint_v<TestTable>,
                "The table must have a primary key or unique column for "
                "insert_or_replace(...) to work.");

  const auto insert_stmt =
      sqlgen::transpilation::to_insert_or_write<TestTable,
                                                sqlgen::dynamic::Insert>(true);
  const auto conn = sqlgen::duckdb::connect().value();

  const auto expected =
      R"(INSERT OR REPLACE INTO "TestTable" BY NAME ( SELECT "field1" AS "field1", "field2" AS "field2", "field3" AS "field3", "id" AS "id", "nullable" AS "nullable" FROM sqlgen_appended_data);)";

  EXPECT_EQ(conn->to_sql(insert_stmt), expected);
}
}  // namespace test_to_insert_or_replace

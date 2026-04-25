#include <gtest/gtest.h>

#include <sqlgen.hpp>
#include <sqlgen/duckdb.hpp>
#include <sqlgen/dynamic/Insert.hpp>
#include <sqlgen/transpilation/to_insert_or_write.hpp>

namespace test_to_insert {

struct TestTable {
  std::string field1;
  int32_t field2;
  sqlgen::PrimaryKey<uint32_t> id;
  std::optional<std::string> nullable;
};

TEST(duckdb, test_to_insert) {
  const auto insert_stmt =
      sqlgen::transpilation::to_insert_or_write<TestTable,
                                                sqlgen::dynamic::Insert>();
  const auto conn = sqlgen::duckdb::connect().value();
  const auto expected =
      R"(INSERT INTO "TestTable" BY NAME ( SELECT "field1" AS "field1", "field2" AS "field2", "id" AS "id", "nullable" AS "nullable" FROM sqlgen_appended_data);)";

  EXPECT_EQ(conn->to_sql(insert_stmt), expected);
}
}  // namespace test_to_insert

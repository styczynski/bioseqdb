#include <gtest/gtest.h>

#include <sqlgen.hpp>
#include <sqlgen/dynamic/Insert.hpp>
#include <sqlgen/sqlite.hpp>
#include <sqlgen/transpilation/to_insert_or_write.hpp>

namespace test_to_insert_or_replace {

struct TestTable {
  std::string field1;
  int32_t field2;
  sqlgen::Unique<std::string> field3;
  sqlgen::PrimaryKey<uint32_t> id;
  std::optional<std::string> nullable;
};

TEST(sqlite, test_to_insert_or_replace) {
  static_assert(sqlgen::internal::has_constraint_v<TestTable>,
                "The table must have a primary key or unique column for "
                "insert_or_replace(...) to work.");

  const auto insert_stmt =
      sqlgen::transpilation::to_insert_or_write<TestTable,
                                                sqlgen::dynamic::Insert>(true);
  const auto conn = sqlgen::sqlite::connect().value();
  const auto expected =
      R"(INSERT INTO "TestTable" ("field1", "field2", "field3", "id", "nullable") VALUES (?, ?, ?, ?, ?) ON CONFLICT (field3, id) DO UPDATE SET field1=excluded.field1, field2=excluded.field2, field3=excluded.field3, id=excluded.id, nullable=excluded.nullable;)";

  EXPECT_EQ(conn->to_sql(insert_stmt), expected);
}
}  // namespace test_to_insert_or_replace

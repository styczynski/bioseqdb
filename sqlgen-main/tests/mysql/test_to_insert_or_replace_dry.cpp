#include <gtest/gtest.h>

#include <sqlgen.hpp>
#include <sqlgen/dynamic/Insert.hpp>
#include <sqlgen/mysql.hpp>
#include <sqlgen/transpilation/to_insert_or_write.hpp>

namespace test_to_insert_or_replace_dry {

struct TestTable {
  std::string field1;
  int32_t field2;
  sqlgen::Unique<std::string> field3;
  sqlgen::PrimaryKey<uint32_t> id;
  std::optional<std::string> nullable;
};

TEST(mysql, test_to_insert_or_replace_dry) {
  static_assert(sqlgen::internal::has_constraint_v<TestTable>,
                "The table must have a primary key or unique column for "
                "insert_or_replace(...) to work.");

  const auto insert_stmt =
      sqlgen::dynamic::Statement(sqlgen::transpilation::to_insert_or_write<TestTable,
                                                sqlgen::dynamic::Insert>(true));
  const auto expected =
      R"(INSERT INTO `TestTable` (`field1`, `field2`, `field3`, `id`, `nullable`) VALUES (?, ?, ?, ?, ?) ON DUPLICATE KEY UPDATE field1=VALUES(field1), field2=VALUES(field2), field3=VALUES(field3), id=VALUES(id), nullable=VALUES(nullable);)";

  EXPECT_EQ(sqlgen::mysql::to_sql(insert_stmt), expected);
}
}  // namespace test_to_insert_or_replace_dry
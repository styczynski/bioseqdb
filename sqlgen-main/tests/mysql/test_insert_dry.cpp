#include <gtest/gtest.h>

#include <sqlgen.hpp>
#include <sqlgen/mysql.hpp>

namespace test_insert_dry {

struct TestTable {
  std::string field1;
  int32_t field2;
  sqlgen::PrimaryKey<uint32_t> id;
  std::optional<std::string> nullable;
};

TEST(mysql, test_insert_dry) {
  const auto query = sqlgen::Insert<TestTable>{};

  const auto expected =
      R"(INSERT INTO `TestTable` (`field1`, `field2`, `id`, `nullable`) VALUES (?, ?, ?, ?);)";

  EXPECT_EQ(sqlgen::mysql::to_sql(query), expected);
}
}  // namespace test_insert_dry

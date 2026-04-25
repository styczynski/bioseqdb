#include <gtest/gtest.h>

#include <sqlgen.hpp>
#include <sqlgen/mysql.hpp>

namespace test_create_index_dry {

struct TestTable {
  std::string field1;
  int32_t field2;
  sqlgen::PrimaryKey<uint32_t> id;
  std::optional<std::string> nullable;
};

TEST(mysql, test_create_index_dry) {
  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto query =
      create_index<"test_table_ix", TestTable>("field1"_c, "field2"_c);

  const auto expected =
      R"(CREATE INDEX `test_table_ix` ON `TestTable`(`field1`, `field2`);)";

  EXPECT_EQ(sqlgen::mysql::to_sql(query), expected);
}
}  // namespace test_create_index_dry

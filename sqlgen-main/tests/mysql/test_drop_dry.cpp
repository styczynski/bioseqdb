#include <gtest/gtest.h>

#include <sqlgen.hpp>
#include <sqlgen/mysql.hpp>

namespace test_drop_dry {

struct TestTable {
  std::string field1;
  int32_t field2;
  sqlgen::PrimaryKey<uint32_t> id;
  std::optional<std::string> nullable;
};

TEST(mysql, test_drop_dry) {
  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto query = drop<TestTable> | if_exists;

  const auto expected = R"(DROP TABLE IF EXISTS `TestTable`;)";

  EXPECT_EQ(sqlgen::mysql::to_sql(query), expected);
}
}  // namespace test_drop_dry

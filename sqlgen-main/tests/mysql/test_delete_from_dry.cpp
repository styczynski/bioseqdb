#include <gtest/gtest.h>

#include <sqlgen.hpp>
#include <sqlgen/mysql.hpp>

namespace test_delete_from_dry {

struct TestTable {
  std::string field1;
  int32_t field2;
  sqlgen::PrimaryKey<uint32_t> id;
  std::optional<std::string> nullable;
};

TEST(mysql, test_delete_from_dry) {
  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto query = delete_from<TestTable> | where("field2"_c > 0);

  const auto expected = R"(DELETE FROM `TestTable` WHERE `field2` > 0;)";

  EXPECT_EQ(sqlgen::mysql::to_sql(query), expected);
}
}  // namespace test_delete_from_dry

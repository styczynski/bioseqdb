#include <gtest/gtest.h>

#include <sqlgen.hpp>
#include <sqlgen/mysql.hpp>

namespace test_to_select_from_with_schema_dry {

struct TestTable {
  constexpr static const char* tablename = "test_table";
  constexpr static const char* schema = "my_schema";

  std::string field1;
  int32_t field2;
  sqlgen::PrimaryKey<uint32_t> id;
  std::optional<std::string> nullable;
};

TEST(mysql, test_to_select_from_with_schema_dry) {
  const auto query = sqlgen::read<std::vector<TestTable>>;

  const auto expected =
      R"(SELECT `field1`, `field2`, `id`, `nullable` FROM `my_schema`.`test_table`)";

  EXPECT_EQ(sqlgen::mysql::to_sql(query), expected);
}
}  // namespace test_to_select_from_with_schema_dry

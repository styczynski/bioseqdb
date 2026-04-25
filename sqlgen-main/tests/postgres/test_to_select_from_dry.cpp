#include <gtest/gtest.h>

#include <sqlgen.hpp>
#include <sqlgen/postgres.hpp>

namespace test_to_select_from_dry {

struct TestTable {
  std::string field1;
  int32_t field2;
  sqlgen::PrimaryKey<uint32_t> id;
  std::optional<std::string> nullable;
};

TEST(postgres, test_to_select_from_dry) {
  const auto query = sqlgen::read<std::vector<TestTable>>;

  const auto expected =
      R"(SELECT "field1", "field2", "id", "nullable" FROM "TestTable")";

  EXPECT_EQ(sqlgen::postgres::to_sql(query), expected);
}
}  // namespace test_to_select_from_dry

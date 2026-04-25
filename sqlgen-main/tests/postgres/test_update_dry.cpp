#include <gtest/gtest.h>

#include <sqlgen.hpp>
#include <sqlgen/postgres.hpp>

namespace test_update_dry {

struct TestTable {
  std::string field1;
  int32_t field2;
  sqlgen::PrimaryKey<uint32_t> id;
  std::optional<std::string> nullable;
};

TEST(postgres, test_update_dry) {
  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto query =
      update<TestTable>("field1"_c.set("Hello"), "nullable"_c.set("field1"_c)) |
      where("field2"_c > 0);

  const auto expected =
      R"(UPDATE "TestTable" SET "field1" = 'Hello', "nullable" = "field1" WHERE "field2" > 0;)";

  EXPECT_EQ(sqlgen::postgres::to_sql(query), expected);
}
}  // namespace test_update_dry

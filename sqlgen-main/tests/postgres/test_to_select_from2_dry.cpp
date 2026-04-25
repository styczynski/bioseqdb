#include <gtest/gtest.h>

#include <sqlgen.hpp>
#include <sqlgen/postgres.hpp>

namespace test_to_select_from2_dry {

struct TestTable {
  std::string field1;
  int32_t field2;
  sqlgen::PrimaryKey<uint32_t> id;
  std::optional<std::string> nullable;
};

TEST(postgres, test_to_select_from2_dry) {
  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto query =
      select_from<TestTable>("field1"_c.as<"field">(),
                             avg("field2"_c).as<"avg_field2">(),
                             "nullable"_c | as<"nullable_field">, as<"one">(1),
                             "hello" | as<"hello">) |
      where("id"_c > 0) | group_by("field1"_c, "nullable"_c) |
      order_by("field1"_c) | limit(10);

  const auto expected =
      R"(SELECT "field1" AS "field", AVG("field2") AS "avg_field2", "nullable" AS "nullable_field", 1 AS "one", 'hello' AS "hello" FROM "TestTable" WHERE "id" > 0 GROUP BY "field1", "nullable" ORDER BY "field1" LIMIT 10)";

  EXPECT_EQ(sqlgen::postgres::to_sql(query), expected);
}
}  // namespace test_to_select_from2_dry

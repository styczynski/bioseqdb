#include <gtest/gtest.h>

#include <sqlgen.hpp>
#include <sqlgen/postgres.hpp>

namespace test_insert_dry {

struct TestTable {
  std::string field1;
  int32_t field2;
  sqlgen::PrimaryKey<uint32_t> id;
  std::optional<std::string> nullable;
};

TEST(postgres, test_insert_dry) {
  const auto query = sqlgen::Insert<TestTable>{};

  const auto expected =
      R"(INSERT INTO "TestTable" ("field1", "field2", "id", "nullable") VALUES ($1, $2, $3, $4);)";

  EXPECT_EQ(sqlgen::postgres::to_sql(query), expected);
}
}  // namespace test_insert_dry

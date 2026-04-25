#include <gtest/gtest.h>

#include <sqlgen.hpp>
#include <sqlgen/postgres.hpp>

namespace test_timestamp_with_tz_dry {

struct TestTable {
  std::string field1;
  int32_t field2;
  sqlgen::PrimaryKey<uint32_t> id;
  sqlgen::Timestamp<"%Y-%m-%d %H:%M:%S%z"> ts;
};

TEST(postgres, test_timestamp_with_tz_dry) {
  const auto query = sqlgen::CreateTable<TestTable>{};

  const auto expected =
      R"(CREATE TABLE IF NOT EXISTS "TestTable" ("field1" TEXT NOT NULL, "field2" INTEGER NOT NULL, "id" INTEGER NOT NULL, "ts" TIMESTAMP WITH TIME ZONE NOT NULL, PRIMARY KEY ("id"));)";

  EXPECT_EQ(sqlgen::postgres::to_sql(query), expected);
}
}  // namespace test_timestamp_with_tz_dry

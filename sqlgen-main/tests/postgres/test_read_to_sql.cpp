#include <gtest/gtest.h>

#include <iostream>
#include <sqlgen.hpp>
#include <sqlgen/postgres.hpp>
#include <vector>

namespace test_write_and_read_dry {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  int age;
};

TEST(postgres, test_write_and_read_dry) {
  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto query1 =
      postgres::to_sql(sqlgen::read<std::vector<Person>> | where("id"_c == 1));
  const auto query2 =
      postgres::to_sql(sqlgen::read<Person> | where("id"_c == 1));

  EXPECT_EQ(
      query1,
      R"(SELECT "id", "first_name", "last_name", "age" FROM "Person" WHERE "id" = 1)");
  EXPECT_EQ(
      query2,
      R"(SELECT "id", "first_name", "last_name", "age" FROM "Person" WHERE "id" = 1)");
}

}  // namespace test_write_and_read_dry


#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/postgres.hpp>
#include <vector>

namespace test_is_null_dry {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  std::optional<int> age;
};

TEST(postgres, test_is_null_dry) {
  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto sql = postgres::to_sql(sqlgen::read<std::vector<Person>> |
                                    where("age"_c.is_null()) |
                                    order_by("first_name"_c.desc()));

  const std::string expected =
      R"(SELECT "id", "first_name", "last_name", "age" FROM "Person" WHERE "age" IS NULL ORDER BY "first_name" DESC)";

  EXPECT_EQ(sql, expected);
}

}  // namespace test_is_null_dry


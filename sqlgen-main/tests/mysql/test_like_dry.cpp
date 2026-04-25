#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/mysql.hpp>
#include <vector>

namespace test_like_dry {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  std::optional<int> age;
};

TEST(mysql, test_like_dry) {
  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto sql =
      mysql::to_sql(sqlgen::read<std::vector<Person>> |
                    where("first_name"_c.like("H%")) | order_by("age"_c));

  const std::string expected =
      R"(SELECT `id`, `first_name`, `last_name`, `age` FROM `Person` WHERE `first_name` LIKE 'H%' ORDER BY `age`)";

  EXPECT_EQ(sql, expected);
}

}  // namespace test_like_dry

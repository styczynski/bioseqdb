#include <gtest/gtest.h>

#include <sqlgen.hpp>
#include <sqlgen/postgres.hpp>
#include <vector>

namespace test_where_dry {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  int age;
};

TEST(postgres, test_where_dry) {
  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto query = sqlgen::read<std::vector<Person>> |
                     where("age"_c < 18 and "first_name"_c != "Hugo") |
                     order_by("age"_c);

  const auto expected =
      R"(SELECT "id", "first_name", "last_name", "age" FROM "Person" WHERE ("age" < 18) AND ("first_name" != 'Hugo') ORDER BY "age")";

  EXPECT_EQ(sqlgen::postgres::to_sql(query), expected);
}

}  // namespace test_where_dry

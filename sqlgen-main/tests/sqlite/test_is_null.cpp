
#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/sqlite.hpp>
#include <vector>

namespace test_is_null {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  std::optional<int> age;
};

TEST(sqlite, test_is_null) {
  const auto people1 = std::vector<Person>(
      {Person{.id = 0, .first_name = "Homer", .last_name = "Simpson"},
       Person{.id = 1, .first_name = "Bart", .last_name = "Simpson", .age = 10},
       Person{.id = 2, .first_name = "Lisa", .last_name = "Simpson", .age = 8},
       Person{
           .id = 3, .first_name = "Maggie", .last_name = "Simpson", .age = 0},
       Person{.id = 4, .first_name = "Hugo", .last_name = "Simpson"}});

  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto conn = sqlite::connect();

  const auto people2 =
      conn.and_then(write(std::ref(people1)))
          .and_then(sqlgen::read<std::vector<Person>> |
                    where("age"_c.is_null()) | order_by("first_name"_c.desc()))
          .value();

  const auto people3 =
      conn.and_then(sqlgen::read<std::vector<Person>> |
                    where("age"_c.is_not_null()) | order_by("age"_c))
          .value();

  const std::string expected1 =
      R"([{"id":4,"first_name":"Hugo","last_name":"Simpson"},{"id":0,"first_name":"Homer","last_name":"Simpson"}])";

  const std::string expected2 =
      R"([{"id":3,"first_name":"Maggie","last_name":"Simpson","age":0},{"id":2,"first_name":"Lisa","last_name":"Simpson","age":8},{"id":1,"first_name":"Bart","last_name":"Simpson","age":10}])";

  EXPECT_EQ(rfl::json::write(people2), expected1);
  EXPECT_EQ(rfl::json::write(people3), expected2);
}

}  // namespace test_is_null


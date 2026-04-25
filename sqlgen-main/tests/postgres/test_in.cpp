#ifndef SQLGEN_BUILD_DRY_TESTS_ONLY

#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/postgres.hpp>
#include <vector>
#include "test_helpers.hpp"

namespace test_in {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  int age;
};

TEST(postgres, test_in) {
  const auto people1 = std::vector<Person>(
      {Person{
           .id = 0, .first_name = "Homer", .last_name = "Simpson", .age = 45},
       Person{.id = 1, .first_name = "Bart", .last_name = "Simpson", .age = 10},
       Person{.id = 2, .first_name = "Lisa", .last_name = "Simpson", .age = 8},
       Person{
           .id = 3, .first_name = "Maggie", .last_name = "Simpson", .age = 0},
       Person{
           .id = 4, .first_name = "Hugo", .last_name = "Simpson", .age = 10}});

  const auto credentials = sqlgen::postgres::test::make_credentials();

  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto people2 =
      postgres::connect(credentials)
          .and_then(drop<Person> | if_exists)
          .and_then(write(std::ref(people1)))
          .and_then(sqlgen::read<std::vector<Person>> |
                    where("first_name"_c.in("Bart", "Lisa", "Maggie")) |
                    order_by("age"_c))
          .value();

  const std::string expected1 =
      R"([{"id":3,"first_name":"Maggie","last_name":"Simpson","age":0},{"id":2,"first_name":"Lisa","last_name":"Simpson","age":8},{"id":1,"first_name":"Bart","last_name":"Simpson","age":10}])";

  EXPECT_EQ(rfl::json::write(people2), expected1);
}

}  // namespace test_in

#endif

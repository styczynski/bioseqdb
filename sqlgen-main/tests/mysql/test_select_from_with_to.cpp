#ifndef SQLGEN_BUILD_DRY_TESTS_ONLY

#include <gtest/gtest.h>

#include <ranges>
#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/mysql.hpp>
#include <vector>
#include "test_helpers.hpp"

namespace test_range_select_from_with_to {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  int age;
};

TEST(mysql, test_range_select_from) {
  const auto people1 = std::vector<Person>(
      {Person{
           .id = 0, .first_name = "Homer", .last_name = "Simpson", .age = 45},
       Person{.id = 1, .first_name = "Bart", .last_name = "Simpson", .age = 10},
       Person{.id = 2, .first_name = "Lisa", .last_name = "Simpson", .age = 8},
       Person{
           .id = 3, .first_name = "Maggie", .last_name = "Simpson", .age = 0}});

  const auto credentials = sqlgen::mysql::test::make_credentials();

  using namespace sqlgen;
  using namespace sqlgen::literals;

  struct FirstName {
    std::string first_name;
  };

  const auto people2 =
      mysql::connect(credentials)
          .and_then(drop<Person> | if_exists)
          .and_then(write(std::ref(people1)))
          .and_then(select_from<Person>("first_name"_c) | order_by("id"_c) |
                    to<std::vector<FirstName>>)
          .value();

  EXPECT_EQ(people2.at(0).first_name, "Homer");
  EXPECT_EQ(people2.at(1).first_name, "Bart");
  EXPECT_EQ(people2.at(2).first_name, "Lisa");
  EXPECT_EQ(people2.at(3).first_name, "Maggie");
}

}  // namespace test_range_select_from_with_to

#endif

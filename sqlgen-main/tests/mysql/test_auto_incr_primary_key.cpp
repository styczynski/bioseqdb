#ifndef SQLGEN_BUILD_DRY_TESTS_ONLY

#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/mysql.hpp>
#include <vector>
#include "test_helpers.hpp"

namespace test_auto_incr_primary_key {

struct Person {
  sqlgen::PrimaryKey<uint32_t, sqlgen::auto_incr> id;
  std::string first_name;
  std::string last_name;
  int age;
};

TEST(mysql, test_auto_incr_primary_key) {
  auto people1 = std::vector<Person>(
      {Person{.first_name = "Homer", .last_name = "Simpson", .age = 45},
       Person{.first_name = "Bart", .last_name = "Simpson", .age = 10},
       Person{.first_name = "Lisa", .last_name = "Simpson", .age = 8},
       Person{.first_name = "Maggie", .last_name = "Simpson", .age = 0}});

  const auto credentials = sqlgen::mysql::test::make_credentials();

  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto people2 = mysql::connect(credentials)
                           .and_then(drop<Person> | if_exists)
                           .and_then(write(std::ref(people1)))
                           .and_then(sqlgen::read<std::vector<Person>> |
                                     order_by("age"_c.desc()))
                           .value();

  people1.at(0).id = 1;
  people1.at(1).id = 2;
  people1.at(2).id = 3;
  people1.at(3).id = 4;

  const auto json1 = rfl::json::write(people1);
  const auto json2 = rfl::json::write(people2);

  EXPECT_EQ(json1, json2);
}

}  // namespace test_auto_incr_primary_key

#endif

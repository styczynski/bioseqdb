#ifndef SQLGEN_BUILD_DRY_TESTS_ONLY

#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/mysql.hpp>
#include <vector>
#include "test_helpers.hpp"

namespace test_boolean_conditions {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  bool has_children;
};

TEST(mysql, test_boolean_conditions) {
  const auto people1 = std::vector<Person>({Person{.id = 0,
                                                   .first_name = "Homer",
                                                   .last_name = "Simpson",
                                                   .has_children = true},
                                            Person{.id = 1,
                                                   .first_name = "Bart",
                                                   .last_name = "Simpson",
                                                   .has_children = false},
                                            Person{.id = 2,
                                                   .first_name = "Lisa",
                                                   .last_name = "Simpson",
                                                   .has_children = false},
                                            Person{.id = 3,
                                                   .first_name = "Maggie",
                                                   .last_name = "Simpson",
                                                   .has_children = false}});

  const auto credentials = sqlgen::mysql::test::make_credentials();

  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto conn =
      mysql::connect(credentials).and_then(drop<Person> | if_exists);

  const auto homer =
      sqlgen::write(conn, people1)
          .and_then(sqlgen::read<Person> | where("has_children"_c == true) |
                    order_by("id"_c))
          .value();

  const auto json1 = rfl::json::write(people1.at(0));
  const auto json2 = rfl::json::write(homer);

  EXPECT_EQ(json1, json2);
}

}  // namespace test_boolean_conditions

#endif

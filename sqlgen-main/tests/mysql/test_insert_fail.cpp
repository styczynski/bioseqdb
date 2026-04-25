#ifndef SQLGEN_BUILD_DRY_TESTS_ONLY

#include <gtest/gtest.h>

#include <ranges>
#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/mysql.hpp>
#include <vector>
#include "test_helpers.hpp"

namespace test_insert_fail {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  int age;
};

TEST(mysql, test_insert_fail) {
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

  const auto conn = mysql::connect(credentials)
                        .and_then(drop<Person> | if_exists)
                        .and_then(write(people1.at(0)));

  const auto res = conn.and_then(begin_transaction)
                       .and_then(insert(people1.at(0)))
                       .and_then(commit);

  // Should fail - duplicate key violation.
  EXPECT_FALSE(res && true);

  const auto people2 =
      conn.and_then(begin_transaction)
          .and_then(insert(people1 | std::ranges::views::drop(1)))
          .and_then(commit)
          .and_then(sqlgen::read<std::vector<Person>>)
          .value();

  const auto json1 = rfl::json::write(people1);
  const auto json2 = rfl::json::write(people2);

  EXPECT_EQ(json1, json2);
}

}  // namespace test_insert_fail

#endif

#ifndef SQLGEN_BUILD_DRY_TESTS_ONLY

#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/postgres.hpp>
#include <vector>
#include "test_helpers.hpp"

namespace test_create_table {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  int age;
};

TEST(postgres, test_create_table) {
  const auto credentials = sqlgen::postgres::test::make_credentials();

  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto people = sqlgen::postgres::connect(credentials)
                          .and_then(drop<Person> | if_exists)
                          .and_then(create_table<Person> | if_not_exists)
                          .and_then(sqlgen::read<std::vector<Person>>)
                          .value();

  const std::string expected = R"([])";

  EXPECT_EQ(rfl::json::write(people), expected);
}

}  // namespace test_create_table

#endif

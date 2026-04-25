#ifndef SQLGEN_BUILD_DRY_TESTS_ONLY

#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/mysql.hpp>
#include <vector>
#include "test_helpers.hpp"

namespace test_create_index {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  sqlgen::Varchar<100> first_name;
  sqlgen::Varchar<100> last_name;
  int age;
};

TEST(mysql, test_create_index) {
  const auto credentials = sqlgen::mysql::test::make_credentials();

  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto people = sqlgen::mysql::connect(credentials)
                          .and_then(drop<Person> | if_exists)
                          .and_then(create_table<Person> | if_not_exists)
                          .and_then(create_index<"test_table_ix", Person>(
                              "first_name"_c, "last_name"_c))
                          .and_then(sqlgen::read<std::vector<Person>>)
                          .value();

  const std::string expected = R"([])";

  EXPECT_EQ(rfl::json::write(people), expected);
}

}  // namespace test_create_index

#endif

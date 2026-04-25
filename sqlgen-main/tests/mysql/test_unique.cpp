#ifndef SQLGEN_BUILD_DRY_TESTS_ONLY

#include <gtest/gtest.h>

#include <ranges>
#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/mysql.hpp>
#include <vector>
#include "test_helpers.hpp"

namespace test_unique {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  sqlgen::Unique<double> age;
};

TEST(mysql, test_unique) {
  const auto people = std::vector<Person>(
      {Person{
           .id = 0, .first_name = "Homer", .last_name = "Simpson", .age = 45},
       Person{
           .id = 1, .first_name = "Marge", .last_name = "Simpson", .age = 40},
       Person{.id = 2, .first_name = "Bart", .last_name = "Simpson", .age = 10},
       Person{.id = 3, .first_name = "Lisa", .last_name = "Simpson", .age = 8},
       Person{
           .id = 4, .first_name = "Maggie", .last_name = "Simpson", .age = 0}});

  const auto credentials = sqlgen::mysql::test::make_credentials();

  using namespace sqlgen;
  using namespace sqlgen::literals;

  mysql::connect(credentials)
      .and_then(drop<Person> | if_exists)
      .and_then(create_table<Person>)
      .and_then(insert(std::ref(people)))
      .value();

  const std::string expected_query =
      R"(CREATE TABLE IF NOT EXISTS `Person` (`id` BIGINT NOT NULL, `first_name` TEXT NOT NULL, `last_name` TEXT NOT NULL, `age` DECIMAL NOT NULL UNIQUE, PRIMARY KEY (`id`));)";

  EXPECT_EQ(mysql::to_sql(create_table<Person>), expected_query);
}

}  // namespace test_unique

#endif

#ifndef SQLGEN_BUILD_DRY_TESTS_ONLY

#include <gtest/gtest.h>

#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/mysql.hpp>
#include "test_helpers.hpp"

namespace test_timestamp {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  sqlgen::Timestamp<"%Y-%m-%d %H:%M:%S"> ts;
};

TEST(mysql, test_timestamp) {
  const auto people1 =
      std::vector<Person>({Person{.id = 0,
                                  .first_name = "Homer",
                                  .last_name = "Simpson",
                                  .ts = "2000-01-01 01:00:00"}});

  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto credentials = sqlgen::mysql::test::make_credentials();

  const auto conn =
      sqlgen::mysql::connect(credentials).and_then(drop<Person> | if_exists);

  const auto people2 = sqlgen::write(conn, people1)
                           .and_then(sqlgen::read<std::vector<Person>>)
                           .value();

  const auto json1 = rfl::json::write(people1);
  const auto json2 = rfl::json::write(people2);

  EXPECT_EQ(json1, json2);
}

}  // namespace test_timestamp

#endif

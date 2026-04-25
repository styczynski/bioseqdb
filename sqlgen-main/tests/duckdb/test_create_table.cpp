#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/duckdb.hpp>
#include <vector>

namespace test_create_table {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  int age;
};

TEST(duckdb, test_create_table) {
  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto people = sqlgen::duckdb::connect()
                          .and_then(create_table<Person> | if_not_exists)
                          .and_then(sqlgen::read<std::vector<Person>>);

  const std::string expected = R"([])";

  EXPECT_EQ(rfl::json::write(people), expected);
}

}  // namespace test_create_table

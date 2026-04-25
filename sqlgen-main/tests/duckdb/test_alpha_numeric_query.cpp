#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/duckdb.hpp>
#include <vector>

namespace test_alpha_numeric_query {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  sqlgen::AlphaNumeric first_name;
  sqlgen::AlphaNumeric last_name;
  int age;
};

sqlgen::Result<std::vector<Person>> get_people(
    const auto& _conn, const sqlgen::AlphaNumeric& _first_name) {
  using namespace sqlgen;
  using namespace sqlgen::literals;
  const auto query =
      sqlgen::read<std::vector<Person>> | where("first_name"_c == _first_name);
  return query(_conn);
}

TEST(duckdb, test_alpha_numeric_query) {
  const auto people1 = std::vector<Person>({Person{
      .id = 0, .first_name = "Homer", .last_name = "Simpson", .age = 45}});

  const auto conn = sqlgen::duckdb::connect();

  sqlgen::write(conn, people1);

  const auto people2 = sqlgen::AlphaNumeric::from_value("Homer")
                           .and_then([&](const auto& _first_name) {
                             return get_people(conn, _first_name);
                           })
                           .value();

  const auto json1 = rfl::json::write(people1);
  const auto json2 = rfl::json::write(people2);

  EXPECT_EQ(json1, json2);
}

}  // namespace test_alpha_numeric_query

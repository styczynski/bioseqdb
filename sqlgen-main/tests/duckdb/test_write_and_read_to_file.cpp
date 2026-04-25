#include <gtest/gtest.h>

#include <cstdio>
#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/duckdb.hpp>
#include <vector>

namespace test_write_and_read_to_file {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  int age;
};

TEST(duckdb, test_write_and_read_to_file) {
  const auto people1 = std::vector<Person>(
      {Person{
           .id = 0, .first_name = "Homer", .last_name = "Simpson", .age = 45},
       Person{.id = 1, .first_name = "Bart", .last_name = "Simpson", .age = 10},
       Person{.id = 2, .first_name = "Lisa", .last_name = "Simpson", .age = 8},
       Person{
           .id = 3, .first_name = "Maggie", .last_name = "Simpson", .age = 0}});

  sqlgen::duckdb::connect("test.db")
      .and_then([&](auto&& _conn) { return sqlgen::write(_conn, people1); })
      .value();

  const auto people2 = sqlgen::duckdb::connect("test.db")
                           .and_then(sqlgen::read<std::vector<Person>>)
                           .value();

  std::remove("test.db");

  const auto json1 = rfl::json::write(people1);
  const auto json2 = rfl::json::write(people2);

  EXPECT_EQ(json1, json2);
}

}  // namespace test_write_and_read_to_file

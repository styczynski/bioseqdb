#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/duckdb.hpp>
#include <vector>

namespace test_create_table_as {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  int age;
};

struct Name {
  std::string first_name;
  std::string last_name;
};

TEST(duckdb, test_create_table_as) {
  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto people = std::vector<Person>(
      {Person{
           .id = 0, .first_name = "Homer", .last_name = "Simpson", .age = 45},
       Person{
           .id = 1, .first_name = "Marge", .last_name = "Simpson", .age = 40},
       Person{.id = 2, .first_name = "Bart", .last_name = "Simpson", .age = 10},
       Person{.id = 3, .first_name = "Lisa", .last_name = "Simpson", .age = 8},
       Person{
           .id = 4, .first_name = "Maggie", .last_name = "Simpson", .age = 0}});

  const auto names_query = select_from<Person>("first_name"_c, "last_name"_c);

  const auto get_names = create_as<Name>(names_query) | if_not_exists;

  const auto names = sqlgen::duckdb::connect()
                         .and_then(drop<Name> | if_exists)
                         .and_then(drop<Person> | if_exists)
                         .and_then(write(std::ref(people)))
                         .and_then(get_names)
                         .and_then(sqlgen::read<std::vector<Name>>)
                         .value();

  const std::string expected_query =
      R"(CREATE TABLE IF NOT EXISTS "Name" AS SELECT "first_name", "last_name" FROM "Person")";

  const std::string expected =
      R"([{"first_name":"Homer","last_name":"Simpson"},{"first_name":"Marge","last_name":"Simpson"},{"first_name":"Bart","last_name":"Simpson"},{"first_name":"Lisa","last_name":"Simpson"},{"first_name":"Maggie","last_name":"Simpson"}])";

  EXPECT_EQ(duckdb::to_sql(get_names), expected_query);
  EXPECT_EQ(rfl::json::write(names), expected);
}

}  // namespace test_create_table_as


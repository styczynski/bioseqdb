#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/duckdb.hpp>
#include <vector>

namespace test_update_with_optional {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  std::optional<int> age;
};

TEST(duckdb, test_update_with_optional) {
  const auto people1 = std::vector<Person>(
      {Person{
           .id = 0, .first_name = "Homer", .last_name = "Simpson", .age = 45},
       Person{.id = 1, .first_name = "Bart", .last_name = "Simpson", .age = 10},
       Person{.id = 2, .first_name = "Lisa", .last_name = "Simpson", .age = 8},
       Person{
           .id = 3, .first_name = "Maggie", .last_name = "Simpson", .age = 0},
       Person{
           .id = 4, .first_name = "Hugo", .last_name = "Simpson", .age = 10}});

  const auto conn = sqlgen::duckdb::connect();

  sqlgen::write(conn, people1);

  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto query1 = update<Person>("first_name"_c.set("last_name"_c),
                                     "age"_c.set(std::nullopt)) |
                      where("first_name"_c == "Hugo");

  const auto query2 =
      update<Person>("age"_c.set(50)) | where("first_name"_c == "Homer");

  const auto query3 = update<Person>("age"_c.set(std::optional<int>(11))) |
                      where("first_name"_c == "Bart");

  query1(conn).and_then(query2).and_then(query3).value();

  const auto people2 =
      (sqlgen::read<std::vector<Person>> | order_by("id"_c))(conn).value();

  const std::string expected =
      R"([{"id":0,"first_name":"Homer","last_name":"Simpson","age":50},{"id":1,"first_name":"Bart","last_name":"Simpson","age":11},{"id":2,"first_name":"Lisa","last_name":"Simpson","age":8},{"id":3,"first_name":"Maggie","last_name":"Simpson","age":0},{"id":4,"first_name":"Simpson","last_name":"Simpson"}])";

  EXPECT_EQ(rfl::json::write(people2), expected);
}

}  // namespace test_update_with_optional

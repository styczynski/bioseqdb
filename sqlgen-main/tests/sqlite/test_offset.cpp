#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/sqlite.hpp>
#include <vector>

namespace test_offset {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  int age;
};

TEST(sqlite, test_offset) {
  const auto people1 = std::vector<Person>(
      {Person{
           .id = 0, .first_name = "Homer", .last_name = "Simpson", .age = 45},
       Person{.id = 1, .first_name = "Bart", .last_name = "Simpson", .age = 10},
       Person{.id = 2, .first_name = "Lisa", .last_name = "Simpson", .age = 8},
       Person{
           .id = 3, .first_name = "Maggie", .last_name = "Simpson", .age = 0},
       Person{
           .id = 4, .first_name = "Hugo", .last_name = "Simpson", .age = 10}});

  const auto conn = sqlgen::sqlite::connect();

  sqlgen::write(conn, people1);

  using namespace sqlgen;
  using namespace sqlgen::literals;

  // Note: SQLite does not support offset without limit.
  const auto query =
          sqlgen::read<std::vector<Person>> | order_by("id"_c) | limit(5) | offset(3);

  const auto sql = sqlite::to_sql(query);

  const auto people2 = query(conn).value();

  const std::string expected =
      R"([{"id":3,"first_name":"Maggie","last_name":"Simpson","age":0},{"id":4,"first_name":"Hugo","last_name":"Simpson","age":10}])";

  EXPECT_EQ(rfl::json::write(people2), expected);
}

}  // namespace test_offset

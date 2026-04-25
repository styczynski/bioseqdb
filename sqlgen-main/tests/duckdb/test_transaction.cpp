#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/duckdb.hpp>
#include <vector>

namespace test_update {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  int age;
};

TEST(duckdb, test_transaction) {
  const auto people1 = std::vector<Person>(
      {Person{
           .id = 0, .first_name = "Homer", .last_name = "Simpson", .age = 45},
       Person{.id = 1, .first_name = "Bart", .last_name = "Simpson", .age = 10},
       Person{.id = 2, .first_name = "Lisa", .last_name = "Simpson", .age = 8},
       Person{
           .id = 3, .first_name = "Maggie", .last_name = "Simpson", .age = 0},
       Person{
           .id = 4, .first_name = "Hugo", .last_name = "Simpson", .age = 10}});

  auto conn = sqlgen::duckdb::connect();

  sqlgen::write(conn, people1);

  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto delete_hugo =
      delete_from<Person> | where("first_name"_c == "Hugo");

  const auto update_homers_age =
      update<Person>("age"_c.set(46)) | where("first_name"_c == "Homer");

  conn = sqlgen::begin_transaction(conn)
             .and_then(delete_hugo)
             .and_then(update_homers_age)
             .and_then(sqlgen::commit);

  const auto people2 = sqlgen::read<std::vector<Person>>(conn).value();

  const std::string expected =
      R"([{"id":0,"first_name":"Homer","last_name":"Simpson","age":46},{"id":1,"first_name":"Bart","last_name":"Simpson","age":10},{"id":2,"first_name":"Lisa","last_name":"Simpson","age":8},{"id":3,"first_name":"Maggie","last_name":"Simpson","age":0}])";

  EXPECT_EQ(rfl::json::write(people2), expected);
}

}  // namespace test_update

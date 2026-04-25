#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/sqlite.hpp>
#include <vector>

namespace test_insert_by_ref_and_read {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  int age;
};

TEST(sqlite, test_insert_by_ref_and_read) {
  const auto people1 = std::vector<Person>(
      {Person{
           .id = 0, .first_name = "Homer", .last_name = "Simpson", .age = 45},
       Person{.id = 1, .first_name = "Bart", .last_name = "Simpson", .age = 10},
       Person{.id = 2, .first_name = "Lisa", .last_name = "Simpson", .age = 8},
       Person{
           .id = 3, .first_name = "Maggie", .last_name = "Simpson", .age = 0}});

  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto people2 = sqlite::connect()
                           .and_then(begin_transaction)
                           .and_then(create_table<Person> | if_not_exists)
                           .and_then(insert(std::ref(people1)))
                           .and_then(commit)
                           .and_then(sqlgen::read<std::vector<Person>>)
                           .value();

  const auto json1 = rfl::json::write(people1);
  const auto json2 = rfl::json::write(people2);

  EXPECT_EQ(json1, json2);
}

}  // namespace test_insert_by_ref_and_read

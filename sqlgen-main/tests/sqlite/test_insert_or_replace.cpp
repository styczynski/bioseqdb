#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/sqlite.hpp>
#include <vector>

namespace test_insert_or_replace {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  int age;
};

TEST(sqlite, test_insert_or_replace) {
  const auto people1 = std::vector<Person>(
      {Person{
           .id = 0, .first_name = "Homer", .last_name = "Simpson", .age = 45},
       Person{.id = 1, .first_name = "Bart", .last_name = "Simpson", .age = 10},
       Person{.id = 2, .first_name = "Lisa", .last_name = "Simpson", .age = 8},
       Person{
           .id = 3, .first_name = "Maggie", .last_name = "Simpson", .age = 0}});

  const auto people2 = std::vector<Person>({Person{.id = 1,
                                                   .first_name = "Bartholomew",
                                                   .last_name = "Simpson",
                                                   .age = 10},
                                            Person{.id = 3,
                                                   .first_name = "Margaret",
                                                   .last_name = "Simpson",
                                                   .age = 1}});

  const auto people3 = std::vector<Person>(
      {Person{
           .id = 0, .first_name = "Homer", .last_name = "Simpson", .age = 45},
       Person{.id = 1,
              .first_name = "Bartholomew",
              .last_name = "Simpson",
              .age = 10},
       Person{.id = 2, .first_name = "Lisa", .last_name = "Simpson", .age = 8},
       Person{.id = 3,
              .first_name = "Margaret",
              .last_name = "Simpson",
              .age = 1}});

  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto people4 =
      sqlite::connect()
          .and_then(begin_transaction)
          .and_then(create_table<Person> | if_not_exists)
          .and_then(insert(people1))
          .and_then(commit)
          .and_then(begin_transaction)
          .and_then(insert_or_replace(std::ref(people2)))
          .and_then(commit)
          .and_then(sqlgen::read<std::vector<Person>> | order_by("id"_c))
          .value();

  const auto json3 = rfl::json::write(people3);
  const auto json4 = rfl::json::write(people4);

  EXPECT_EQ(json3, json4);
}

}  // namespace test_insert_or_replace

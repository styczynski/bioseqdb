#ifndef SQLGEN_BUILD_DRY_TESTS_ONLY

#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/postgres.hpp>
#include <vector>
#include "test_helpers.hpp"

namespace test_foreign_key {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  int age;
};

struct Relationship {
  sqlgen::ForeignKey<uint32_t, Person, "id"> parent_id;
  uint32_t child_id;
};

TEST(postgres, test_foreign_key) {
  auto people1 = std::vector<Person>(
      {Person{
           .id = 0, .first_name = "Homer", .last_name = "Simpson", .age = 45},
       Person{.id = 1, .first_name = "Bart", .last_name = "Simpson", .age = 10},
       Person{.id = 2, .first_name = "Lisa", .last_name = "Simpson", .age = 8},
       Person{
           .id = 3, .first_name = "Maggie", .last_name = "Simpson", .age = 0}});

  const auto relationships =
      std::vector<Relationship>({Relationship{.parent_id = 0, .child_id = 2},
                                 Relationship{.parent_id = 0, .child_id = 3},
                                 Relationship{.parent_id = 0, .child_id = 4},
                                 Relationship{.parent_id = 1, .child_id = 2},
                                 Relationship{.parent_id = 1, .child_id = 3},
                                 Relationship{.parent_id = 1, .child_id = 4}});

  const auto credentials = sqlgen::postgres::test::make_credentials();

  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto people = postgres::connect(credentials)
                          .and_then(drop<Person> | if_exists | cascade)
                          .and_then(drop<Relationship> | if_exists)
                          .and_then(begin_transaction)
                          .and_then(create_table<Person>)
                          .and_then(create_table<Relationship>)
                          .and_then(insert(std::ref(people1)))
                          .and_then(insert(std::ref(relationships)))
                          .and_then(drop<Person> | if_exists | cascade)
                          .and_then(drop<Relationship> | if_exists)
                          .and_then(commit)
                          .value();
}

}  // namespace test_foreign_key

#endif

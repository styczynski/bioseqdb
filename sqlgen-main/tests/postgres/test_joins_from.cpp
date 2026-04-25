#ifndef SQLGEN_BUILD_DRY_TESTS_ONLY

#include <gtest/gtest.h>

#include <ranges>
#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/postgres.hpp>
#include <vector>
#include "test_helpers.hpp"

namespace test_joins_from {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  double age;
};

struct Relationship {
  sqlgen::PrimaryKey<uint32_t> parent_id;
  sqlgen::PrimaryKey<uint32_t> child_id;
};

TEST(postgres, test_joins_from) {
  const auto people1 = std::vector<Person>(
      {Person{
           .id = 0, .first_name = "Homer", .last_name = "Simpson", .age = 45},
       Person{
           .id = 1, .first_name = "Marge", .last_name = "Simpson", .age = 40},
       Person{.id = 2, .first_name = "Bart", .last_name = "Simpson", .age = 10},
       Person{.id = 3, .first_name = "Lisa", .last_name = "Simpson", .age = 8},
       Person{
           .id = 4, .first_name = "Maggie", .last_name = "Simpson", .age = 0}});

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

  struct ParentAndChild {
    std::string last_name;
    std::string first_name_parent;
    std::string first_name_child;
    double parent_age_at_birth;
  };

  const auto get_parents =
      select_from<Person, "t1">(
          "child_id"_t2 | as<"id">, "first_name"_t1 | as<"first_name">,
          "last_name"_t1 | as<"last_name">, "age"_t1 | as<"age">) |
      inner_join<Relationship, "t2">("id"_t1 == "parent_id"_t2);

  const auto get_people =
      select_from<"t1">(get_parents, "last_name"_t1 | as<"last_name">,
                        "first_name"_t1 | as<"first_name_parent">,
                        "first_name"_t2 | as<"first_name_child">,
                        ("age"_t1 - "age"_t2) | as<"parent_age_at_birth">) |
      inner_join<Person, "t2">("id"_t1 == "id"_t2) |
      order_by("id"_t2, "id"_t1) | to<std::vector<ParentAndChild>>;

  const auto people = postgres::connect(credentials)
                          .and_then(drop<Person> | if_exists)
                          .and_then(drop<Relationship> | if_exists)
                          .and_then(write(std::ref(people1)))
                          .and_then(write(std::ref(relationships)))
                          .and_then(get_people)
                          .value();

  const std::string expected_query =
      R"(SELECT t1."last_name" AS "last_name", t1."first_name" AS "first_name_parent", t2."first_name" AS "first_name_child", (t1."age") - (t2."age") AS "parent_age_at_birth" FROM (SELECT t2."child_id" AS "id", t1."first_name" AS "first_name", t1."last_name" AS "last_name", t1."age" AS "age" FROM "Person" t1 INNER JOIN "Relationship" t2 ON t1."id" = t2."parent_id") t1 INNER JOIN "Person" t2 ON t1."id" = t2."id" ORDER BY t2."id", t1."id")";

  const std::string expected =
      R"([{"last_name":"Simpson","first_name_parent":"Homer","first_name_child":"Bart","parent_age_at_birth":35.0},{"last_name":"Simpson","first_name_parent":"Marge","first_name_child":"Bart","parent_age_at_birth":30.0},{"last_name":"Simpson","first_name_parent":"Homer","first_name_child":"Lisa","parent_age_at_birth":37.0},{"last_name":"Simpson","first_name_parent":"Marge","first_name_child":"Lisa","parent_age_at_birth":32.0},{"last_name":"Simpson","first_name_parent":"Homer","first_name_child":"Maggie","parent_age_at_birth":45.0},{"last_name":"Simpson","first_name_parent":"Marge","first_name_child":"Maggie","parent_age_at_birth":40.0}])";

  EXPECT_EQ(postgres::to_sql(get_people), expected_query);
  EXPECT_EQ(rfl::json::write(people), expected);
}

}  // namespace test_joins_from

#endif

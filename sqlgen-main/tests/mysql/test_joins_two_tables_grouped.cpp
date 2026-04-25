#ifndef SQLGEN_BUILD_DRY_TESTS_ONLY

#include <gtest/gtest.h>

#include <ranges>
#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/mysql.hpp>
#include <vector>
#include "test_helpers.hpp"

namespace test_joins_two_tables_grouped {

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

TEST(mysql, test_joins_two_tables_grouped) {
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

  const auto credentials = sqlgen::mysql::test::make_credentials();

  using namespace sqlgen;
  using namespace sqlgen::literals;

  struct ParentAndChild {
    std::string last_name;
    std::string first_name_child;
    double avg_parent_age_at_birth;
  };

  const auto get_people =
      select_from<Person, "t1">(
          "last_name"_t1 | as<"last_name">,
          "first_name"_t3 | as<"first_name_child">,
          avg("age"_t1 - "age"_t3) | as<"avg_parent_age_at_birth">) |
      inner_join<Relationship, "t2">("id"_t1 == "parent_id"_t2) |
      inner_join<Person, "t3">("id"_t3 == "child_id"_t2) |
      group_by("last_name"_t1, "first_name"_t3) |
      order_by("last_name"_t1, "first_name"_t3) |
      to<std::vector<ParentAndChild>>;

  const auto people = mysql::connect(credentials)
                          .and_then(drop<Person> | if_exists)
                          .and_then(drop<Relationship> | if_exists)
                          .and_then(write(std::ref(people1)))
                          .and_then(write(std::ref(relationships)))
                          .and_then(get_people)
                          .value();

  const std::string expected_query =
      R"(SELECT t1.`last_name` AS `last_name`, t3.`first_name` AS `first_name_child`, AVG((t1.`age`) - (t3.`age`)) AS `avg_parent_age_at_birth` FROM `Person` t1 INNER JOIN `Relationship` t2 ON t1.`id` = t2.`parent_id` INNER JOIN `Person` t3 ON t3.`id` = t2.`child_id` GROUP BY t1.`last_name`, t3.`first_name` ORDER BY t1.`last_name`, t3.`first_name`)";

  const std::string expected =
      R"([{"last_name":"Simpson","first_name_child":"Bart","avg_parent_age_at_birth":32.5},{"last_name":"Simpson","first_name_child":"Lisa","avg_parent_age_at_birth":34.5},{"last_name":"Simpson","first_name_child":"Maggie","avg_parent_age_at_birth":42.5}])";

  EXPECT_EQ(mysql::to_sql(get_people), expected_query);
  EXPECT_EQ(rfl::json::write(people), expected);
}

}  // namespace test_joins_two_tables_grouped

#endif

#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/sqlite.hpp>
#include <vector>

namespace test_joins_nested_grouped {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  double age;
};

struct Relationship {
  uint32_t parent_id;
  uint32_t child_id;
};

TEST(sqlite, test_joins_nested_grouped) {
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

  using namespace sqlgen;
  using namespace sqlgen::literals;

  struct ParentAndChild {
    std::string last_name;
    std::string first_name_child;
    double avg_parent_age_at_birth;
  };

  const auto get_children =
      select_from<Relationship, "t1">("parent_id"_t1 | as<"id">,
                                      "first_name"_t2 | as<"first_name">,
                                      "age"_t2 | as<"age">) |
      inner_join<Person, "t2">("id"_t2 == "child_id"_t1);

  const auto get_people =
      select_from<Person, "t1">(
          "last_name"_t1 | as<"last_name">,
          "first_name"_t2 | as<"first_name_child">,
          avg("age"_t1 - "age"_t2) | as<"avg_parent_age_at_birth">) |
      inner_join<"t2">(get_children, "id"_t1 == "id"_t2) |
      group_by("last_name"_t1, "first_name"_t2) | order_by("first_name"_t2) |
      to<std::vector<ParentAndChild>>;

  const auto people = sqlite::connect()
                          .and_then(write(std::ref(people1)))
                          .and_then(write(std::ref(relationships)))
                          .and_then(get_people)
                          .value();

  const std::string expected_query =
      R"(SELECT t1."last_name" AS "last_name", t2."first_name" AS "first_name_child", AVG((t1."age") - (t2."age")) AS "avg_parent_age_at_birth" FROM "Person" t1 INNER JOIN (SELECT t1."parent_id" AS "id", t2."first_name" AS "first_name", t2."age" AS "age" FROM "Relationship" t1 INNER JOIN "Person" t2 ON t2."id" = t1."child_id") t2 ON t1."id" = t2."id" GROUP BY t1."last_name", t2."first_name" ORDER BY t2."first_name")";

  const std::string expected =
      R"([{"last_name":"Simpson","first_name_child":"Bart","avg_parent_age_at_birth":32.5},{"last_name":"Simpson","first_name_child":"Lisa","avg_parent_age_at_birth":34.5},{"last_name":"Simpson","first_name_child":"Maggie","avg_parent_age_at_birth":42.5}])";
  EXPECT_EQ(sqlite::to_sql(get_people), expected_query);
  EXPECT_EQ(rfl::json::write(people), expected);
}

}  // namespace test_joins_nested_grouped

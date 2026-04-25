#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/sqlite.hpp>
#include <vector>

namespace test_join {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  double age;
};

TEST(sqlite, test_join) {
  const auto people1 = std::vector<Person>(
      {Person{
           .id = 0, .first_name = "Homer", .last_name = "Simpson", .age = 45},
       Person{.id = 1, .first_name = "Bart", .last_name = "Simpson", .age = 10},
       Person{.id = 2, .first_name = "Lisa", .last_name = "Simpson", .age = 8},
       Person{
           .id = 3, .first_name = "Maggie", .last_name = "Simpson", .age = 0}});

  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto get_people =
      select_from<Person, "t1">(
          "id"_t1 | as<"id">, "first_name"_t1 | as<"first_name">,
          "last_name"_t2 | as<"last_name">, "age"_t2 | as<"age">) |
      inner_join<Person, "t2">("id"_t1 == "id"_t2) | order_by("id"_t1) |
      to<std::vector<Person>>;

  const auto people = sqlite::connect()
                          .and_then(write(std::ref(people1)))
                          .and_then(get_people)
                          .value();

  const std::string expected_query =
      R"(SELECT t1."id" AS "id", t1."first_name" AS "first_name", t2."last_name" AS "last_name", t2."age" AS "age" FROM "Person" t1 INNER JOIN "Person" t2 ON t1."id" = t2."id" ORDER BY t1."id")";
  const std::string expected =
      R"([{"id":0,"first_name":"Homer","last_name":"Simpson","age":45.0},{"id":1,"first_name":"Bart","last_name":"Simpson","age":10.0},{"id":2,"first_name":"Lisa","last_name":"Simpson","age":8.0},{"id":3,"first_name":"Maggie","last_name":"Simpson","age":0.0}])";

  EXPECT_EQ(sqlite::to_sql(get_people), expected_query);
  EXPECT_EQ(rfl::json::write(people), expected);
}

}  // namespace test_join

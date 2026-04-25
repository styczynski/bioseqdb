
#include <gtest/gtest.h>

#include <ranges>
#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/sqlite.hpp>
#include <vector>

namespace test_aggregations {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  int age;
};

TEST(sqlite, test_aggregations) {
  const auto people1 = std::vector<Person>(
      {Person{
           .id = 0, .first_name = "Homer", .last_name = "Simpson", .age = 45},
       Person{.id = 1, .first_name = "Bart", .last_name = "Simpson", .age = 10},
       Person{.id = 2, .first_name = "Lisa", .last_name = "Simpson", .age = 8},
       Person{
           .id = 3, .first_name = "Maggie", .last_name = "Simpson", .age = 0}});

  using namespace sqlgen;
  using namespace sqlgen::literals;

  struct Children {
    int num_children;
    int num_last_names;
    double avg_age;
    double max_age;
    double min_age;
    double sum_age;
  };

  const auto get_children =
      select_from<Person>(
          avg("age"_c).as<"avg_age">(), count().as<"num_children">(),
          max("age"_c).as<"max_age">(), min("age"_c).as<"min_age">(),
          sum("age"_c).as<"sum_age">(),
          count_distinct("last_name"_c).as<"num_last_names">()) |
      where("age"_c < 18) | to<Children>;

  const auto children = sqlite::connect()
                            .and_then(drop<Person> | if_exists)
                            .and_then(write(std::ref(people1)))
                            .and_then(get_children)
                            .value();

  EXPECT_EQ(children.num_children, 3);
  EXPECT_EQ(children.num_last_names, 1);
  EXPECT_EQ(children.avg_age, 6.0);
  EXPECT_EQ(children.max_age, 10.0);
  EXPECT_EQ(children.min_age, 0.0);
  EXPECT_EQ(children.sum_age, 18.0);
}

}  // namespace test_aggregations


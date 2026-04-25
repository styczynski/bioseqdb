
#include <gtest/gtest.h>

#include <ranges>
#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/duckdb.hpp>
#include <vector>

namespace test_group_by_with_operations {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  int age;
};

TEST(duckdb, test_group_by_with_operations) {
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
    std::string last_name;
    std::string last_name_trimmed;
    double avg_age;
    double max_age_plus_one;
    double min_age_plus_one;
  };

  const auto get_children =
      select_from<Person>(
          "last_name"_c, trim("last_name"_c).as<"last_name_trimmed">(),
          max(cast<double>("age"_c) + 1.0).as<"max_age_plus_one">(),
          (min(cast<double>("age"_c)) + 1.0).as<"min_age_plus_one">(),
          round(avg(cast<double>("age"_c))).as<"avg_age">()) |
      where("age"_c < 18) | group_by("last_name"_c) | to<std::vector<Children>>;

  const auto conn = duckdb::connect()
                        .and_then(drop<Person> | if_exists)
                        .and_then(write(std::ref(people1)));

  const auto children = get_children(conn).value();

  EXPECT_EQ(children.size(), 1);
  EXPECT_EQ(children.at(0).last_name, "Simpson");
  EXPECT_EQ(children.at(0).last_name_trimmed, "Simpson");
  EXPECT_EQ(children.at(0).avg_age, 6.0);
  EXPECT_EQ(children.at(0).max_age_plus_one, 11.0);
  EXPECT_EQ(children.at(0).min_age_plus_one, 1.0);
}

}  // namespace test_group_by_with_operations


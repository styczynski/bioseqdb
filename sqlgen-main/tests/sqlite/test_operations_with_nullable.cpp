
#include <gtest/gtest.h>

#include <optional>
#include <ranges>
#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/sqlite.hpp>
#include <vector>

namespace test_operations_with_nullable {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::optional<std::string> last_name;
  std::optional<int> age;
};

TEST(sqlite, test_operations_with_nullable) {
  const auto people1 = std::vector<Person>(
      {Person{
           .id = 0, .first_name = "Homer", .last_name = "Simpson", .age = 45},
       Person{.id = 1, .first_name = "Bart", .last_name = "Simpson", .age = 10},
       Person{.id = 2, .first_name = "Hugo", .age = 10},
       Person{.id = 3, .first_name = "Lisa", .last_name = "Simpson", .age = 8},
       Person{
           .id = 4, .first_name = "Maggie", .last_name = "Simpson", .age = 0}});

  using namespace sqlgen;
  using namespace sqlgen::literals;

  struct Children {
    std::optional<int> id_plus_age;
    std::optional<int> age_times_2;
    std::optional<int> id_plus_2_minus_age;
    std::optional<std::string> full_name;
    std::string last_name_or_none;
  };

  const auto get_children =
      select_from<Person>(
          ("id"_c + "age"_c) | as<"id_plus_age">,
          ("age"_c * 2) | as<"age_times_2">,
          ("id"_c + 2 - "age"_c) | as<"id_plus_2_minus_age">,
          concat(upper("last_name"_c), ", ", "first_name"_c) | as<"full_name">,
          coalesce(upper("last_name"_c), "none") | as<"last_name_or_none">) |
      where("age"_c < 18) | to<std::vector<Children>>;

  const auto children = sqlite::connect()
                            .and_then(write(std::ref(people1)))
                            .and_then(get_children)
                            .value();

  const std::string expected =
      R"([{"id_plus_age":11,"age_times_2":20,"id_plus_2_minus_age":-7,"full_name":"SIMPSON, Bart","last_name_or_none":"SIMPSON"},{"id_plus_age":12,"age_times_2":20,"id_plus_2_minus_age":-6,"last_name_or_none":"none"},{"id_plus_age":11,"age_times_2":16,"id_plus_2_minus_age":-3,"full_name":"SIMPSON, Lisa","last_name_or_none":"SIMPSON"},{"id_plus_age":4,"age_times_2":0,"id_plus_2_minus_age":6,"full_name":"SIMPSON, Maggie","last_name_or_none":"SIMPSON"}])";

  EXPECT_EQ(rfl::json::write(children), expected);
}

}  // namespace test_operations_with_nullable


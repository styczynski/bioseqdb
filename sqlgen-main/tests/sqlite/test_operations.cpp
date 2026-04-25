
#include <gtest/gtest.h>

#include <ranges>
#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/sqlite.hpp>
#include <vector>

namespace test_operations {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  int age;
};

TEST(sqlite, test_operations) {
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
    int id_plus_age;
    int age_times_2;
    int id_plus_2_minus_age;
    int age_mod_3;
    int abs_age;
    double exp_age;
    double sqrt_age;
    size_t length_first_name;
    std::string full_name;
    std::string first_name_lower;
    std::string first_name_upper;
    std::string first_name_replaced;
  };

  const auto get_children =
      select_from<Person>(
          ("id"_c + "age"_c) | as<"id_plus_age">,
          ("age"_c * 2) | as<"age_times_2">, ("age"_c % 3) | as<"age_mod_3">,
          abs("age"_c * (-1)) | as<"abs_age">,
          round(exp(cast<double>("age"_c)), 2) | as<"exp_age">,
          round(sqrt(cast<double>("age"_c)), 2) | as<"sqrt_age">,
          ("id"_c + 2 - "age"_c) | as<"id_plus_2_minus_age">,
          length(trim("first_name"_c)) | as<"length_first_name">,
          concat(ltrim("first_name"_c), " ", rtrim("last_name"_c)) |
              as<"full_name">,
          upper(rtrim(concat("first_name"_c, " "))) | as<"first_name_upper">,
          lower(ltrim(concat(" ", "first_name"_c))) | as<"first_name_lower">,
          replace("first_name"_c, "Bart", "Hugo") | as<"first_name_replaced">) |
      where("age"_c < 18) | order_by("age"_c.desc()) |
      to<std::vector<Children>>;

  const auto children = sqlite::connect()
                            .and_then(write(std::ref(people1)))
                            .and_then(get_children)
                            .value();

  const std::string expected =
      R"([{"id_plus_age":11,"age_times_2":20,"id_plus_2_minus_age":-7,"age_mod_3":1,"abs_age":10,"exp_age":22026.47,"sqrt_age":3.16,"length_first_name":4,"full_name":"Bart Simpson","first_name_lower":"bart","first_name_upper":"BART","first_name_replaced":"Hugo"},{"id_plus_age":10,"age_times_2":16,"id_plus_2_minus_age":-4,"age_mod_3":2,"abs_age":8,"exp_age":2980.96,"sqrt_age":2.83,"length_first_name":4,"full_name":"Lisa Simpson","first_name_lower":"lisa","first_name_upper":"LISA","first_name_replaced":"Lisa"},{"id_plus_age":3,"age_times_2":0,"id_plus_2_minus_age":5,"age_mod_3":0,"abs_age":0,"exp_age":1.0,"sqrt_age":0.0,"length_first_name":6,"full_name":"Maggie Simpson","first_name_lower":"maggie","first_name_upper":"MAGGIE","first_name_replaced":"Maggie"}])";

  EXPECT_EQ(rfl::json::write(children), expected);
}

}  // namespace test_operations


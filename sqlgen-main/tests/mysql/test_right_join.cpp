#ifndef SQLGEN_BUILD_DRY_TESTS_ONLY

#include <gtest/gtest.h>

#include <optional>
#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/mysql.hpp>
#include <vector>
#include "test_helpers.hpp"

namespace test_right_join {

TEST(mysql, test_right_join) {
  struct Person {
    sqlgen::PrimaryKey<uint32_t> id;
    std::string first_name;
    std::string last_name;
  };

  struct Pet {
    sqlgen::PrimaryKey<uint32_t> id;
    std::string name;
    uint32_t owner_id;
  };

  const auto people = std::vector<Person>({
      Person{.id = 1, .first_name = "Homer", .last_name = "Simpson"},
      Person{.id = 2, .first_name = "Marge", .last_name = "Simpson"},
      Person{.id = 3, .first_name = "Bart", .last_name = "Simpson"},
      Person{.id = 4, .first_name = "Lisa", .last_name = "Simpson"},
  });

  const auto pets = std::vector<Pet>({
      Pet{.id = 1, .name = "Santa's Little Helper", .owner_id = 1},
      Pet{.id = 2, .name = "Snowball", .owner_id = 4},
      Pet{.id = 3, .name = "Mr. Teeny", .owner_id = 99},
  });

  const auto credentials = sqlgen::mysql::test::make_credentials();

  using namespace sqlgen;
  using namespace sqlgen::literals;

  struct PetWithOwner {
    std::string pet_name;
    std::optional<std::string> owner_first_name;
    std::optional<std::string> owner_last_name;
  };

  const auto get_pets_with_owners =
      select_from<Person, "t1">("name"_t2 | as<"pet_name">,
                                "first_name"_t1 | as<"owner_first_name">,
                                "last_name"_t1 | as<"owner_last_name">) |
      right_join<Pet, "t2">("id"_t1 == "owner_id"_t2) | order_by("id"_t2) |
      to<std::vector<PetWithOwner>>;

  const auto result = mysql::connect(credentials)
                          .and_then(drop<Person> | if_exists)
                          .and_then(drop<Pet> | if_exists)
                          .and_then(write(std::ref(people)))
                          .and_then(write(std::ref(pets)))
                          .and_then(get_pets_with_owners)
                          .value();

  const std::string expected_query =
      R"(SELECT t2.`name` AS `pet_name`, t1.`first_name` AS `owner_first_name`, t1.`last_name` AS `owner_last_name` FROM `Person` t1 RIGHT JOIN `Pet` t2 ON t1.`id` = t2.`owner_id` ORDER BY t2.`id`)";
  const std::string expected_json =
      R"([{"pet_name":"Santa's Little Helper","owner_first_name":"Homer","owner_last_name":"Simpson"},{"pet_name":"Snowball","owner_first_name":"Lisa","owner_last_name":"Simpson"},{"pet_name":"Mr. Teeny"}])";

  EXPECT_EQ(mysql::to_sql(get_pets_with_owners), expected_query);
  EXPECT_EQ(rfl::json::write(result), expected_json);
}

}  // namespace test_right_join

#endif

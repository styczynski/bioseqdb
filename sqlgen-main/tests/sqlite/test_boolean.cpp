#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/sqlite.hpp>
#include <vector>

namespace test_boolean {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  bool has_children;
};

TEST(sqlite, test_boolean) {
  const auto people1 = std::vector<Person>({Person{.id = 0,
                                                   .first_name = "Homer",
                                                   .last_name = "Simpson",
                                                   .has_children = true},
                                            Person{.id = 1,
                                                   .first_name = "Bart",
                                                   .last_name = "Simpson",
                                                   .has_children = false},
                                            Person{.id = 2,
                                                   .first_name = "Lisa",
                                                   .last_name = "Simpson",
                                                   .has_children = false},
                                            Person{.id = 3,
                                                   .first_name = "Maggie",
                                                   .last_name = "Simpson",
                                                   .has_children = false}});

  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto conn = sqlite::connect();

  const auto people2 = sqlgen::write(conn, people1)
                           .and_then(sqlgen::read<std::vector<Person>>)
                           .value();

  const auto json1 = rfl::json::write(people1);
  const auto json2 = rfl::json::write(people2);

  EXPECT_EQ(json1, json2);
}

}  // namespace test_boolean


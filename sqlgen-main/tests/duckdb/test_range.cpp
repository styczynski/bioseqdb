#include <gtest/gtest.h>

#include <ranges>
#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/duckdb.hpp>
#include <vector>

namespace test_range {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  int age;
};

TEST(duckdb, test_range) {
  using namespace std::ranges::views;

  const auto people1 = std::vector<Person>(
      {Person{
           .id = 0, .first_name = "Homer", .last_name = "Simpson", .age = 45},
       Person{.id = 1, .first_name = "Bart", .last_name = "Simpson", .age = 10},
       Person{.id = 2, .first_name = "Lisa", .last_name = "Simpson", .age = 8},
       Person{
           .id = 3, .first_name = "Maggie", .last_name = "Simpson", .age = 0}});

  const auto conn = sqlgen::duckdb::connect();

  sqlgen::write(conn, people1);

  const auto people2 = sqlgen::read<sqlgen::Range<Person>>(conn).value();

  const auto first_names =
      sqlgen::internal::collect::vector(people2 | transform([](const auto& _r) {
                                          return _r.value().first_name;
                                        }));

  EXPECT_EQ(first_names.at(0), "Homer");
  EXPECT_EQ(first_names.at(1), "Bart");
  EXPECT_EQ(first_names.at(2), "Lisa");
  EXPECT_EQ(first_names.at(3), "Maggie");
}

}  // namespace test_range

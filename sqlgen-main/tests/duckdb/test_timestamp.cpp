#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/duckdb.hpp>
#include <vector>

namespace test_timestamp {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  sqlgen::Timestamp<"%Y-%m-%d %H:%M:%S"> birthdate;
};

TEST(duckdb, test_timestamp) {
  const auto people1 =
      std::vector<Person>({Person{.id = 0,
                                  .first_name = "Homer",
                                  .last_name = "Simpson",
                                  .birthdate = "1989-12-17 12:00:00"},
                           Person{.id = 1,
                                  .first_name = "Bart",
                                  .last_name = "Simpson",
                                  .birthdate = "1989-12-17 12:00:00"},
                           Person{.id = 2,
                                  .first_name = "Lisa",
                                  .last_name = "Simpson",
                                  .birthdate = "1989-12-17 12:00:00"},
                           Person{.id = 3,
                                  .first_name = "Maggie",
                                  .last_name = "Simpson",
                                  .birthdate = "1989-12-17 12:00:00"}});

  const auto conn = sqlgen::duckdb::connect();

  sqlgen::write(conn, people1);

  const auto people2 = sqlgen::read<std::vector<Person>>(conn).value();

  const auto json1 = rfl::json::write(people1);
  const auto json2 = rfl::json::write(people2);

  EXPECT_EQ(json1, json2);
}

}  // namespace test_timestamp

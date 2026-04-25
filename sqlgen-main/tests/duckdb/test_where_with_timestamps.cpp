
#include <gtest/gtest.h>

#include <chrono>
#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/duckdb.hpp>
#include <vector>

namespace test_where_with_timestamps {

struct Person {
  sqlgen::PrimaryKey<uint32_t, sqlgen::auto_incr> id;
  std::string first_name;
  std::string last_name;
  sqlgen::Date birthday;
};

TEST(duckdb, test_where_with_timestamps) {
  const auto people1 =
      std::vector<Person>({Person{.first_name = "Homer",
                                  .last_name = "Simpson",
                                  .birthday = sqlgen::Date("1970-01-01")},
                           Person{.first_name = "Bart",
                                  .last_name = "Simpson",
                                  .birthday = sqlgen::Date("2000-01-01")},
                           Person{.first_name = "Lisa",
                                  .last_name = "Simpson",
                                  .birthday = sqlgen::Date("2002-01-01")},
                           Person{.first_name = "Maggie",
                                  .last_name = "Simpson",
                                  .birthday = sqlgen::Date("2010-01-01")}});

  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto conn = sqlgen::duckdb::connect();

  sqlgen::write(conn, people1).value();

  const auto query =
      sqlgen::read<std::vector<Person>> |
      where("birthday"_c + std::chrono::years(11) - std::chrono::weeks(10) +
                std::chrono::milliseconds(4000005) >
            Date("2010-01-01")) |
      order_by("id"_c);

  const auto people2 = query(conn).value();

  const std::string expected_query =
      R"(SELECT "id", "first_name", "last_name", "birthday" FROM "Person" WHERE datetime("birthday", '+11 years', '-70 days', '+01:06:40.005') > '2010-01-01' ORDER BY "id";)";
  const std::string expected =
      R"([{"id":2,"first_name":"Bart","last_name":"Simpson","birthday":"2000-01-01"},{"id":3,"first_name":"Lisa","last_name":"Simpson","birthday":"2002-01-01"},{"id":4,"first_name":"Maggie","last_name":"Simpson","birthday":"2010-01-01"}])";

  EXPECT_EQ(rfl::json::write(people2), expected);
}

}  // namespace test_where_with_timestamps


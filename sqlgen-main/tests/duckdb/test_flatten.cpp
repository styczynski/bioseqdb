#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/duckdb.hpp>
#include <vector>

namespace test_write_and_read {

struct Person {
  sqlgen::PrimaryKey<uint32_t> id;
  std::string first_name;
  std::string last_name;
  int age;
};

struct Employee {
  static constexpr const char* tablename = "EMPLOYEES";

  sqlgen::Flatten<Person> person;
  float salary;
};

TEST(duckdb, test_flatten) {
  const auto people1 =
      std::vector<Employee>({Employee{.person = Person{.id = 0,
                                                       .first_name = "Homer",
                                                       .last_name = "Simpson",
                                                       .age = 45},
                                      .salary = 60000.0}});

  const auto conn = sqlgen::duckdb::connect();

  sqlgen::write(conn, people1);

  const auto people2 = sqlgen::read<std::vector<Employee>>(conn).value();

  const auto json1 = rfl::json::write(people1);
  const auto json2 = rfl::json::write(people2);

  EXPECT_EQ(json1, json2);
}

}  // namespace test_write_and_read

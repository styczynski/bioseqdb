#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/duckdb.hpp>
#include <vector>

namespace test_hello_world {

struct User {
  std::string name;
  int age;
};

TEST(duckdb, test_hello_world) {
  // Connect to duckdb database
  const auto conn = sqlgen::duckdb::connect("test.db");

  // Create and insert a user
  const auto user = User{.name = "John", .age = 30};
  sqlgen::write(conn, user);

  // Read all users
  const auto users = sqlgen::read<std::vector<User>>(conn).value();

  EXPECT_EQ(users.size(), 1);
  EXPECT_EQ(users.at(0).name, "John");
  EXPECT_EQ(users.at(0).age, 30);
}

}  // namespace test_hello_world

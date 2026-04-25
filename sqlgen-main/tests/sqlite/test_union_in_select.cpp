#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/sqlite.hpp>
#include <vector>

#include "sqlgen/sqlite/to_sql.hpp"

namespace test_union_in_select {

struct User1 {
  std::string name;
  int age;
};

struct User2 {
  std::string name;
  int age;
};

struct User3 {
  int age;
  std::string name;
};

TEST(sqlite, test_union_in_select) {
  using namespace sqlgen::literals;

  // Connect to SQLite database
  const auto conn = sqlgen::sqlite::connect();

  // Create and insert a user
  const auto user1 = User1{.name = "John", .age = 30};
  sqlgen::write(conn, user1);

  const auto user2 = User2{.name = "Jane", .age = 25};
  sqlgen::write(conn, user2);

  const auto user3 = User3{.age = 40, .name = "Joe"};
  sqlgen::write(conn, user3);

  const auto s1 = sqlgen::select_from<User1>("name"_c, "age"_c);
  const auto s2 = sqlgen::select_from<User2>("name"_c, "age"_c);
  const auto s3 = sqlgen::select_from<User3>("name"_c, "age"_c);

  const auto united = sqlgen::unite<std::vector<User1>>(s1, s2, s3);

  const auto sel = sqlgen::select_from(united, "name"_c, "age"_c) |
                   sqlgen::to<std::vector<User1>>;

  const auto result = sel(conn);

  const auto users = result.value();

  const auto query = sqlgen::sqlite::to_sql(sel);

  EXPECT_EQ(
      query,
      R"(SELECT "name", "age" FROM (SELECT "name", "age" FROM (SELECT "name", "age" FROM "User1") UNION SELECT "name", "age" FROM (SELECT "name", "age" FROM "User2") UNION SELECT "name", "age" FROM (SELECT "name", "age" FROM "User3")))");

  EXPECT_EQ(users.size(), 3);
  EXPECT_EQ(users.at(0).name, "Jane");
  EXPECT_EQ(users.at(0).age, 25);
  EXPECT_EQ(users.at(1).name, "Joe");
  EXPECT_EQ(users.at(1).age, 40);
  EXPECT_EQ(users.at(2).name, "John");
  EXPECT_EQ(users.at(2).age, 30);
}

}  // namespace test_union_in_select

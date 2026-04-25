#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/sqlite.hpp>
#include <vector>

#include "sqlgen/sqlite/to_sql.hpp"

namespace test_union_in_join2 {

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

struct Login {
  int id;
  std::string username;
};

TEST(sqlite, test_union_in_join2) {
  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto conn = sqlgen::sqlite::connect();

  const auto user1 = User1{.name = "John", .age = 30};
  sqlgen::write(conn, user1);

  const auto user2 = User2{.name = "Jane", .age = 25};
  sqlgen::write(conn, user2);

  const auto user3 = User3{.age = 40, .name = "Joe"};
  sqlgen::write(conn, user3);

  const auto login = Login{.id = 1, .username = "John"};
  sqlgen::write(conn, login);

  const auto s1 = sqlgen::select_from<User1>("name"_c, "age"_c);
  const auto s2 = sqlgen::select_from<User2>("name"_c, "age"_c);
  const auto s3 = sqlgen::select_from<User3>("name"_c, "age"_c);

  const auto united = sqlgen::unite<std::vector<User1>>(s1, s2, s3);

  const auto sel = select_from<"t1">(united, "id"_t2, "username"_t2) |
                   inner_join<Login, "t2">("username"_t2 == "name"_t1) |
                   where("id"_t2 == 1) | to<std::vector<Login>>;

  const auto query = sqlgen::sqlite::to_sql(sel);

  const auto result = sel(conn);

  const auto users = result.value();

  EXPECT_EQ(
      query,
      R"(SELECT t2."id", t2."username" FROM (SELECT "name", "age" FROM (SELECT "name", "age" FROM "User1") UNION SELECT "name", "age" FROM (SELECT "name", "age" FROM "User2") UNION SELECT "name", "age" FROM (SELECT "name", "age" FROM "User3")) t1 INNER JOIN "Login" t2 ON t2."username" = t1."name" WHERE t2."id" = 1)");

  EXPECT_EQ(users.size(), 1);
  EXPECT_EQ(users.at(0).id, 1);
  EXPECT_EQ(users.at(0).username, "John");
}

}  // namespace test_union_in_join2

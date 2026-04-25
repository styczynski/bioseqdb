#ifndef SQLGEN_BUILD_DRY_TESTS_ONLY

#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/postgres.hpp>
#include <vector>

#include "sqlgen/postgres/to_sql.hpp"
#include "test_helpers.hpp"

namespace test_union {

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

TEST(postgres, test_union) {
  using namespace sqlgen::literals;

  const auto credentials = sqlgen::postgres::test::make_credentials();

  const auto conn = sqlgen::postgres::connect(credentials)
                        .and_then(sqlgen::drop<User1> | sqlgen::if_exists)
                        .and_then(sqlgen::drop<User2> | sqlgen::if_exists)
                        .and_then(sqlgen::drop<User3> | sqlgen::if_exists);

  const auto user1 = User1{.name = "John", .age = 30};
  sqlgen::write(conn, user1);

  const auto user2 = User2{.name = "Jane", .age = 25};
  sqlgen::write(conn, user2);

  const auto user3 = User3{.age = 40, .name = "Joe"};
  sqlgen::write(conn, user3);

  const auto s1 = sqlgen::select_from<User1>("name"_c, "age"_c);
  const auto s2 = sqlgen::select_from<User2>("name"_c, "age"_c);
  const auto s3 = sqlgen::select_from<User3>("name"_c, "age"_c);

  const auto result = sqlgen::unite<std::vector<User1>>(s1, s2, s3)(conn);

  const auto users = result.value();

  const auto query =
      sqlgen::postgres::to_sql(sqlgen::unite<std::vector<User1>>(s1, s2, s3));

  EXPECT_EQ(
      query,
      R"(SELECT "name", "age" FROM (SELECT "name", "age" FROM "User1") UNION SELECT "name", "age" FROM (SELECT "name", "age" FROM "User2") UNION SELECT "name", "age" FROM (SELECT "name", "age" FROM "User3"))");

  EXPECT_EQ(users.size(), 3);
  EXPECT_EQ(users.at(0).name, "John");
  EXPECT_EQ(users.at(0).age, 30);
  EXPECT_EQ(users.at(1).name, "Jane");
  EXPECT_EQ(users.at(1).age, 25);
  EXPECT_EQ(users.at(2).name, "Joe");
  EXPECT_EQ(users.at(2).age, 40);
}

}  // namespace test_union

#endif  // SQLGEN_BUILD_DRY_TESTS_ONLY

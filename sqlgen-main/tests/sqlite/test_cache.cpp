#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/sqlite.hpp>
#include <vector>

namespace test_cache {

struct User {
  std::string name;
  int age;
};

TEST(sqlite, test_cache) {
  const auto conn = sqlgen::sqlite::connect();

  const auto user = User{.name = "John", .age = 30};
  sqlgen::write(conn, user);

  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto query = sqlgen::read<User> | where("name"_c == "John");

  const auto cached_query = sqlgen::cache<100>(query);

  auto test_cache_population = [&]() {
    const auto user1 = conn.and_then(cache<100>(query)).value();

    EXPECT_EQ(cached_query.cache(conn).size(), 1);

    const auto user2 = cached_query(conn).value();
    const auto user3 = cached_query(conn).value();

    EXPECT_EQ(user1.name, "John");
    EXPECT_EQ(user1.age, 30);
    EXPECT_EQ(user2.name, "John");
    EXPECT_EQ(user2.age, 30);
    EXPECT_EQ(cached_query.cache(conn).size(), 1);
    EXPECT_EQ(user3.name, "John");
    EXPECT_EQ(user3.age, 30);
  };
  test_cache_population();

  // Test cache invalidation
  cached_query.clear(conn);
  EXPECT_EQ(cached_query.cache(conn).size(), 0);
  test_cache_population();
}

}  // namespace test_cache

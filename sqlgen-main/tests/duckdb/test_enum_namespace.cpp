#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/duckdb.hpp>
#include <vector>

namespace test_enum_namespace {
namespace first {
enum class IdenticallyNamed { VALUE0, VALUE1, VALUE2 };

}
namespace second {
enum class IdenticallyNamed { VALUE3, VALUE4, VALUE5 };
}

struct MultiStruct {
  sqlgen::PrimaryKey<uint32_t, sqlgen::auto_incr> id;
  first::IdenticallyNamed enum_one;
  second::IdenticallyNamed enum_two;
};

TEST(duckdb, test_enum_namespace) {
  using namespace sqlgen;
  using namespace sqlgen::literals;

  auto objects = std::vector<MultiStruct>({
      MultiStruct{.enum_one = first::IdenticallyNamed::VALUE0,
                  .enum_two = second::IdenticallyNamed::VALUE3},
      MultiStruct{.enum_one = first::IdenticallyNamed::VALUE1,
                  .enum_two = second::IdenticallyNamed::VALUE4},
      MultiStruct{.enum_one = first::IdenticallyNamed::VALUE2,
                  .enum_two = second::IdenticallyNamed::VALUE5},
  });

  const auto conn = duckdb::connect();
  conn.and_then(drop<MultiStruct> | if_exists);

  write(conn, objects);

  const auto read_objects =
      sqlgen::read<std::vector<MultiStruct>>(conn).value();
  std::vector<uint32_t> actual_ids;
  for (const auto& obj : read_objects) {
    if (obj.enum_one == first::IdenticallyNamed::VALUE0) {
      EXPECT_EQ(obj.enum_two, second::IdenticallyNamed::VALUE3);
    } else if (obj.enum_one == first::IdenticallyNamed::VALUE1) {
      EXPECT_EQ(obj.enum_two, second::IdenticallyNamed::VALUE4);
    } else if (obj.enum_one == first::IdenticallyNamed::VALUE2) {
      EXPECT_EQ(obj.enum_two, second::IdenticallyNamed::VALUE5);
    } else {
      FAIL() << "Unexpected enum value";
    }
  }
}

}  // namespace test_enum_namespace


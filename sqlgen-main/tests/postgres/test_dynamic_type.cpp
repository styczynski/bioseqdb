#ifndef SQLGEN_BUILD_DRY_TESTS_ONLY

#include <gtest/gtest.h>

#include <boost/lexical_cast.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/postgres.hpp>
#include <vector>
#include "test_helpers.hpp"

namespace sqlgen::parsing {

template <>
struct Parser<boost::uuids::uuid> {
  using Type = boost::uuids::uuid;

  static Result<boost::uuids::uuid> read(
      const std::optional<std::string>& _str) noexcept {
    if (!_str) {
      return error("boost::uuids::uuid cannot be NULL.");
    }
    return boost::lexical_cast<boost::uuids::uuid>(*_str);
  }

  static std::optional<std::string> write(
      const boost::uuids::uuid& _u) noexcept {
    return boost::uuids::to_string(_u);
  }

  static dynamic::Type to_type() noexcept {
    return sqlgen::dynamic::types::Dynamic{"UUID"};
  }
};

}  // namespace sqlgen::parsing

namespace sqlgen::transpilation {

template <>
struct ToValue<boost::uuids::uuid> {
  dynamic::Value operator()(const boost::uuids::uuid& _u) const {
    return dynamic::Value{dynamic::String{.val = boost::uuids::to_string(_u)}};
  }
};

}  // namespace sqlgen::transpilation

/// For the JSON serialization - not needed for
/// the actual DB operations.
namespace rfl {

template <>
struct Reflector<boost::uuids::uuid> {
  using ReflType = std::string;

  static boost::uuids::uuid to(const std::string& _str) {
    return boost::lexical_cast<boost::uuids::uuid>(_str);
  }

  static std::string from(const boost::uuids::uuid& _u) {
    return boost::uuids::to_string(_u);
  }
};

}  // namespace rfl

namespace test_dynamic_type {

struct Person {
  sqlgen::PrimaryKey<boost::uuids::uuid> id =
      boost::uuids::uuid(boost::uuids::random_generator()());
  std::string first_name;
  std::string last_name;
  int age;
};

TEST(postgres, test_dynamic_type) {
  const auto people1 = std::vector<Person>(
      {Person{.first_name = "Homer", .last_name = "Simpson", .age = 45},
       Person{.first_name = "Bart", .last_name = "Simpson", .age = 10},
       Person{.first_name = "Lisa", .last_name = "Simpson", .age = 8},
       Person{.first_name = "Maggie", .last_name = "Simpson", .age = 0}});

  const auto credentials = sqlgen::postgres::test::make_credentials();

  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto conn =
      postgres::connect(credentials).and_then(drop<Person> | if_exists);

  const auto people2 = sqlgen::write(conn, people1)
                           .and_then(sqlgen::read<std::vector<Person>> |
                                     where("id"_c == people1.front().id()))
                           .value();

  const auto json1 = rfl::json::write(std::vector<Person>({people1.front()}));
  const auto json2 = rfl::json::write(people2);

  EXPECT_EQ(json1, json2);
}

}  // namespace test_dynamic_type

#endif

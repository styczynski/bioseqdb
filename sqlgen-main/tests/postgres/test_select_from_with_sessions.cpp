#ifndef SQLGEN_BUILD_DRY_TESTS_ONLY

#include <gtest/gtest.h>

#include <chrono>
#include <ranges>
#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/postgres.hpp>
#include <vector>
#include "test_helpers.hpp"

namespace test_range_select_from_with_sessions {

struct Person {
  sqlgen::PrimaryKey<uint32_t, sqlgen::auto_incr> id;
  std::string first_name;
  std::string last_name;
  sqlgen::Date birthday;
};

TEST(postgres, test_range_select_from_with_sessions) {
  const auto people1 = std::vector<Person>(
      {Person{.first_name = "Homer",
              .last_name = "Simpson",
              .birthday = sqlgen::Timestamp<"%Y-%m-%d">("1970-01-01")},
       Person{.first_name = "Bart",
              .last_name = "Simpson",
              .birthday = sqlgen::Timestamp<"%Y-%m-%d">("2000-01-01")},
       Person{.first_name = "Lisa",
              .last_name = "Simpson",
              .birthday = sqlgen::Timestamp<"%Y-%m-%d">("2002-01-01")},
       Person{.first_name = "Maggie",
              .last_name = "Simpson",
              .birthday = sqlgen::Timestamp<"%Y-%m-%d">("2010-01-01")}});

  const auto pool_config = sqlgen::ConnectionPoolConfig{.size = 2};

  const auto credentials = sqlgen::postgres::test::make_credentials();

  const auto pool = sqlgen::make_connection_pool<sqlgen::postgres::Connection>(
      pool_config, credentials);

  using namespace sqlgen;
  using namespace sqlgen::literals;

  struct Birthday {
    Date birthday;
    Date birthday_recreated;
    time_t birthday_unixepoch;
    double age_in_days;
    int hour;
    int minute;
    int second;
    int weekday;
  };

  const auto get_birthdays =
      select_from<Person>(
          ("birthday"_c + std::chrono::days(10)) | as<"birthday">,
          ((cast<Date>(concat(cast<std::string>(year("birthday"_c)), "-",
                              cast<std::string>(month("birthday"_c)), "-",
                              cast<std::string>(day("birthday"_c))))) +
           std::chrono::days(10)) |
              as<"birthday_recreated">,
          days_between("birthday"_c, Date("2011-01-01")) | as<"age_in_days">,
          unixepoch("birthday"_c + std::chrono::days(10)) |
              as<"birthday_unixepoch">,
          hour(cast<DateTime>("birthday"_c)) | as<"hour">,
          minute(cast<DateTime>("birthday"_c)) | as<"minute">,
          second(cast<DateTime>("birthday"_c)) | as<"second">,
          weekday("birthday"_c) | as<"weekday">) |
      order_by("id"_c) | to<std::vector<Birthday>>;

  const auto birthdays = session(pool)
                             .and_then(drop<Person> | if_exists)
                             .and_then(write(std::ref(people1)))
                             .and_then(get_birthdays)
                             .value();

  const std::string expected_query =
      R"(SELECT "birthday" + INTERVAL '10 days' AS "birthday", cast((cast(extract(YEAR from "birthday") as TEXT) || '-' || cast(extract(MONTH from "birthday") as TEXT) || '-' || cast(extract(DAY from "birthday") as TEXT)) as DATE) + INTERVAL '10 days' AS "birthday_recreated", cast('2011-01-01' as DATE) - cast("birthday" as DATE) AS "age_in_days", extract(EPOCH FROM "birthday" + INTERVAL '10 days') AS "birthday_unixepoch", extract(HOUR from cast("birthday" as TIMESTAMP)) AS "hour", extract(MINUTE from cast("birthday" as TIMESTAMP)) AS "minute", extract(SECOND from cast("birthday" as TIMESTAMP)) AS "second", extract(DOW from "birthday") AS "weekday" FROM "Person" ORDER BY "id")";

  const std::string expected =
      R"([{"birthday":"1970-01-11","birthday_recreated":"1970-01-11","birthday_unixepoch":864000,"age_in_days":14975.0,"hour":0,"minute":0,"second":0,"weekday":4},{"birthday":"2000-01-11","birthday_recreated":"2000-01-11","birthday_unixepoch":947548800,"age_in_days":4018.0,"hour":0,"minute":0,"second":0,"weekday":6},{"birthday":"2002-01-11","birthday_recreated":"2002-01-11","birthday_unixepoch":1010707200,"age_in_days":3287.0,"hour":0,"minute":0,"second":0,"weekday":2},{"birthday":"2010-01-11","birthday_recreated":"2010-01-11","birthday_unixepoch":1263168000,"age_in_days":365.0,"hour":0,"minute":0,"second":0,"weekday":5}])";

  EXPECT_EQ(postgres::to_sql(get_birthdays), expected_query);
  EXPECT_EQ(rfl::json::write(birthdays), expected);
}

}  // namespace test_range_select_from_with_sessions

#endif

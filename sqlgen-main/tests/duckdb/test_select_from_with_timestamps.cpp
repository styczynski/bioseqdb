#include <gtest/gtest.h>

#include <chrono>
#include <ranges>
#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/duckdb.hpp>
#include <vector>

namespace test_range_select_from_with_timestamps {

struct Person {
  sqlgen::PrimaryKey<uint32_t, sqlgen::auto_incr> id;
  std::string first_name;
  std::string last_name;
  sqlgen::Date birthday;
};

TEST(duckdb, test_range_select_from_with_timestamps) {
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
                              cast<std::string>(day("birthday"_c)))))) |
              as<"birthday_recreated">,
          days_between("birthday"_c, Date("2011-01-01")) | as<"age_in_days">,
          unixepoch("birthday"_c + std::chrono::days(10)) |
              as<"birthday_unixepoch">,
          hour("birthday"_c) | as<"hour">, minute("birthday"_c) | as<"minute">,
          second("birthday"_c) | as<"second">,
          weekday("birthday"_c) | as<"weekday">) |
      order_by("id"_c) | to<std::vector<Birthday>>;

  const auto birthdays = duckdb::connect()
                             .and_then(write(std::ref(people1)))
                             .and_then(get_birthdays)
                             .value();

  const std::string expected_query =
      R"(SELECT "birthday" + INTERVAL '10 days' AS "birthday", cast((cast(extract(YEAR from "birthday") as TEXT) || '-' || cast(extract(MONTH from "birthday") as TEXT) || '-' || cast(extract(DAY from "birthday") as TEXT)) as DATE) AS "birthday_recreated", cast('2011-01-01' as DATE) - cast("birthday" as DATE) AS "age_in_days", extract(EPOCH FROM "birthday" + INTERVAL '10 days') AS "birthday_unixepoch", extract(HOUR from "birthday") AS "hour", extract(MINUTE from "birthday") AS "minute", extract(SECOND from "birthday") AS "second", extract(DOW from "birthday") AS "weekday" FROM "Person" ORDER BY "id")";
  const std::string expected =
      R"([{"birthday":"1970-01-11","birthday_recreated":"1970-01-01","birthday_unixepoch":864000,"age_in_days":14975.0,"hour":0,"minute":0,"second":0,"weekday":4},{"birthday":"2000-01-11","birthday_recreated":"2000-01-01","birthday_unixepoch":947548800,"age_in_days":4018.0,"hour":0,"minute":0,"second":0,"weekday":6},{"birthday":"2002-01-11","birthday_recreated":"2002-01-01","birthday_unixepoch":1010707200,"age_in_days":3287.0,"hour":0,"minute":0,"second":0,"weekday":2},{"birthday":"2010-01-11","birthday_recreated":"2010-01-01","birthday_unixepoch":1263168000,"age_in_days":365.0,"hour":0,"minute":0,"second":0,"weekday":5}])";

  EXPECT_EQ(duckdb::to_sql(get_birthdays), expected_query);
  EXPECT_EQ(rfl::json::write(birthdays), expected);
}

}  // namespace test_range_select_from_with_timestamps


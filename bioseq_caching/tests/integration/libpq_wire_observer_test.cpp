#include <catch2/catch_test_macros.hpp>

#include <chrono>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

TEST_CASE("single-row libpq mode observes wire arrival before completion",
          "[component:cache][layer:integration]"
          "[feature:reporting][resource:postgres]") {
    bioseq::postgres_test::private_cluster_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();

    const auto delayed =
        connection.execute_with_row_timing(
            "SELECT 1 AS stage, repeat('x', 1048576) AS payload "
            "UNION ALL "
            "SELECT 2, repeat('y', 1048576) "
            "FROM (SELECT pg_sleep(0.35)) AS delayed",
            10s);
    REQUIRE(delayed.outcome.row_count() == 2);
    REQUIRE(delayed.outcome.column_count() == 2);
    CHECK(delayed.outcome.text_value(0, 0) == "1");
    CHECK(delayed.outcome.text_value(1, 0) == "2");
    CHECK(delayed.outcome.text_value(0, 1).size() == 1048576);
    CHECK(delayed.outcome.text_value(1, 1).size() == 1048576);
    REQUIRE(delayed.time_to_first_row.has_value());
    CHECK(*delayed.time_to_first_row
          > std::chrono::nanoseconds::zero());
    CHECK(*delayed.time_to_first_row < delayed.elapsed);
    CHECK(delayed.elapsed - *delayed.time_to_first_row
          >= 150ms);

    const auto empty = connection.execute_with_row_timing(
        "SELECT 1 WHERE false", 10s);
    CHECK(empty.outcome.row_count() == 0);
    CHECK_FALSE(empty.time_to_first_row.has_value());
    CHECK(empty.elapsed > std::chrono::nanoseconds::zero());
}

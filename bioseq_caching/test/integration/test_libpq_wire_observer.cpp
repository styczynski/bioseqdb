#include <catch2/catch_test_macros.hpp>

#include <chrono>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

TEST_CASE("single-row libpq mode observes wire arrival before completion",
          "[cache][integration][libpq][wire]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "single-row libpq mode observes wire arrival before completion"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();

    const auto delayed =
        connection.exec_single_row_observed(
            "SELECT 1 AS phase, repeat('x', 1048576) AS payload "
            "UNION ALL "
            "SELECT 2, repeat('y', 1048576) "
            "FROM (SELECT pg_sleep(0.35)) AS delayed",
            10s);
    REQUIRE(delayed.rows.rows() == 2);
    REQUIRE(delayed.rows.columns() == 2);
    CHECK(delayed.rows.value(0, 0) == "1");
    CHECK(delayed.rows.value(1, 0) == "2");
    CHECK(delayed.rows.value(0, 1).size() == 1048576);
    CHECK(delayed.rows.value(1, 1).size() == 1048576);
    REQUIRE(delayed.time_to_first_row.has_value());
    CHECK(*delayed.time_to_first_row
          > std::chrono::nanoseconds::zero());
    CHECK(*delayed.time_to_first_row < delayed.elapsed);
    CHECK(delayed.elapsed - *delayed.time_to_first_row
          >= 150ms);
    bioseq::postgres_test::record_migration_probe(
        "harness.libpq.single_row_precedes_completion");

    const auto empty = connection.exec_single_row_observed(
        "SELECT 1 WHERE false", 10s);
    CHECK(empty.rows.rows() == 0);
    CHECK_FALSE(empty.time_to_first_row.has_value());
    CHECK(empty.elapsed > std::chrono::nanoseconds::zero());
    bioseq::postgres_test::record_migration_probe(
        "harness.libpq.empty_result_has_no_first_row");
}

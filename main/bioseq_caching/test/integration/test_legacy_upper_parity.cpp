#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string_view>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

namespace {

void require_window_rows(const bioseq::libpq::result& result,
                         std::string_view first,
                         std::string_view second,
                         std::string_view third,
                         std::string_view fourth) {
    REQUIRE(result.rows() == 4);
    REQUIRE(result.columns() == 2);
    CHECK(result.value(0, 0) == "1");
    CHECK(result.value(0, 1) == first);
    CHECK(result.value(1, 0) == "2");
    CHECK(result.value(1, 1) == second);
    CHECK(result.value(2, 0) == "3");
    CHECK(result.value(2, 1) == third);
    CHECK(result.value(3, 0) == "4");
    CHECK(result.value(3, 1) == fourth);
}

}  // namespace

TEST_CASE("aggregate replacement and dependency canonicalization stay current",
          "[cache][integration][exact][upper][aggregate][dependency][join]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "aggregate replacement and dependency canonicalization stay current"};

    SECTION("aggregate replacement and dependency canonicalization") {
        bioseq::postgres_test::postgres_fixture fixture{
            bioseq::cache_test::cluster_options(
                false, bioseq::cache_test::preload_order::cache_only)};
        auto& connection = fixture.admin();
        bioseq::cache_test::exec(
            connection,
            R"SQL(
                SET bioseq.query_cache_optimisation_mode = 'same';
                CREATE TABLE cache_upper_left(
                    id integer, key integer, payload text);
                CREATE TABLE cache_upper_right(
                    id integer, key integer, payload text);
                INSERT INTO cache_upper_left VALUES
                    (1, 1, 'l1'), (2, 1, 'l2'),
                    (3, 2, 'l3'), (4, NULL, 'ln');
                INSERT INTO cache_upper_right VALUES
                    (10, 1, 'r1'), (11, 1, 'r2'),
                    (12, 3, 'r3'), (13, NULL, 'rn');

                CREATE FUNCTION cache_replace_sum_state(integer, integer)
                RETURNS integer
                LANGUAGE SQL IMMUTABLE STRICT
                AS 'SELECT $1 + $2';
                CREATE AGGREGATE cache_replace_sum(integer) (
                    SFUNC = cache_replace_sum_state,
                    STYPE = integer,
                    INITCOND = '0'
                );
            )SQL");

        constexpr std::string_view stable_concat_query{
            "SELECT count(*) || '|' || sum(id) FROM cache_upper_left"};
        bioseq::cache_test::exec(
            connection,
            "SELECT bioseq_cache.invalidate_all(); "
            "SELECT bioseq_cache.reset_stats()");
        const auto stable_concat_first =
            connection.exec(stable_concat_query);
        const auto stable_concat_second =
            connection.exec(stable_concat_query);
        CHECK(bioseq::postgres_test::single_text_cell(
                  stable_concat_first)
              == "4|10");
        CHECK(bioseq::postgres_test::exact_row_hash(
                  stable_concat_first)
              == bioseq::postgres_test::exact_row_hash(
                  stable_concat_second));
        const auto stable_concat_stats =
            bioseq::cache_test::read_cache_stats(connection);
        CHECK(stable_concat_stats.produced == 0);
        CHECK(stable_concat_stats.completed_hits == 0);
        CHECK(stable_concat_stats.fallbacks == 0);

        constexpr std::string_view immutable_concat_query{
            "SELECT count(*)::text || '|' || sum(id)::text "
            "FROM cache_upper_left"};
        bioseq::cache_test::exec(
            connection,
            "SELECT bioseq_cache.invalidate_all(); "
            "SELECT bioseq_cache.reset_stats()");
        const auto immutable_concat_cold =
            connection.exec(immutable_concat_query);
        const auto immutable_concat_warm =
            connection.exec(immutable_concat_query);
        CHECK(bioseq::postgres_test::single_text_cell(
                  immutable_concat_cold)
              == "4|10");
        CHECK(bioseq::postgres_test::exact_row_hash(
                  immutable_concat_cold)
              == bioseq::postgres_test::exact_row_hash(
                  immutable_concat_warm));
        const auto immutable_concat_stats =
            bioseq::cache_test::read_cache_stats(connection);
        CHECK(immutable_concat_stats.produced == 1);
        CHECK(immutable_concat_stats.completed_hits == 1);
        CHECK(immutable_concat_stats.fallbacks == 0);

        constexpr std::string_view aggregate_query{
            "SELECT cache_replace_sum(id) FROM cache_upper_left"};
        bioseq::cache_test::exec(
            connection,
            "SELECT bioseq_cache.invalidate_all(); "
            "SELECT bioseq_cache.reset_stats()");
        const auto aggregate_cold = connection.exec(aggregate_query);
        const auto aggregate_warm = connection.exec(aggregate_query);
        CHECK(bioseq::postgres_test::single_text_cell(aggregate_cold)
              == "10");
        CHECK(bioseq::postgres_test::single_text_cell(aggregate_warm)
              == "10");
        CHECK(bioseq::postgres_test::exact_row_hash(aggregate_cold)
              == bioseq::postgres_test::exact_row_hash(aggregate_warm));
        const auto baseline_stats =
            bioseq::cache_test::read_cache_stats(connection);
        CHECK(baseline_stats.produced == 1);
        CHECK(baseline_stats.completed_hits == 1);
        CHECK(baseline_stats.fallbacks == 0);

        bioseq::cache_test::exec(
            connection,
            R"SQL(
                CREATE OR REPLACE FUNCTION
                    cache_replace_sum_state(integer, integer)
                RETURNS integer
                LANGUAGE SQL IMMUTABLE STRICT
                AS 'SELECT $1 + $2 + 100';
                SELECT bioseq_cache.reset_stats();
            )SQL");
        const auto aggregate_replaced_cold =
            connection.exec(aggregate_query);
        const auto aggregate_replaced_warm =
            connection.exec(aggregate_query);
        CHECK(bioseq::postgres_test::single_text_cell(
                  aggregate_replaced_cold)
              == "410");
        CHECK(bioseq::postgres_test::single_text_cell(
                  aggregate_replaced_warm)
              == "410");
        CHECK(bioseq::postgres_test::exact_row_hash(
                  aggregate_replaced_cold)
              == bioseq::postgres_test::exact_row_hash(
                  aggregate_replaced_warm));
        const auto replaced_stats =
            bioseq::cache_test::read_cache_stats(connection);
        CHECK(replaced_stats.produced == 1);
        CHECK(replaced_stats.completed_hits == 1);
        CHECK(replaced_stats.fallbacks == 0);

        bioseq::cache_test::exec(
            connection,
            R"SQL(
                CREATE FUNCTION
                    cache_replace_window_state(integer, integer)
                RETURNS integer
                LANGUAGE SQL IMMUTABLE STRICT
                AS 'SELECT $1 + $2';
                CREATE AGGREGATE cache_replace_window_sum(integer) (
                    SFUNC = cache_replace_window_state,
                    STYPE = integer,
                    INITCOND = '0'
                );
                SELECT bioseq_cache.invalidate_all();
                SELECT bioseq_cache.reset_stats();
            )SQL");
        constexpr std::string_view window_query{
            "SELECT id, cache_replace_window_sum(id) OVER ("
            "ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING "
            "AND CURRENT ROW) "
            "FROM cache_upper_left ORDER BY id"};
        const auto window_cold = connection.exec(window_query);
        const auto window_warm = connection.exec(window_query);
        require_window_rows(window_cold, "1", "3", "6", "10");
        CHECK(bioseq::postgres_test::exact_row_hash(window_cold)
              == bioseq::postgres_test::exact_row_hash(window_warm));
        const auto window_baseline_stats =
            bioseq::cache_test::read_cache_stats(connection);
        CHECK(window_baseline_stats.produced == 1);
        CHECK(window_baseline_stats.completed_hits == 1);
        CHECK(window_baseline_stats.fallbacks == 0);

        bioseq::cache_test::exec(
            connection,
            R"SQL(
                CREATE OR REPLACE FUNCTION
                    cache_replace_window_state(integer, integer)
                RETURNS integer
                LANGUAGE SQL IMMUTABLE STRICT
                AS 'SELECT $1 + $2 + 1000';
                SELECT bioseq_cache.reset_stats();
            )SQL");
        const auto window_replaced_cold =
            connection.exec(window_query);
        const auto window_replaced_warm =
            connection.exec(window_query);
        require_window_rows(
            window_replaced_cold, "1001", "2003", "3006", "4010");
        CHECK(bioseq::postgres_test::exact_row_hash(
                  window_replaced_cold)
              == bioseq::postgres_test::exact_row_hash(
                  window_replaced_warm));
        const auto window_replaced_stats =
            bioseq::cache_test::read_cache_stats(connection);
        CHECK(window_replaced_stats.produced == 1);
        CHECK(window_replaced_stats.completed_hits == 1);
        CHECK(window_replaced_stats.fallbacks == 0);

        bioseq::cache_test::exec(
            connection,
            "SELECT bioseq_cache.invalidate_all(); "
            "SELECT bioseq_cache.reset_stats()");
        const auto self_join = connection.exec(
            "SELECT left_row.id, right_row.id "
            "FROM cache_upper_left AS left_row "
            "JOIN cache_upper_left AS right_row "
            "ON left_row.id = right_row.id "
            "ORDER BY left_row.id, right_row.id");
        REQUIRE(self_join.rows() == 4);
        CHECK(bioseq::cache_test::integer(
                  connection,
                  "SELECT (dependencies->>'dependency_count')::integer "
                  "FROM bioseq_cache.entries() "
                  "WHERE kind = 'ExactFinalResult' "
                  "AND state = 'COMPLETE'")
              == 1);

        bioseq::cache_test::exec(
            connection, "SELECT bioseq_cache.invalidate_all()");
        const auto left_to_right = connection.exec(
            "SELECT left_row.id, right_row.id "
            "FROM cache_upper_left AS left_row "
            "JOIN cache_upper_right AS right_row "
            "ON left_row.key = right_row.key "
            "ORDER BY left_row.id, right_row.id");
        const auto right_to_left = connection.exec(
            "SELECT left_row.id, right_row.id "
            "FROM cache_upper_right AS right_row "
            "JOIN cache_upper_left AS left_row "
            "ON right_row.key = left_row.key "
            "ORDER BY left_row.id, right_row.id");
        REQUIRE(left_to_right.rows() == 4);
        CHECK(bioseq::postgres_test::exact_row_hash(left_to_right)
              == bioseq::postgres_test::exact_row_hash(right_to_left));
        CHECK(bioseq::cache_test::boolean(
            connection,
            "SELECT count(*) = 2 "
            "AND count(DISTINCT "
            "dependencies->>'dependency_fingerprint') = 1 "
            "AND bool_and(dependencies ? 'dependency_count') "
            "AND bool_and(dependencies ? 'dependency_fingerprint') "
            "FROM bioseq_cache.entries() "
            "WHERE kind = 'ExactFinalResult' "
            "AND state = 'COMPLETE' "
            "AND (dependencies->>'dependency_count')::integer = 2"));

        bioseq::cache_test::exec(
            connection,
            R"SQL(
                CREATE FUNCTION cache_volatile_sum_state(integer, integer)
                RETURNS integer
                LANGUAGE SQL VOLATILE
                AS 'SELECT $1 + $2';
                CREATE AGGREGATE cache_volatile_sum(integer) (
                    SFUNC = cache_volatile_sum_state,
                    STYPE = integer,
                    INITCOND = '0'
                );
                SELECT bioseq_cache.invalidate_all();
                SELECT bioseq_cache.reset_stats();
            )SQL");
        CHECK(bioseq::cache_test::scalar(
                  connection,
                  "SELECT cache_volatile_sum(id) "
                  "FROM cache_upper_left")
              == "10");
        const auto volatile_aggregate_stats =
            bioseq::cache_test::read_cache_stats(connection);
        CHECK(volatile_aggregate_stats.produced == 0);
        CHECK(volatile_aggregate_stats.completed_hits == 0);
        CHECK(volatile_aggregate_stats.fallbacks == 0);
        CHECK(bioseq::cache_test::integer(
                  connection,
                  "SELECT count(*) FROM cache_upper_left AS left_row "
                  "JOIN cache_upper_right AS right_row "
                  "ON left_row.key = right_row.key")
              == 4);
        CHECK(bioseq::cache_test::integer(
                  connection,
                  "SELECT count(*) FROM cache_upper_left AS left_row "
                  "FULL JOIN cache_upper_right AS right_row "
                  "ON left_row.key = right_row.key")
              == 8);

        bioseq::postgres_test::record_migration_probe(
            "regress.cache_joins_upper.aggregate_replacement_dependency_canon");
    }
}

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <string>
#include <string_view>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

namespace {

struct finalized_bypass_case {
    std::string_view fault;
    std::string_view planner_settings;
    std::string_view query;
    std::string_view expected_result;
};

constexpr std::array<finalized_bypass_case, 3> finalized_bypass_cases{{
    {
        "unknown_table_am",
        "SET enable_seqscan = on; "
        "SET enable_indexscan = off; "
        "SET enable_indexonlyscan = off; "
        "SET enable_bitmapscan = off",
        "SELECT sum(id)::bigint FROM cache_eligibility_rows",
        "3",
    },
    {
        "unknown_index_am",
        "SET enable_indexscan = on; "
        "SET enable_indexonlyscan = on; "
        "SET enable_seqscan = off; "
        "SET enable_bitmapscan = off",
        "SELECT value FROM cache_eligibility_rows WHERE id = 1",
        "one",
    },
    {
        "unknown_custom_scan",
        "RESET enable_seqscan; "
        "RESET enable_indexscan; "
        "RESET enable_indexonlyscan; "
        "RESET enable_bitmapscan",
        "SELECT value FROM cache_eligibility_rows WHERE id = 2",
        "two",
    },
}};

}  // namespace

TEST_CASE("unknown finalized scans bypass and cache diagnostics redact",
          "[component:cache][layer:integration]"
          "[feature:security][resource:postgres]") {

    SECTION("unknown AM, CustomScan, and diagnostic redaction") {
        bioseq::postgres_test::private_cluster_fixture fixture{
            bioseq::cache_test::cluster_options(
                false, bioseq::cache_test::preload_order::cache_only)};
        auto& connection = fixture.admin();
        bioseq::cache_test::execute(
            connection,
            R"SQL(
                SET bioseq_cache.query_cache.mode = 'exact_query';
                CREATE TABLE cache_eligibility_rows(
                    id integer PRIMARY KEY,
                    value text NOT NULL);
                INSERT INTO cache_eligibility_rows VALUES
                    (1, 'one'), (2, 'two');
            )SQL");

        for (const finalized_bypass_case& scenario :
             finalized_bypass_cases) {
            bioseq::cache_test::execute(
                connection, scenario.planner_settings);
            bioseq::cache_test::execute(
                connection,
                "SELECT bioseq_cache_test.set_exact_plan_fault("
                    + connection.escape_literal(scenario.fault) + "); "
                    "SELECT bioseq_cache.invalidate_all(); "
                    "SELECT bioseq_cache.reset_stats()");

            const auto first = connection.execute(scenario.query);
            const auto second = connection.execute(scenario.query);
            CHECK(bioseq::postgres_test::read_single_text_cell(first)
                  == scenario.expected_result);
            CHECK(bioseq::postgres_test::read_single_text_cell(second)
                  == scenario.expected_result);
            CHECK(bioseq::postgres_test::hash_ordered_result_rows(first)
                  == bioseq::postgres_test::hash_ordered_result_rows(second));

            const auto plan = bioseq::postgres_test::execute_explain(
                connection, scenario.query, false, 30s);
            const auto cache_nodes = plan.find_plan_nodes(
                "Custom Plan Provider", "BioseqCacheScan");
            REQUIRE(cache_nodes.size() == 1);
            CHECK(bioseq::postgres_test::read_plan_boolean_property(
                      *cache_nodes.front(), "Cache Force Bypass")
                  == true);
            CHECK(bioseq::postgres_test::read_plan_string_property(
                      *cache_nodes.front(), "Cache Bypass Reason")
                  == "unsafe finalized source path");

            const auto stats =
                bioseq::cache_test::read_cache_stats(connection);
            CHECK(stats.produced == 0);
            CHECK(stats.completed_hits == 0);
            CHECK(stats.inflight_waits == 0);
            CHECK(stats.fallbacks == 2);
            CHECK(bioseq::cache_test::integer(
                      connection,
                      "SELECT count(*) "
                      "FROM bioseq_cache_test.entry_states() "
                      "WHERE state IN ('BUILDING', 'COMPLETE')")
                  == 0);
        }
        bioseq::cache_test::execute(
            connection,
            "SELECT bioseq_cache_test.set_exact_plan_fault('none'); "
            "RESET enable_seqscan; "
            "RESET enable_indexscan; "
            "RESET enable_indexonlyscan; "
            "RESET enable_bitmapscan");

        bioseq::cache_test::execute(
            connection,
            R"SQL(
                CREATE ROLE cache_redaction_producer;
                CREATE ROLE cache_redaction_monitor;
                CREATE ROLE cache_redaction_stats_monitor;
                CREATE ROLE cache_redaction_denied;
                GRANT pg_read_all_stats
                    TO cache_redaction_stats_monitor;
                GRANT SELECT ON cache_eligibility_rows
                    TO cache_redaction_producer;
                GRANT USAGE ON SCHEMA bioseq_cache
                    TO cache_redaction_producer,
                       cache_redaction_monitor;
                GRANT EXECUTE ON FUNCTION
                    bioseq_cache.entries(),
                    bioseq_cache.stats()
                    TO cache_redaction_producer,
                       cache_redaction_monitor;
                SELECT bioseq_cache.invalidate_all();
                SELECT bioseq_cache.reset_stats();
                SET ROLE cache_redaction_producer;
                SET bioseq_cache.query_cache.mode = 'exact_query';
            )SQL");
        constexpr std::string_view private_literal{
            "qc-cache-private-literal-7f13"};
        const auto producer_result = connection.execute(
            "SELECT id, value, "
            + connection.escape_literal(private_literal)
            + "::text AS private_marker "
              "FROM cache_eligibility_rows WHERE id = 1");
        REQUIRE(producer_result.row_count() == 1);
        REQUIRE(producer_result.column_count() == 3);
        CHECK(producer_result.text_value(0, 0) == "1");
        CHECK(producer_result.text_value(0, 1) == "one");
        CHECK(producer_result.text_value(0, 2) == private_literal);
        bioseq::cache_test::execute(connection, "RESET ROLE");

        bioseq::cache_test::execute(
            connection, "SET ROLE cache_redaction_monitor");
        CHECK(bioseq::cache_test::boolean(
            connection,
            "SELECT count(*) = 1 "
            "AND bool_and(owner_role IS NULL) "
            "AND bool_and(producer_pid IS NULL) "
            "AND bool_and(sql IS NULL) "
            "AND bool_and(producer_plan IS NULL) "
            "AND bool_and(dependencies IS NULL) "
            "AND bool_and(rows = 1) "
            "AND bool_and(logical_bytes > 0) "
            "AND bool_and(stored_bytes > 0) "
            "FROM bioseq_cache.entries() "
            "WHERE kind = 'ExactFinalResult' "
            "AND state = 'COMPLETE'"));
        CHECK(bioseq::cache_test::integer(
                  connection,
                  "SELECT count(*) FROM bioseq_cache.stats()")
              == 1);
        bioseq::cache_test::execute(connection, "RESET ROLE");

        const std::string authorized_entry_predicate =
            "SELECT EXISTS ("
            "SELECT 1 FROM bioseq_cache.entries() "
            "WHERE owner_role = 'cache_redaction_producer'::regrole "
            "AND producer_pid IS NOT NULL "
            "AND sql LIKE "
            + connection.escape_literal(
                "%" + std::string(private_literal) + "%")
            + " AND dependencies IS NOT NULL)";
        bioseq::cache_test::execute(
            connection, "SET ROLE cache_redaction_producer");
        CHECK(bioseq::cache_test::boolean(
            connection, authorized_entry_predicate));
        bioseq::cache_test::execute(connection, "RESET ROLE");

        bioseq::cache_test::execute(
            connection, "SET ROLE cache_redaction_stats_monitor");
        CHECK(bioseq::cache_test::boolean(
            connection, authorized_entry_predicate));
        bioseq::cache_test::execute(connection, "RESET ROLE");

        CHECK_FALSE(bioseq::cache_test::boolean(
            connection,
            "SELECT has_function_privilege("
            "'cache_redaction_denied', "
            "'bioseq_cache.entries()', 'EXECUTE')"));
        bioseq::cache_test::execute(
            connection, "SET ROLE cache_redaction_denied");
        const auto denied = bioseq::postgres_test::execute_expecting_sql_state(
            connection,
            "SELECT * FROM bioseq_cache.entries()",
            "42501", 10s);
        CHECK(denied.sqlstate == "42501");
        bioseq::cache_test::execute(connection, "RESET ROLE");

    }
}

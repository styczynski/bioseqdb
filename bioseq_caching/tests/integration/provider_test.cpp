#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>
#include <string_view>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

namespace {

constexpr std::string_view provider_query{
    "SELECT row_id, base_id, occurrence_no, nullable_value, payload, "
    "capability_mask "
    "FROM cache_provider_rows "
    "WHERE nullable_value <= 10 "
    "ORDER BY row_id"};

constexpr std::string_view mapping_source_query{
    "SELECT row_id, base_id, occurrence_no, nullable_value, payload, "
    "capability_mask "
    "FROM cache_provider_rows "
    "WHERE nullable_value >= 5 "
    "ORDER BY row_id"};

constexpr std::string_view late_fetch_source_query{"SELECT row_id "
                                                   "FROM cache_provider_rows "
                                                   "WHERE nullable_value <= 10 "
                                                   "ORDER BY row_id"};

constexpr std::string_view late_fetch_consumer_query{"SELECT row_id "
                                                     "FROM cache_provider_rows "
                                                     "WHERE nullable_value BETWEEN 5 AND 20 "
                                                     "ORDER BY row_id"};

constexpr std::string_view late_fetch_text_source_query{"SELECT row_id "
                                                        "FROM cache_provider_rows "
                                                        "WHERE payload <> 'twenty' "
                                                        "ORDER BY row_id"};

constexpr std::string_view late_fetch_text_consumer_query{
    "SELECT row_id "
    "FROM cache_provider_rows "
    "WHERE payload = 'first match' OR payload = 'second match' "
    "ORDER BY row_id"};

void create_provider_fixture(bioseq::libpq::client_connection& connection) {
    bioseq::cache_test::execute(connection,
                                R"SQL(
            SET bioseq_cache.query_cache.mode = 'reusable_fragments';
            SET bioseq_cache.query_cache.completed_result_ttl_ms = '60 min';
            CREATE TABLE cache_provider_rows(
                row_id bigint PRIMARY KEY,
                base_id bigint NOT NULL,
                occurrence_no integer NOT NULL,
                nullable_value integer,
                payload text NOT NULL,
                capability_mask bigint NOT NULL,
                UNIQUE (base_id, occurrence_no)
            );
            INSERT INTO cache_provider_rows VALUES
                (1, 10, 0, NULL, 'null', 3),
                (2, 11, 0, 0,    'zero', 3),
                (3, 12, 0, 5,    'first match', 3),
                (4, 12, 1, 5,    'second match', 3),
                (5, 13, 0, 10,   'ten', 3),
                (6, 14, 0, 20,   'twenty', 3);
            SELECT bioseq_cache_test.provider_init();
            SELECT bioseq_cache_test.provider_use_relation(
                'cache_provider_rows');
            SELECT bioseq_cache_test.provider_reset();
            SELECT bioseq_cache.invalidate_all();
            SELECT bioseq_cache_test.request_janitor_sweep();
            SELECT bioseq_cache.reset_stats();
        )SQL");
}

void prime_mapping_source(bioseq::libpq::client_connection& connection) {
    bioseq::cache_test::execute(connection,
                                "SET bioseq_cache.query_cache.mode = 'reusable_fragments'; "
                                "SELECT bioseq_cache.invalidate_all(); "
                                "SELECT bioseq_cache_test.request_janitor_sweep(); "
                                "SELECT bioseq_cache_test.provider_set_mapping_fault('none')");
    static_cast<void>(connection.execute(mapping_source_query, 30s));
    bioseq::cache_test::execute(connection,
                                "SELECT bioseq_cache_test.provider_reset(); "
                                "SELECT bioseq_cache.reset_stats(); "
                                "DISCARD PLANS");
}

void require_provider_references_drained(bioseq::libpq::client_connection& connection) {
    CHECK(bioseq::cache_test::integer(connection,
                                      "SELECT reader_pins + waiter_references "
                                      "     + admission_reservations "
                                      "FROM bioseq_cache_test.planner_reference_counts()") == 0);
    CHECK(bioseq::cache_test::integer(connection,
                                      "SELECT count(*) "
                                      "FROM bioseq_cache_test.entry_states() "
                                      "WHERE state = 'BUILDING' "
                                      "   OR waiter_count <> 0 "
                                      "   OR reader_count <> 0") == 0);
}

} // namespace

TEST_CASE("provider registration owns its descriptor storage",
          "[component:cache][layer:integration]"
          "[feature:provider][resource:postgres]") {
    bioseq::postgres_test::private_cluster_fixture fixture{
        bioseq::cache_test::cluster_options(false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();

    bioseq::cache_test::execute(connection, "SELECT bioseq_cache_test.provider_init()");
    CHECK(bioseq::cache_test::boolean(connection,
                                      "SELECT bioseq_cache_test.provider_registry_owns_copy()"));
    bioseq::cache_test::execute(connection, "SELECT bioseq_cache_test.provider_shutdown()");
}

TEST_CASE("generic reusable provider produces and replays current coverage",
          "[component:cache][layer:integration]"
          "[feature:provider][resource:postgres]") {
    bioseq::postgres_test::private_cluster_fixture fixture{
        bioseq::cache_test::cluster_options(false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();
    create_provider_fixture(connection);

    const auto plan =
        bioseq::postgres_test::execute_explain(connection, provider_query, false, 30s);
    CHECK_FALSE(
        plan.find_plan_nodes("Custom Plan Provider", "BioseqCacheGenericTestProvider").empty());
    CHECK_FALSE(plan.find_plan_nodes("Cache Entry Kind", "ReusableCoverage").empty());

    bioseq::cache_test::execute(connection,
                                "SELECT bioseq_cache_test.provider_reset(); "
                                "SELECT bioseq_cache.reset_stats()");
    const auto cold = connection.execute(provider_query, 30s);
    const auto warm = connection.execute(provider_query, 30s);
    REQUIRE(cold.row_count() == 4);
    CHECK(bioseq::postgres_test::hash_ordered_result_rows(cold) ==
          bioseq::postgres_test::hash_ordered_result_rows(warm));
    const auto stats = bioseq::cache_test::read_cache_stats(connection);
    CHECK(stats.produced == 1);
    CHECK(stats.completed_hits == 0);
    CHECK(stats.reuse_hits == 1);
    CHECK(stats.fallbacks == 0);
    CHECK(bioseq::cache_test::integer(connection,
                                      "SELECT (bioseq_cache_test.provider_stats()"
                                      "->>'executor_rows')::bigint") == 4);
    CHECK(bioseq::cache_test::integer(connection,
                                      "SELECT (bioseq_cache_test.provider_stats()"
                                      "->>'extract_occurrence')::bigint") == 4);

    bioseq::cache_test::execute(connection, "SELECT bioseq_cache_test.provider_shutdown()");
}

TEST_CASE("provider status outcomes fail open without committing partial rows",
          "[component:cache][layer:integration]"
          "[feature:provider][resource:postgres]") {
    bioseq::postgres_test::private_cluster_fixture fixture{
        bioseq::cache_test::cluster_options(false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();
    create_provider_fixture(connection);

    bioseq::cache_test::execute(connection, "SET bioseq_cache.query_cache.mode = 'disabled'");
    const auto oracle = connection.execute(provider_query, 30s);
    const auto oracle_digest = bioseq::postgres_test::hash_ordered_result_rows(oracle);

    constexpr std::array<std::string_view, 2> explicit_fail_open_statuses{
        "provider_unsupported",
        "provider_status_error",
    };
    for (const std::string_view fault : explicit_fail_open_statuses) {
        prime_mapping_source(connection);
        bioseq::cache_test::execute(connection,
                                    "SELECT bioseq_cache_test.provider_set_mapping_fault(" +
                                        connection.escape_literal(fault) + ")");
        const auto declined = connection.execute(provider_query, 30s);
        CHECK(bioseq::postgres_test::hash_ordered_result_rows(declined) == oracle_digest);
        const auto stats = bioseq::cache_test::read_cache_stats(connection);
        CHECK(stats.produced == 1);
        CHECK(stats.completed_hits == 0);
        CHECK(stats.reuse_hits == 0);
        CHECK(stats.fallbacks == 0);
        CHECK(bioseq::cache_test::integer(connection,
                                          "SELECT (bioseq_cache_test.provider_stats()"
                                          "->>'map_source')::bigint") >= 1);
        require_provider_references_drained(connection);
        bioseq::cache_test::execute(connection,
                                    "SELECT bioseq_cache_test.provider_set_mapping_fault('none')");
    }

    prime_mapping_source(connection);
    bioseq::cache_test::execute(connection,
                                "SELECT bioseq_cache_test.provider_set_mapping_fault('none'); "
                                "SELECT bioseq_cache_test.set_coverage_descriptor_fault("
                                "'wrong_node_type'); "
                                "SELECT bioseq_cache.reset_stats()");
    const auto malformed = connection.execute(provider_query, 30s);
    CHECK(bioseq::postgres_test::hash_ordered_result_rows(malformed) == oracle_digest);

    bioseq::cache_test::execute(connection,
                                "SELECT bioseq_cache_test.set_coverage_descriptor_fault('none'); "
                                "SELECT bioseq_cache_test.provider_shutdown()");
}

TEST_CASE("provider PostgreSQL errors and interrupts preserve SQL semantics",
          "[component:cache][layer:integration]"
          "[feature:provider][resource:postgres]") {
    bioseq::postgres_test::private_cluster_fixture fixture{
        bioseq::cache_test::cluster_options(false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();
    create_provider_fixture(connection);

    bioseq::cache_test::execute(connection, "SET bioseq_cache.query_cache.mode = 'disabled'");
    const auto oracle = connection.execute(provider_query, 30s);
    const auto oracle_digest = bioseq::postgres_test::hash_ordered_result_rows(oracle);

    const auto require_propagated = [&connection,
                                     &oracle_digest](std::string_view fault,
                                                     std::string_view expected_sqlstate,
                                                     std::string_view stable_probe) {
        prime_mapping_source(connection);
        bioseq::cache_test::execute(connection,
                                    "SELECT bioseq_cache_test.provider_set_mapping_fault(" +
                                        connection.escape_literal(fault) + ")");
        bioseq::cache_test::execute(
            connection, "BEGIN", 10s, bioseq::libpq::transaction_policy::allow_open);
        const auto provider_error = bioseq::postgres_test::execute_expecting_sql_state(
            connection,
            provider_query,
            expected_sqlstate,
            30s,
            bioseq::libpq::transaction_policy::allow_open);
        CHECK(provider_error.sql_state == expected_sqlstate);
        const auto aborted = bioseq::postgres_test::execute_expecting_sql_state(
            connection, "SELECT 1", "25P02", 10s, bioseq::libpq::transaction_policy::allow_open);
        CHECK(aborted.sql_state == "25P02");
        bioseq::cache_test::execute(
            connection, "ROLLBACK", 10s, bioseq::libpq::transaction_policy::allow_open);
        bioseq::cache_test::execute(connection,
                                    "SELECT bioseq_cache_test.provider_set_mapping_fault('none')");
        const auto recovered = connection.execute(provider_query, 30s);
        CHECK(bioseq::postgres_test::hash_ordered_result_rows(recovered) == oracle_digest);
        require_provider_references_drained(connection);
    };

    require_propagated("provider_error", "XX000", "provider.postgres_error_aborts_transaction");
    require_propagated("provider_query_canceled", "57014", "provider.interrupt_is_rethrown");
    bioseq::cache_test::execute(connection, "SELECT bioseq_cache_test.provider_shutdown()");
}

TEST_CASE("late fetch status failure discards staged sink output",
          "[component:cache][layer:integration]"
          "[feature:provider][resource:postgres]") {
    bioseq::postgres_test::private_cluster_fixture fixture{
        bioseq::cache_test::cluster_options(false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();
    create_provider_fixture(connection);

    bioseq::cache_test::execute(connection, "SET bioseq_cache.query_cache.mode = 'disabled'");
    const auto oracle = connection.execute(late_fetch_consumer_query, 30s);
    const auto oracle_digest = bioseq::postgres_test::hash_ordered_result_rows(oracle);

    bioseq::cache_test::execute(connection,
                                "SET bioseq_cache.query_cache.mode = 'reusable_fragments'; "
                                "SELECT bioseq_cache.invalidate_all(); "
                                "SELECT bioseq_cache_test.request_janitor_sweep(); "
                                "SELECT bioseq_cache_test.provider_set_mapping_fault('none')");
    static_cast<void>(connection.execute(late_fetch_source_query, 30s));
    bioseq::cache_test::execute(connection,
                                "SELECT bioseq_cache_test.provider_reset(); "
                                "SELECT bioseq_cache.reset_stats(); "
                                "DISCARD PLANS; "
                                "SELECT bioseq_cache_test.provider_set_mapping_fault("
                                "'late_fetch_partial_status_error_once')");

    const auto recovered = connection.execute(late_fetch_consumer_query, 30s);
    CHECK(bioseq::postgres_test::hash_ordered_result_rows(recovered) == oracle_digest);
    CHECK(bioseq::cache_test::integer(connection,
                                      "SELECT (bioseq_cache_test.provider_stats()"
                                      "->>'late_fetch_batch')::bigint") >= 1);
    CHECK(bioseq::cache_test::integer(connection,
                                      "SELECT (bioseq_cache_test.provider_stats()"
                                      "->>'late_fetch_values')::bigint") >= 1);
    require_provider_references_drained(connection);

    const auto warm = connection.execute(late_fetch_consumer_query, 30s);
    CHECK(bioseq::postgres_test::hash_ordered_result_rows(warm) == oracle_digest);
    CHECK(bioseq::cache_test::integer(connection,
                                      "SELECT (bioseq_cache_test.provider_stats()"
                                      "->>'late_fetch_errors')::bigint") == 1);
    require_provider_references_drained(connection);
    bioseq::cache_test::execute(connection, "SELECT bioseq_cache_test.provider_shutdown()");
}

TEST_CASE("successful late fetch preserves missing text datums",
          "[component:cache][layer:integration]"
          "[feature:provider][resource:postgres]") {
    bioseq::postgres_test::private_cluster_fixture fixture{
        bioseq::cache_test::cluster_options(false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();
    create_provider_fixture(connection);

    bioseq::cache_test::execute(connection, "SET bioseq_cache.query_cache.mode = 'disabled'");
    const auto oracle = connection.execute(late_fetch_text_consumer_query, 30s);
    REQUIRE(oracle.row_count() == 2);
    REQUIRE(oracle.text_value(0, 0) == "3");
    REQUIRE(oracle.text_value(1, 0) == "4");
    const auto oracle_digest = bioseq::postgres_test::hash_ordered_result_rows(oracle);

    bioseq::cache_test::execute(connection,
                                "SET bioseq_cache.query_cache.mode = 'reusable_fragments'; "
                                "SET enable_indexscan = off; "
                                "SELECT bioseq_cache.invalidate_all(); "
                                "SELECT bioseq_cache_test.request_janitor_sweep(); "
                                "SELECT bioseq_cache_test.provider_set_mapping_fault('none')");
    const auto source_plan = bioseq::postgres_test::execute_explain(
        connection, late_fetch_text_source_query, false, 30s);
    REQUIRE_FALSE(
        source_plan.find_plan_nodes("Custom Plan Provider", "BioseqCacheGenericTestProvider")
            .empty());
    REQUIRE_FALSE(source_plan.find_plan_nodes("Cache Entry Kind", "ReusableCoverage").empty());
    const auto source = connection.execute(late_fetch_text_source_query, 30s);
    REQUIRE(source.row_count() == 5);
    for (int row = 0; row < source.row_count(); ++row) {
        CHECK(source.text_value(row, 0) == std::to_string(row + 1));
    }
    CHECK(bioseq::cache_test::integer(connection,
                                      "SELECT count(*) "
                                      "FROM bioseq_cache.entries() "
                                      "WHERE kind = 'ReusableCoverage' "
                                      "  AND state = 'COMPLETE'") == 1);
    bioseq::cache_test::execute(connection,
                                "RESET enable_indexscan; "
                                "SELECT bioseq_cache_test.provider_reset(); "
                                "SELECT bioseq_cache.reset_stats(); "
                                "DISCARD PLANS");

    const auto fetched = connection.execute(late_fetch_text_consumer_query, 30s);
    REQUIRE(fetched.row_count() == 2);
    CHECK(fetched.text_value(0, 0) == "3");
    CHECK(fetched.text_value(1, 0) == "4");
    CHECK(bioseq::postgres_test::hash_ordered_result_rows(fetched) == oracle_digest);
    CHECK(bioseq::cache_test::integer(connection,
                                      "SELECT (bioseq_cache_test.provider_stats()"
                                      "->>'late_fetch_batch')::bigint") == 1);
    CHECK(bioseq::cache_test::integer(connection,
                                      "SELECT (bioseq_cache_test.provider_stats()"
                                      "->>'late_fetch_references')::bigint") == 5);
    CHECK(bioseq::cache_test::integer(connection,
                                      "SELECT (bioseq_cache_test.provider_stats()"
                                      "->>'late_fetch_values')::bigint") == 5);
    require_provider_references_drained(connection);

    const auto warm = connection.execute(late_fetch_text_consumer_query, 30s);
    CHECK(bioseq::postgres_test::hash_ordered_result_rows(warm) == oracle_digest);
    CHECK(warm.text_value(0, 0) == "3");
    CHECK(warm.text_value(1, 0) == "4");
    bioseq::cache_test::execute(connection, "SELECT bioseq_cache_test.provider_shutdown()");
}

TEST_CASE("older reusable sources merge with a residual without stale rows",
          "[component:cache][layer:integration]"
          "[feature:provider][resource:postgres]") {
    bioseq::postgres_test::private_cluster_fixture fixture{
        bioseq::cache_test::cluster_options(false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();
    create_provider_fixture(connection);

    static_cast<void>(connection.execute("SELECT row_id FROM cache_provider_rows "
                                         "WHERE nullable_value <= 10 ORDER BY row_id",
                                         30s));
    bioseq::cache_test::execute(connection,
                                "SELECT bioseq_cache_test.provider_set_mapping_fault("
                                "'provider_status_error')");
    static_cast<void>(connection.execute("SELECT row_id FROM cache_provider_rows "
                                         "WHERE nullable_value >= 5 ORDER BY row_id",
                                         30s));
    bioseq::cache_test::execute(connection,
                                "SELECT bioseq_cache_test.provider_set_mapping_fault('none'); "
                                "SELECT bioseq_cache_test.provider_reset(); "
                                "SELECT bioseq_cache.reset_stats()");

    constexpr std::string_view merged_query{"SELECT row_id FROM cache_provider_rows "
                                            "WHERE nullable_value IS NULL "
                                            "OR nullable_value BETWEEN 5 AND 20 "
                                            "ORDER BY row_id"};
    const auto merged = connection.execute(merged_query, 30s);
    REQUIRE(merged.row_count() == 5);
    CHECK(merged.text_value(0, 0) == "1");
    CHECK(merged.text_value(4, 0) == "6");
    const auto merged_stats = bioseq::cache_test::read_cache_stats(connection);
    CHECK(merged_stats.produced == 1);
    CHECK(merged_stats.reuse_hits >= 1);
    CHECK(merged_stats.fallbacks == 0);

    bioseq::cache_test::execute(connection, "SELECT bioseq_cache_test.provider_shutdown()");
}

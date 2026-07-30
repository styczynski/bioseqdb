#include <catch2/catch_test_macros.hpp>

#include <array>
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

constexpr std::string_view late_fetch_source_query{
    "SELECT row_id "
    "FROM cache_provider_rows "
    "WHERE nullable_value <= 10 "
    "ORDER BY row_id"};

constexpr std::string_view late_fetch_consumer_query{
    "SELECT row_id "
    "FROM cache_provider_rows "
    "WHERE nullable_value BETWEEN 5 AND 20 "
    "ORDER BY row_id"};

constexpr std::string_view late_fetch_text_source_query{
    "SELECT row_id "
    "FROM cache_provider_rows "
    "WHERE row_id <= 5 "
    "ORDER BY row_id"};

constexpr std::string_view late_fetch_text_consumer_query{
    "SELECT row_id "
    "FROM cache_provider_rows "
    "WHERE payload IN ('first match', 'second match') "
    "ORDER BY row_id"};

void create_provider_fixture(bioseq::libpq::connection& connection) {
    bioseq::cache_test::exec(
        connection,
        R"SQL(
            SET bioseq.query_cache_optimisation_mode = 'reuse';
            SET bioseq.query_cache_ttl = '60 min';
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

void prime_mapping_source(bioseq::libpq::connection& connection) {
    bioseq::cache_test::exec(
        connection,
        "SET bioseq.query_cache_optimisation_mode = 'reuse'; "
        "SELECT bioseq_cache.invalidate_all(); "
        "SELECT bioseq_cache_test.request_janitor_sweep(); "
        "SELECT bioseq_cache_test.provider_set_mapping_fault('none')");
    static_cast<void>(connection.exec(mapping_source_query, 30s));
    bioseq::cache_test::exec(
        connection,
        "SELECT bioseq_cache_test.provider_reset(); "
        "SELECT bioseq_cache.reset_stats(); "
        "DISCARD PLANS");
}

void require_provider_references_drained(
    bioseq::libpq::connection& connection) {
    CHECK(bioseq::cache_test::integer(
              connection,
              "SELECT reader_pins + waiter_references "
              "     + admission_reservations "
              "FROM bioseq_cache_test.planner_reference_counts()")
          == 0);
    CHECK(bioseq::cache_test::integer(
              connection,
              "SELECT count(*) "
              "FROM bioseq_cache_test.entry_states() "
              "WHERE state = 'BUILDING' "
              "   OR waiter_count <> 0 "
              "   OR reader_count <> 0")
          == 0);
}

}  // namespace

TEST_CASE("provider registration owns its descriptor storage",
          "[cache][integration][provider][registry]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "provider registration owns its descriptor storage"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();

    bioseq::cache_test::exec(
        connection, "SELECT bioseq_cache_test.provider_init()");
    CHECK(bioseq::cache_test::boolean(
        connection,
        "SELECT bioseq_cache_test.provider_registry_owns_copy()"));
    bioseq::cache_test::exec(
        connection, "SELECT bioseq_cache_test.provider_shutdown()");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_provider_registry.owns_copy");
}

TEST_CASE("generic reusable provider produces and replays current coverage",
          "[cache][integration][provider][reuse][current]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "generic reusable provider produces and replays current coverage"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();
    create_provider_fixture(connection);

    const auto plan = bioseq::postgres_test::run_explain(
        connection, provider_query, false, 30s);
    CHECK_FALSE(plan
                    .find_plan_nodes(
                        "Custom Plan Provider",
                        "BioseqCacheGenericTestProvider")
                    .empty());
    CHECK_FALSE(plan
                    .find_plan_nodes(
                        "Cache Entry Kind", "ReusableCoverage")
                    .empty());
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_reuse_provider.custom_scan_selected");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_reuse_provider.reusable_plan_identity");

    bioseq::cache_test::exec(
        connection,
        "SELECT bioseq_cache_test.provider_reset(); "
        "SELECT bioseq_cache.reset_stats()");
    const auto cold = connection.exec(provider_query, 30s);
    const auto warm = connection.exec(provider_query, 30s);
    REQUIRE(cold.rows() == 4);
    CHECK(bioseq::postgres_test::exact_row_hash(cold)
          == bioseq::postgres_test::exact_row_hash(warm));
    const auto stats = bioseq::cache_test::read_cache_stats(connection);
    CHECK(stats.produced == 1);
    CHECK(stats.completed_hits == 0);
    CHECK(stats.reuse_hits == 1);
    CHECK(stats.fallbacks == 0);
    CHECK(bioseq::cache_test::integer(
              connection,
              "SELECT (bioseq_cache_test.provider_stats()"
              "->>'executor_rows')::bigint")
          == 4);
    CHECK(bioseq::cache_test::integer(
              connection,
              "SELECT (bioseq_cache_test.provider_stats()"
              "->>'extract_occurrence')::bigint")
          == 4);
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_reuse_provider.current_row_identity");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_reuse_provider.current_reuse_hit");
    bioseq::postgres_test::record_migration_probe(
        "isolation.cache_reuse_provider_current.no_child_on_replay");

    bioseq::cache_test::exec(
        connection, "SELECT bioseq_cache_test.provider_shutdown()");
}

TEST_CASE("provider status outcomes fail open without committing partial rows",
          "[cache][integration][provider][reuse][fault][status]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "provider status outcomes fail open without committing partial rows"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();
    create_provider_fixture(connection);

    bioseq::cache_test::exec(
        connection,
        "SET bioseq.query_cache_optimisation_mode = 'disabled'");
    const auto oracle = connection.exec(provider_query, 30s);
    const auto oracle_digest =
        bioseq::postgres_test::exact_row_hash(oracle);

    constexpr std::array<std::string_view, 2> explicit_fail_open_statuses{
        "provider_unsupported",
        "provider_status_error",
    };
    for (const std::string_view fault : explicit_fail_open_statuses) {
        prime_mapping_source(connection);
        bioseq::cache_test::exec(
            connection,
            "SELECT bioseq_cache_test.provider_set_mapping_fault("
                + connection.escape_literal(fault) + ")");
        const auto declined = connection.exec(provider_query, 30s);
        CHECK(bioseq::postgres_test::exact_row_hash(declined)
              == oracle_digest);
        CHECK(bioseq::cache_test::read_cache_stats(connection).fallbacks
              >= 1);
        require_provider_references_drained(connection);
        bioseq::cache_test::exec(
            connection,
            "SELECT bioseq_cache_test.provider_set_mapping_fault('none')");
    }
    bioseq::postgres_test::record_migration_probe(
        "provider.v2.unsupported_fails_open");
    bioseq::postgres_test::record_migration_probe(
        "provider.v2.status_error_fails_open");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_reuse_provider.mapping_fault_declines");

    prime_mapping_source(connection);
    bioseq::cache_test::exec(
        connection,
        "SELECT bioseq_cache_test.provider_set_mapping_fault('none'); "
        "SELECT bioseq_cache_test.set_coverage_descriptor_fault("
        "'wrong_node_type'); "
        "SELECT bioseq_cache.reset_stats()");
    const auto malformed = connection.exec(provider_query, 30s);
    CHECK(bioseq::postgres_test::exact_row_hash(malformed)
          == oracle_digest);
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_reuse_provider_binding.malformed_descriptor_declines");

    bioseq::cache_test::exec(
        connection,
        "SELECT bioseq_cache_test.set_coverage_descriptor_fault('none'); "
        "SELECT bioseq_cache_test.provider_shutdown()");
}

TEST_CASE("provider PostgreSQL errors and interrupts preserve SQL semantics",
          "[cache][integration][provider][reuse][fault][sqlstate]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "provider PostgreSQL errors and interrupts preserve SQL semantics"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();
    create_provider_fixture(connection);

    bioseq::cache_test::exec(
        connection,
        "SET bioseq.query_cache_optimisation_mode = 'disabled'");
    const auto oracle = connection.exec(provider_query, 30s);
    const auto oracle_digest =
        bioseq::postgres_test::exact_row_hash(oracle);

    const auto require_propagated =
        [&](std::string_view fault,
            std::string_view expected_sqlstate,
            std::string_view stable_probe) {
            prime_mapping_source(connection);
            bioseq::cache_test::exec(
                connection,
                "SELECT bioseq_cache_test.provider_set_mapping_fault("
                    + connection.escape_literal(fault) + ")");
            bioseq::cache_test::exec(
                connection, "BEGIN", 10s,
                bioseq::libpq::transaction_policy::allow_open);
            const auto provider_error =
                bioseq::postgres_test::expect_sqlstate(
                    connection, provider_query, expected_sqlstate, 30s,
                    bioseq::libpq::transaction_policy::allow_open);
            CHECK(provider_error.sqlstate == expected_sqlstate);
            const auto aborted =
                bioseq::postgres_test::expect_sqlstate(
                    connection, "SELECT 1", "25P02", 10s,
                    bioseq::libpq::transaction_policy::allow_open);
            CHECK(aborted.sqlstate == "25P02");
            bioseq::cache_test::exec(
                connection, "ROLLBACK", 10s,
                bioseq::libpq::transaction_policy::allow_open);
            bioseq::cache_test::exec(
                connection,
                "SELECT bioseq_cache_test.provider_set_mapping_fault('none')");
            const auto recovered = connection.exec(provider_query, 30s);
            CHECK(bioseq::postgres_test::exact_row_hash(recovered)
                  == oracle_digest);
            require_provider_references_drained(connection);
            bioseq::postgres_test::record_migration_probe(stable_probe);
        };

    require_propagated(
        "provider_error", "XX000",
        "provider.v2.postgres_error_aborts_transaction");
    require_propagated(
        "provider_query_canceled", "57014",
        "provider.v2.interrupt_is_rethrown");
    bioseq::cache_test::exec(
        connection, "SELECT bioseq_cache_test.provider_shutdown()");
}

TEST_CASE("late fetch status failure discards staged sink output",
          "[cache][integration][provider][reuse][fault][sink]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "late fetch status failure discards staged sink output"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();
    create_provider_fixture(connection);

    bioseq::cache_test::exec(
        connection,
        "SET bioseq.query_cache_optimisation_mode = 'disabled'");
    const auto oracle =
        connection.exec(late_fetch_consumer_query, 30s);
    const auto oracle_digest =
        bioseq::postgres_test::exact_row_hash(oracle);

    bioseq::cache_test::exec(
        connection,
        "SET bioseq.query_cache_optimisation_mode = 'reuse'; "
        "SELECT bioseq_cache.invalidate_all(); "
        "SELECT bioseq_cache_test.request_janitor_sweep(); "
        "SELECT bioseq_cache_test.provider_set_mapping_fault('none')");
    static_cast<void>(connection.exec(late_fetch_source_query, 30s));
    bioseq::cache_test::exec(
        connection,
        "SELECT bioseq_cache_test.provider_reset(); "
        "SELECT bioseq_cache.reset_stats(); "
        "DISCARD PLANS; "
        "SELECT bioseq_cache_test.provider_set_mapping_fault("
        "'late_fetch_partial_status_error_once')");

    const auto recovered =
        connection.exec(late_fetch_consumer_query, 30s);
    CHECK(bioseq::postgres_test::exact_row_hash(recovered)
          == oracle_digest);
    CHECK(bioseq::cache_test::integer(
              connection,
              "SELECT (bioseq_cache_test.provider_stats()"
              "->>'late_fetch_batch')::bigint")
          >= 1);
    CHECK(bioseq::cache_test::integer(
              connection,
              "SELECT (bioseq_cache_test.provider_stats()"
              "->>'late_fetch_values')::bigint")
          >= 1);
    require_provider_references_drained(connection);

    const auto warm =
        connection.exec(late_fetch_consumer_query, 30s);
    CHECK(bioseq::postgres_test::exact_row_hash(warm)
          == oracle_digest);
    CHECK(bioseq::cache_test::integer(
              connection,
              "SELECT (bioseq_cache_test.provider_stats()"
              "->>'late_fetch_errors')::bigint")
          == 1);
    require_provider_references_drained(connection);
    bioseq::postgres_test::record_migration_probe(
        "provider.v2.staged_sink_discards_partial_output");
    bioseq::postgres_test::record_migration_probe(
        "provider.v2.finish_abort_cleanup_drains_references");
    bioseq::postgres_test::record_migration_probe(
        "provider.v2.partial_sink_fault_resets_automatically");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_reuse_provider.source_error");
    bioseq::cache_test::exec(
        connection, "SELECT bioseq_cache_test.provider_shutdown()");
}

TEST_CASE("successful late fetch preserves missing text datums",
          "[cache][integration][provider][reuse][late-fetch][varlena]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "successful late fetch preserves missing text datums"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();
    create_provider_fixture(connection);

    bioseq::cache_test::exec(
        connection,
        "SET bioseq.query_cache_optimisation_mode = 'disabled'");
    const auto oracle =
        connection.exec(late_fetch_text_consumer_query, 30s);
    REQUIRE(oracle.rows() == 2);
    REQUIRE(oracle.value(0, 0) == "3");
    REQUIRE(oracle.value(1, 0) == "4");
    const auto oracle_digest =
        bioseq::postgres_test::exact_row_hash(oracle);

    bioseq::cache_test::exec(
        connection,
        "SET bioseq.query_cache_optimisation_mode = 'reuse'; "
        "SELECT bioseq_cache.invalidate_all(); "
        "SELECT bioseq_cache_test.request_janitor_sweep(); "
        "SELECT bioseq_cache_test.provider_set_mapping_fault('none')");
    static_cast<void>(
        connection.exec(late_fetch_text_source_query, 30s));
    bioseq::cache_test::exec(
        connection,
        "SELECT bioseq_cache_test.provider_reset(); "
        "SELECT bioseq_cache.reset_stats(); "
        "DISCARD PLANS");

    const auto fetched =
        connection.exec(late_fetch_text_consumer_query, 30s);
    REQUIRE(fetched.rows() == 2);
    CHECK(fetched.value(0, 0) == "3");
    CHECK(fetched.value(1, 0) == "4");
    CHECK(bioseq::postgres_test::exact_row_hash(fetched)
          == oracle_digest);
    CHECK(bioseq::cache_test::integer(
              connection,
              "SELECT (bioseq_cache_test.provider_stats()"
              "->>'late_fetch_batch')::bigint")
          == 1);
    CHECK(bioseq::cache_test::integer(
              connection,
              "SELECT (bioseq_cache_test.provider_stats()"
              "->>'late_fetch_references')::bigint")
          == 5);
    CHECK(bioseq::cache_test::integer(
              connection,
              "SELECT (bioseq_cache_test.provider_stats()"
              "->>'late_fetch_values')::bigint")
          == 5);
    require_provider_references_drained(connection);

    const auto warm =
        connection.exec(late_fetch_text_consumer_query, 30s);
    CHECK(bioseq::postgres_test::exact_row_hash(warm)
          == oracle_digest);
    CHECK(warm.value(0, 0) == "3");
    CHECK(warm.value(1, 0) == "4");
    bioseq::postgres_test::record_migration_probe(
        "provider.v2.late_fetch_varlena_lifetime");
    bioseq::cache_test::exec(
        connection, "SELECT bioseq_cache_test.provider_shutdown()");
}

TEST_CASE("older reusable sources merge with a residual without stale rows",
          "[cache][integration][provider][reuse][older]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "older reusable sources merge with a residual without stale rows"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();
    create_provider_fixture(connection);

    static_cast<void>(connection.exec(
        "SELECT row_id FROM cache_provider_rows "
        "WHERE nullable_value <= 10 ORDER BY row_id",
        30s));
    bioseq::cache_test::exec(
        connection,
        "SELECT bioseq_cache_test.provider_set_mapping_fault("
        "'provider_status_error')");
    static_cast<void>(connection.exec(
        "SELECT row_id FROM cache_provider_rows "
        "WHERE nullable_value >= 5 ORDER BY row_id",
        30s));
    bioseq::cache_test::exec(
        connection,
        "SELECT bioseq_cache_test.provider_set_mapping_fault('none'); "
        "SELECT bioseq_cache_test.provider_reset(); "
        "SELECT bioseq_cache.reset_stats()");

    constexpr std::string_view merged_query{
        "SELECT row_id FROM cache_provider_rows "
        "WHERE nullable_value IS NULL "
        "OR nullable_value BETWEEN 5 AND 20 "
        "ORDER BY row_id"};
    const auto merged = connection.exec(merged_query, 30s);
    REQUIRE(merged.rows() == 5);
    CHECK(merged.value(0, 0) == "1");
    CHECK(merged.value(4, 0) == "6");
    const auto merged_stats =
        bioseq::cache_test::read_cache_stats(connection);
    CHECK(merged_stats.produced == 1);
    CHECK(merged_stats.reuse_hits >= 1);
    CHECK(merged_stats.fallbacks == 0);
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_reuse_provider_older.two_source_merge");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_reuse_provider_older.residual_correctness");
    bioseq::postgres_test::record_migration_probe(
        "isolation.cache_reuse_provider_sources.older_sources_visible");

    bioseq::cache_test::exec(
        connection, "SELECT bioseq_cache_test.provider_shutdown()");
}

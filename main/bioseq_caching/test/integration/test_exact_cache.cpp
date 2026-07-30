#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <string_view>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

namespace {

constexpr std::string_view ordered_query{
    "SELECT id, value FROM cache_exact_rows ORDER BY id"};

}  // namespace

TEST_CASE("disabled and same modes preserve exact rows and counters",
          "[cache][integration][exact][explain][guc]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "disabled and same modes preserve exact rows and counters"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& admin = fixture.admin();
    auto data = fixture.connect();

    bioseq::cache_test::exec(
        admin,
        "CREATE TABLE cache_exact_rows("
        "id integer PRIMARY KEY, value text NOT NULL); "
        "INSERT INTO cache_exact_rows VALUES "
        "(1, 'alpha'), (2, 'beta'), (3, 'beta')");
    bioseq::cache_test::exec(
        admin,
        "SET bioseq.query_cache_optimisation_mode = 'disabled'; "
        "SELECT bioseq_cache.reset_stats()");
    bioseq::cache_test::exec(
        data, "SET bioseq.query_cache_optimisation_mode = 'disabled'");

    const bioseq::libpq::result oracle =
        data.exec(ordered_query, 30s);
    REQUIRE(oracle.rows() == 3);
    CHECK(oracle.value(0, 0) == "1");
    CHECK(oracle.value(2, 1) == "beta");
    const auto oracle_digest =
        bioseq::postgres_test::exact_row_hash(oracle);

    const auto disabled_plan = bioseq::postgres_test::run_explain(
        data, ordered_query, false, 30s);
    CHECK(disabled_plan
              .find_plan_nodes("Custom Plan Provider", "BioseqCacheScan")
              .empty());
    const auto disabled_stats =
        bioseq::cache_test::read_cache_stats(admin);
    CHECK(disabled_stats.produced == 0);
    CHECK(disabled_stats.completed_hits == 0);
    CHECK(disabled_stats.inflight_waits == 0);
    CHECK(disabled_stats.reuse_hits == 0);
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_disabled.rows");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_disabled.no_cache_plan");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_disabled.zero_counters");

    bioseq::cache_test::exec(
        admin,
        "SELECT bioseq_cache.invalidate_all(); "
        "SELECT bioseq_cache.reset_stats()");
    bioseq::cache_test::exec(
        data, "SET bioseq.query_cache_optimisation_mode = 'same'");

    const bioseq::cache_test::cache_stats before =
        bioseq::cache_test::read_cache_stats(admin);
    const bioseq::libpq::result cold = data.exec(ordered_query, 30s);
    const bioseq::cache_test::cache_stats after_cold =
        bioseq::cache_test::read_cache_stats(admin);
    CHECK(bioseq::postgres_test::exact_row_hash(cold) == oracle_digest);
    const auto cold_delta = after_cold - before;
    CHECK(cold_delta.produced == 1);
    CHECK(cold_delta.completed_hits == 0);
    CHECK(cold_delta.fallbacks == 0);
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_same_params.cold_identity");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_same_params.cold_produced_once");

    const bioseq::libpq::result warm = data.exec(ordered_query, 30s);
    const bioseq::cache_test::cache_stats after_warm =
        bioseq::cache_test::read_cache_stats(admin);
    CHECK(bioseq::postgres_test::exact_row_hash(warm) == oracle_digest);
    const auto warm_delta = after_warm - after_cold;
    CHECK(warm_delta.produced == 0);
    CHECK(warm_delta.completed_hits == 1);
    CHECK(warm_delta.fallbacks == 0);
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_same_params.warm_identity");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_same_params.warm_hit_once");

    CHECK(bioseq::cache_test::integer(
              admin,
              "SELECT count(*) FROM bioseq_cache.entries() "
              "WHERE kind = 'ExactFinalResult' "
              "AND state = 'COMPLETE' "
              "AND rows = 3 "
              "AND logical_bytes > 0 "
              "AND stored_bytes > 0")
          == 1);
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_same_params.complete_entry");

    const auto hit_plan = bioseq::postgres_test::run_explain(
        data, ordered_query, true, 30s);
    const auto cache_nodes =
        hit_plan.find_plan_nodes("Custom Plan Provider", "BioseqCacheScan");
    REQUIRE(cache_nodes.size() == 1);
    CHECK(bioseq::postgres_test::explain_property_string(
              *cache_nodes.front(), "Actual Cache Action")
          == "[Hit] exact final result");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_same_params.explain_hit");

    bioseq::cache_test::exec(
        admin, "UPDATE cache_exact_rows SET value = 'gamma' WHERE id = 2");
    const auto invalidated_before =
        bioseq::cache_test::read_cache_stats(admin);
    const bioseq::libpq::result refreshed = data.exec(ordered_query, 30s);
    const auto invalidated_after =
        bioseq::cache_test::read_cache_stats(admin);
    CHECK(refreshed.value(1, 1) == "gamma");
    CHECK(bioseq::postgres_test::exact_row_hash(refreshed)
          != oracle_digest);
    CHECK((invalidated_after - invalidated_before).produced == 1);
    CHECK(invalidated_after.invalidations
          >= invalidated_before.invalidations);
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_dml_transactions.committed_write_invalidates");
}

TEST_CASE("exact cache handles parameters, nulls, duplicates, and empty sets",
          "[cache][integration][exact][parameters][null]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "exact cache handles parameters, nulls, duplicates, and empty sets"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();

    bioseq::cache_test::exec(
        connection,
        "SET bioseq.query_cache_optimisation_mode = 'same'; "
        "CREATE TABLE cache_parameter_rows("
        "id integer, value integer); "
        "INSERT INTO cache_parameter_rows VALUES "
        "(1, NULL), (2, 4), (2, 4), (3, 9); "
        "PREPARE cache_parameter_query(integer) AS "
        "SELECT id, value FROM cache_parameter_rows "
        "WHERE value IS NOT DISTINCT FROM $1 ORDER BY id, value");

    const auto null_first = connection.exec(
        "SELECT id, value FROM cache_parameter_rows "
        "WHERE value IS NULL ORDER BY id");
    const auto null_second = connection.exec(
        "SELECT id, value FROM cache_parameter_rows "
        "WHERE value IS NULL ORDER BY id");
    CHECK(bioseq::postgres_test::exact_row_hash(null_first)
          == bioseq::postgres_test::exact_row_hash(null_second));
    CHECK(null_second.rows() == 1);
    CHECK(null_second.is_null(0, 1));
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_same_params.null_round_trip");

    const auto duplicate_first =
        connection.exec("EXECUTE cache_parameter_query(4)");
    const auto duplicate_second =
        connection.exec("EXECUTE cache_parameter_query(4)");
    CHECK(duplicate_first.rows() == 2);
    CHECK(bioseq::postgres_test::exact_row_hash(duplicate_first)
          == bioseq::postgres_test::exact_row_hash(duplicate_second));
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_same_params.parameter_key");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_same_params.duplicates_preserved");

    const auto empty_first =
        connection.exec("EXECUTE cache_parameter_query(999)");
    const auto empty_second =
        connection.exec("EXECUTE cache_parameter_query(999)");
    CHECK(empty_first.rows() == 0);
    CHECK(bioseq::postgres_test::exact_row_hash(empty_first)
          == bioseq::postgres_test::exact_row_hash(empty_second));
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_same_params.empty_result");
}

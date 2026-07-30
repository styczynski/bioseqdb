#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <string_view>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

namespace {

constexpr std::string_view prepared_query{
    "SELECT id FROM cache_prepared_rows ORDER BY id"};

}  // namespace

TEST_CASE("prepared commit and rollback control cache visibility",
          "[cache][integration][transactions][two-phase]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "prepared commit and rollback control cache visibility"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only,
            {{"max_prepared_transactions", "16"}})};
    auto& admin = fixture.admin();
    bioseq::cache_test::exec(
        admin,
        "CREATE TABLE cache_prepared_rows(id integer PRIMARY KEY); "
        "INSERT INTO cache_prepared_rows VALUES (1), (2); "
        "SET bioseq.query_cache_optimisation_mode = 'same'");
    const auto baseline = admin.exec(prepared_query);
    const auto baseline_hit = admin.exec(prepared_query);
    REQUIRE(baseline.rows() == 2);
    CHECK(bioseq::postgres_test::exact_row_hash(baseline)
          == bioseq::postgres_test::exact_row_hash(baseline_hit));
    bioseq::postgres_test::record_migration_probe(
        "tap.009.two_phase.baseline_stable");

    {
        bioseq::postgres_test::scoped_prepared_transaction transaction{
            fixture.cluster(), "cache_commit_prepared"};
        bioseq::cache_test::exec(
            transaction.session(),
            "INSERT INTO cache_prepared_rows VALUES (3)", 10s,
            bioseq::libpq::transaction_policy::allow_open);
        transaction.prepare();
        CHECK(bioseq::cache_test::integer(
                  admin,
                  "SELECT count(*) FROM cache_prepared_rows")
              == 2);
        transaction.commit_prepared();
    }
    const auto committed = admin.exec(prepared_query);
    REQUIRE(committed.rows() == 3);
    CHECK(committed.value(2, 0) == "3");
    bioseq::postgres_test::record_migration_probe(
        "tap.009.two_phase.commit_hides_until_resolution");
    bioseq::postgres_test::record_migration_probe(
        "tap.009.two_phase.commit_exposes_row");
    bioseq::postgres_test::record_migration_probe(
        "isolation.cache_prepared_transition.commit_visibility");

    {
        bioseq::postgres_test::scoped_prepared_transaction transaction{
            fixture.cluster(), "cache_rollback_prepared"};
        bioseq::cache_test::exec(
            transaction.session(),
            "INSERT INTO cache_prepared_rows VALUES (4)", 10s,
            bioseq::libpq::transaction_policy::allow_open);
        transaction.prepare();
        transaction.rollback_prepared();
    }
    const auto rolled_back = admin.exec(prepared_query);
    REQUIRE(rolled_back.rows() == 3);
    CHECK(rolled_back.value(2, 0) == "3");
    bioseq::postgres_test::record_migration_probe(
        "tap.009.two_phase.rollback_hides_row");
    bioseq::postgres_test::record_migration_probe(
        "isolation.cache_prepared_transition.rollback_visibility");

    const auto missing = bioseq::postgres_test::expect_sqlstate(
        admin,
        "COMMIT PREPARED 'cache_missing_gid'",
        "42704", 10s);
    CHECK(missing.message.find("does not exist") != std::string::npos);
    const auto after_error = admin.exec(prepared_query);
    REQUIRE(after_error.rows() == 3);
    bioseq::postgres_test::record_migration_probe(
        "tap.009.two_phase.missing_gid_sqlstate");
    bioseq::postgres_test::record_migration_probe(
        "tap.009.two_phase.error_cleanup_cache_usable");
}

TEST_CASE("prepared transitions reconstruct across postmaster restart",
          "[cache][integration][transactions][two-phase][restart]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "prepared transitions reconstruct across postmaster restart"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only,
            {{"max_prepared_transactions", "16"}})};
    auto& admin = fixture.admin();
    bioseq::cache_test::exec(
        admin,
        "CREATE TABLE cache_prepared_restart_rows("
        "id integer PRIMARY KEY); "
        "INSERT INTO cache_prepared_restart_rows VALUES (1), (2); "
        "SET bioseq.query_cache_optimisation_mode = 'same'");

    auto preparer = fixture.connect();
    bioseq::cache_test::exec(
        preparer, "BEGIN", 10s,
        bioseq::libpq::transaction_policy::allow_open);
    bioseq::cache_test::exec(
        preparer,
        "INSERT INTO cache_prepared_restart_rows VALUES (3)",
        10s, bioseq::libpq::transaction_policy::allow_open);
    bioseq::cache_test::exec(
        preparer,
        "PREPARE TRANSACTION 'cache_restart_commit'",
        10s, bioseq::libpq::transaction_policy::allow_open);

    fixture.cluster().restart();
    fixture.reconnect_admin();
    auto& recovered = fixture.admin();
    CHECK(bioseq::cache_test::boolean(
        recovered,
        "SELECT (bioseq_cache_test.bootstrap_status()->>"
        "'storage_attached')::boolean "
        "AND bioseq_cache_test.cache_infrastructure_ready()"));
    CHECK(bioseq::cache_test::integer(
              recovered,
              "SELECT count(*) FROM cache_prepared_restart_rows")
          == 2);
    bioseq::cache_test::exec(
        recovered, "COMMIT PREPARED 'cache_restart_commit'");
    CHECK(bioseq::cache_test::integer(
              recovered,
              "SELECT count(*) FROM cache_prepared_restart_rows")
          == 3);
    bioseq::postgres_test::record_migration_probe(
        "tap.009.two_phase.restart_gate_reconstructed");
    bioseq::postgres_test::record_migration_probe(
        "tap.009.two_phase.commit_after_restart");

    auto rollback_preparer = fixture.connect();
    bioseq::cache_test::exec(
        rollback_preparer, "BEGIN", 10s,
        bioseq::libpq::transaction_policy::allow_open);
    bioseq::cache_test::exec(
        rollback_preparer,
        "INSERT INTO cache_prepared_restart_rows VALUES (4)",
        10s, bioseq::libpq::transaction_policy::allow_open);
    bioseq::cache_test::exec(
        rollback_preparer,
        "PREPARE TRANSACTION 'cache_restart_rollback'",
        10s, bioseq::libpq::transaction_policy::allow_open);
    fixture.cluster().restart();
    fixture.reconnect_admin();
    auto& rollback_recovered = fixture.admin();
    CHECK(bioseq::cache_test::boolean(
        rollback_recovered,
        "SELECT (bioseq_cache_test.bootstrap_status()->>"
        "'storage_attached')::boolean "
        "AND bioseq_cache_test.cache_infrastructure_ready()"));
    bioseq::cache_test::exec(
        rollback_recovered,
        "ROLLBACK PREPARED 'cache_restart_rollback'");
    CHECK(bioseq::cache_test::integer(
              rollback_recovered,
              "SELECT count(*) FROM cache_prepared_restart_rows")
          == 3);
    bioseq::postgres_test::record_migration_probe(
        "tap.009.two_phase.rollback_after_restart");
}

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
          "[component:cache][layer:integration]"
          "[feature:transaction][resource:postgres]") {
    bioseq::postgres_test::private_cluster_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only,
            {{"max_prepared_transactions", "16"}})};
    auto& admin = fixture.admin();
    bioseq::cache_test::execute(
        admin,
        "CREATE TABLE cache_prepared_rows(id integer PRIMARY KEY); "
        "INSERT INTO cache_prepared_rows VALUES (1), (2); "
        "SET bioseq_cache.query_cache.mode = 'exact_query'");
    const auto baseline = admin.execute(prepared_query);
    const auto baseline_hit = admin.execute(prepared_query);
    REQUIRE(baseline.row_count() == 2);
    CHECK(bioseq::postgres_test::hash_ordered_result_rows(baseline)
          == bioseq::postgres_test::hash_ordered_result_rows(baseline_hit));

    {
        bioseq::postgres_test::scoped_prepared_transaction transaction{
            fixture.cluster(), "cache_commit_prepared"};
        bioseq::cache_test::execute(
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
    const auto committed = admin.execute(prepared_query);
    REQUIRE(committed.row_count() == 3);
    CHECK(committed.text_value(2, 0) == "3");

    {
        bioseq::postgres_test::scoped_prepared_transaction transaction{
            fixture.cluster(), "cache_rollback_prepared"};
        bioseq::cache_test::execute(
            transaction.session(),
            "INSERT INTO cache_prepared_rows VALUES (4)", 10s,
            bioseq::libpq::transaction_policy::allow_open);
        transaction.prepare();
        transaction.rollback_prepared();
    }
    const auto rolled_back = admin.execute(prepared_query);
    REQUIRE(rolled_back.row_count() == 3);
    CHECK(rolled_back.text_value(2, 0) == "3");

    const auto missing = bioseq::postgres_test::execute_expecting_sql_state(
        admin,
        "COMMIT PREPARED 'cache_missing_gid'",
        "42704", 10s);
    CHECK(missing.message.find("does not exist") != std::string::npos);
    const auto after_error = admin.execute(prepared_query);
    REQUIRE(after_error.row_count() == 3);
}

TEST_CASE("prepared transitions reconstruct across postmaster restart",
          "[component:cache][layer:integration]"
          "[feature:transaction][resource:postgres]") {
    bioseq::postgres_test::private_cluster_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only,
            {{"max_prepared_transactions", "16"}})};
    auto& admin = fixture.admin();
    bioseq::cache_test::execute(
        admin,
        "CREATE TABLE cache_prepared_restart_rows("
        "id integer PRIMARY KEY); "
        "INSERT INTO cache_prepared_restart_rows VALUES (1), (2); "
        "SET bioseq_cache.query_cache.mode = 'exact_query'");

    auto preparer = fixture.connect();
    bioseq::cache_test::execute(
        preparer, "BEGIN", 10s,
        bioseq::libpq::transaction_policy::allow_open);
    bioseq::cache_test::execute(
        preparer,
        "INSERT INTO cache_prepared_restart_rows VALUES (3)",
        10s, bioseq::libpq::transaction_policy::allow_open);
    bioseq::cache_test::execute(
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
    bioseq::cache_test::execute(
        recovered, "COMMIT PREPARED 'cache_restart_commit'");
    CHECK(bioseq::cache_test::integer(
              recovered,
              "SELECT count(*) FROM cache_prepared_restart_rows")
          == 3);

    auto rollback_preparer = fixture.connect();
    bioseq::cache_test::execute(
        rollback_preparer, "BEGIN", 10s,
        bioseq::libpq::transaction_policy::allow_open);
    bioseq::cache_test::execute(
        rollback_preparer,
        "INSERT INTO cache_prepared_restart_rows VALUES (4)",
        10s, bioseq::libpq::transaction_policy::allow_open);
    bioseq::cache_test::execute(
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
    bioseq::cache_test::execute(
        rollback_recovered,
        "ROLLBACK PREPARED 'cache_restart_rollback'");
    CHECK(bioseq::cache_test::integer(
              rollback_recovered,
              "SELECT count(*) FROM cache_prepared_restart_rows")
          == 3);
}

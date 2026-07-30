#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

TEST_CASE("joins and upper relations replay exact ordered results",
          "[cache][integration][exact][join][upper]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "joins and upper relations replay exact ordered results"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();
    bioseq::cache_test::exec(
        connection,
        "SET bioseq.query_cache_optimisation_mode = 'same'; "
        "CREATE TABLE cache_join_left("
        "id integer, key integer, payload text); "
        "CREATE TABLE cache_join_right("
        "id integer, key integer, payload text); "
        "INSERT INTO cache_join_left VALUES "
        "(1, 1, 'a'), (2, 1, 'b'), (3, 2, 'c'), (4, NULL, 'd'); "
        "INSERT INTO cache_join_right VALUES "
        "(10, 1, 'x'), (11, 2, 'y'), (12, 3, 'z')");

    constexpr std::array<std::string_view, 6> queries{
        "SELECT l.id, r.id FROM cache_join_left AS l "
        "JOIN cache_join_right AS r USING (key) ORDER BY l.id, r.id",
        "SELECT key, count(*), min(id), max(id) "
        "FROM cache_join_left GROUP BY key ORDER BY key NULLS FIRST",
        "SELECT DISTINCT key FROM cache_join_left "
        "ORDER BY key NULLS FIRST",
        "SELECT id, row_number() OVER (PARTITION BY key ORDER BY id) "
        "FROM cache_join_left ORDER BY id",
        "(SELECT key FROM cache_join_left WHERE key IS NOT NULL) "
        "INTERSECT "
        "(SELECT key FROM cache_join_right WHERE key IS NOT NULL) "
        "ORDER BY key",
        "(SELECT key FROM cache_join_left WHERE key IS NOT NULL) "
        "EXCEPT "
        "(SELECT key FROM cache_join_right WHERE key IS NOT NULL) "
        "ORDER BY key"};
    for (std::size_t index = 0; index < queries.size(); ++index) {
        bioseq::postgres_test::migration_probe_scope permutation{
            "joins and upper relations replay exact ordered results",
            "query shape", std::to_string(index)};
        const auto cold = connection.exec(queries[index], 30s);
        const auto warm = connection.exec(queries[index], 30s);
        CHECK(bioseq::postgres_test::exact_row_hash(cold)
              == bioseq::postgres_test::exact_row_hash(warm));
        bioseq::postgres_test::record_migration_probe(
            "regress.cache_joins_upper.row_identity");
    }
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_joins_upper.join");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_joins_upper.aggregate");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_joins_upper.distinct");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_joins_upper.window");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_joins_upper.set_operations");
}

TEST_CASE("volatile and write-capable plans fail open",
          "[cache][integration][eligibility][volatile][explain]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "volatile and write-capable plans fail open"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();
    bioseq::cache_test::exec(
        connection,
        "SET bioseq.query_cache_optimisation_mode = 'same'; "
        "CREATE TABLE cache_eligibility_rows(id integer PRIMARY KEY); "
        "INSERT INTO cache_eligibility_rows VALUES (1), (2), (3); "
        "SELECT bioseq_cache.reset_stats()");

    const auto first = connection.exec(
        "SELECT id, random() IS NOT NULL "
        "FROM cache_eligibility_rows ORDER BY id");
    const auto second = connection.exec(
        "SELECT id, random() IS NOT NULL "
        "FROM cache_eligibility_rows ORDER BY id");
    REQUIRE(first.rows() == 3);
    REQUIRE(second.rows() == 3);
    const auto volatile_plan = bioseq::postgres_test::run_explain(
        connection,
        "SELECT id, random() IS NOT NULL "
        "FROM cache_eligibility_rows ORDER BY id",
        false, 30s);
    CHECK(volatile_plan
              .find_plan_nodes("Custom Plan Provider", "BioseqCacheScan")
              .empty());
    CHECK(bioseq::cache_test::read_cache_stats(connection).produced == 0);
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_eligibility_security_explain.volatile_bypass");

    const auto write_result = connection.exec(
        "WITH changed AS ("
        "UPDATE cache_eligibility_rows SET id = id "
        "RETURNING id) "
        "SELECT count(*) FROM changed");
    CHECK(write_result.value(0, 0) == "3");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_eligibility_security_explain.write_cte_bypass");
}

TEST_CASE("RLS and role namespaces cannot cross cache entries",
          "[cache][integration][security][rls][role]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "RLS and role namespaces cannot cross cache entries"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();
    bioseq::cache_test::exec(
        connection,
        R"SQL(
            SET bioseq.query_cache_optimisation_mode = 'same';
            CREATE ROLE cache_rls_alice;
            CREATE ROLE cache_rls_bob;
            CREATE TABLE cache_rls_rows(owner_name name, secret text);
            INSERT INTO cache_rls_rows VALUES
                ('cache_rls_alice', 'alice-secret'),
                ('cache_rls_bob', 'bob-secret');
            ALTER TABLE cache_rls_rows ENABLE ROW LEVEL SECURITY;
            CREATE POLICY cache_rls_policy ON cache_rls_rows
                USING (owner_name = current_user);
            GRANT SELECT ON cache_rls_rows
                TO cache_rls_alice, cache_rls_bob;
        )SQL");

    bioseq::cache_test::exec(connection, "SET ROLE cache_rls_alice");
    const auto alice_cold = connection.exec(
        "SELECT secret FROM cache_rls_rows ORDER BY secret");
    const auto alice_warm = connection.exec(
        "SELECT secret FROM cache_rls_rows ORDER BY secret");
    REQUIRE(alice_cold.rows() == 1);
    CHECK(alice_cold.value(0, 0) == "alice-secret");
    CHECK(bioseq::postgres_test::exact_row_hash(alice_cold)
          == bioseq::postgres_test::exact_row_hash(alice_warm));
    bioseq::cache_test::exec(connection, "RESET ROLE");

    bioseq::cache_test::exec(connection, "SET ROLE cache_rls_bob");
    const auto bob = connection.exec(
        "SELECT secret FROM cache_rls_rows ORDER BY secret");
    REQUIRE(bob.rows() == 1);
    CHECK(bob.value(0, 0) == "bob-secret");
    CHECK(bioseq::postgres_test::exact_row_hash(bob)
          != bioseq::postgres_test::exact_row_hash(alice_cold));
    bioseq::cache_test::exec(connection, "RESET ROLE");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_eligibility_security_explain.rls_isolation");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_reuse_security.role_namespace");
}

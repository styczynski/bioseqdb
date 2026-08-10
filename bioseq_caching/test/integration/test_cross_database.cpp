#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

namespace {

[[nodiscard]] bioseq::libpq::connection
connect_as(const bioseq::postgres_test::private_cluster &cluster,
           std::string_view database, std::string_view user,
           std::string_view transcript_name) {
  bioseq::libpq::connection connection(
      bioseq::libpq::unix_socket_conninfo(cluster.socket_directory().string(),
                                          database, user, cluster.port()),
      10s);
  bioseq::postgres_test::attach_process_sql_transcript(connection,
                                                       transcript_name);
  return connection;
}

void create_namespaced_source(bioseq::libpq::connection &connection) {
  bioseq::cache_test::exec(
      connection, "CREATE EXTENSION bioseq_cache; "
                  "CREATE TABLE same_oid_shape("
                  "id integer PRIMARY KEY, value text); "
                  "INSERT INTO same_oid_shape VALUES (1, current_database()); "
                  "SET bioseq.query_cache_optimisation_mode = 'same'; "
                  "SELECT * FROM same_oid_shape ORDER BY id; "
                  "SELECT * FROM same_oid_shape ORDER BY id");
}

void require_database_value(bioseq::libpq::connection &connection,
                            std::string_view expected) {
  const auto result = connection.exec("SELECT value FROM same_oid_shape");
  REQUIRE(result.rows() == 1);
  REQUIRE(result.columns() == 1);
  CHECK(result.value(0, 0) == expected);
}

} // namespace

TEST_CASE("database and role identities partition exact and coverage caches",
          "[cache][integration][security][namespace]") {
  bioseq::postgres_test::migration_probe_scope probes{
      "database and role identities partition exact and coverage caches"};
  bioseq::postgres_test::postgres_fixture fixture{
      bioseq::cache_test::cluster_options(
          false, bioseq::cache_test::preload_order::cache_only)};
  auto &admin = fixture.admin();
  bioseq::cache_test::exec(admin, "CREATE DATABASE cache_db_a", 30s);
  bioseq::cache_test::exec(admin, "CREATE DATABASE cache_db_b", 30s);
  bioseq::cache_test::exec(admin,
                           "CREATE ROLE cache_namespace_b SUPERUSER LOGIN");

  auto database_a =
      connect_as(fixture.cluster(), "cache_db_a", "postgres", "namespace-db-a");
  create_namespaced_source(database_a);
  const std::string primary_role_oid = bioseq::cache_test::scalar(
      database_a, "SELECT current_user::regrole::oid::text");
  auto role_b = connect_as(fixture.cluster(), "cache_db_a", "cache_namespace_b",
                           "namespace-role-b");

  constexpr std::string_view domain{"decode('444f4d41494e2d41', 'hex')"};
  constexpr std::string_view provider_digest{"decode(repeat('11', 32), 'hex')"};
  constexpr std::string_view forced_digest{"decode(repeat('aa', 32), 'hex')"};
  std::string generation_a;
  {
    auto database_b = connect_as(fixture.cluster(), "cache_db_b", "postgres",
                                 "namespace-db-b");
    create_namespaced_source(database_b);
    require_database_value(database_a, "cache_db_a");
    require_database_value(database_b, "cache_db_b");

    const auto before = bioseq::cache_test::read_cache_stats(database_a);
    bioseq::cache_test::exec(
        role_b, "SET bioseq.query_cache_optimisation_mode = 'same'");
    const auto role_cold =
        role_b.exec("SELECT * FROM same_oid_shape ORDER BY id");
    const auto role_warm =
        role_b.exec("SELECT * FROM same_oid_shape ORDER BY id");
    REQUIRE(role_cold.rows() == 1);
    REQUIRE(role_warm.rows() == 1);
    CHECK(role_cold.value(0, 0) == "1");
    CHECK(role_cold.value(0, 1) == "cache_db_a");
    CHECK(bioseq::postgres_test::exact_row_hash(role_cold) ==
          bioseq::postgres_test::exact_row_hash(role_warm));
    const auto role_delta =
        bioseq::cache_test::read_cache_stats(database_a) - before;
    CHECK(role_delta.produced == 1);
    CHECK(role_delta.completed_hits == 1);

    generation_a = bioseq::cache_test::scalar(
        database_a, "SELECT bioseq_cache_test.inject_complete_coverage(" +
                        std::string(domain) + ", decode('00', 'hex'), " +
                        std::string(provider_digest) + ", " +
                        std::string(forced_digest) + ")");
    const auto crowded_database = database_b.exec(
        "SELECT bioseq_cache_test.inject_complete_coverage(" +
        std::string(domain) + ", decode(lpad(to_hex(i), 4, '0'), 'hex'), " +
        std::string(provider_digest) + ", " + std::string(forced_digest) +
        ") FROM generate_series(1, 65) AS g(i)");
    REQUIRE(crowded_database.rows() == 65);
  }

  CHECK(bioseq::cache_test::integer(
            database_a, "SELECT count(*) "
                        "FROM bioseq_cache_test.lookup_injected_coverage(" +
                            std::string(domain) +
                            ") "
                            "WHERE generation = " +
                            generation_a) == 1);
  CHECK(bioseq::cache_test::boolean(
      database_a, "SELECT bool_and(database_oid = ("
                  "SELECT oid FROM pg_database "
                  "WHERE datname = current_database())) "
                  "FROM bioseq_cache_test.coverage_bucket_states()"));

  const auto role_generation_result = role_b.exec(
      "SELECT bioseq_cache_test.inject_complete_coverage(" +
      std::string(domain) + ", decode(lpad(to_hex(i), 4, '0'), 'hex'), " +
      std::string(provider_digest) + ", " + std::string(forced_digest) +
      ") FROM generate_series(101, 165) AS g(i)");
  REQUIRE(role_generation_result.rows() == 65);
  REQUIRE(role_generation_result.columns() == 1);
  std::vector<std::string> role_generations;
  role_generations.reserve(65);
  for (int row = 0; row < role_generation_result.rows(); ++row) {
    REQUIRE_FALSE(role_generation_result.is_null(row, 0));
    role_generations.emplace_back(role_generation_result.value(row, 0));
  }
  CHECK(bioseq::cache_test::integer(
            database_a, "SELECT count(*) "
                        "FROM bioseq_cache_test.coverage_bucket_states() "
                        "WHERE current_role_oid = "
                        "'cache_namespace_b'::regrole") == 65);
  CHECK(bioseq::cache_test::integer(
            database_a, "SELECT count(*) "
                        "FROM bioseq_cache_test.coverage_bucket_states() "
                        "WHERE current_role_oid = " +
                            primary_role_oid) == 1);

  bioseq::cache_test::exec(
      database_a, "SELECT bioseq_cache_test.mark_generation_evictable(" +
                      role_generations[0] +
                      "); "
                      "SELECT bioseq_cache_test.request_janitor_sweep()");
  CHECK(bioseq::cache_test::boolean(
      database_a,
      "SELECT bioseq_cache_test.wait_for_generation_absent(" +
          role_generations[0] + ", interval '10 seconds')",
      15s));
  CHECK(bioseq::cache_test::integer(
            database_a, "SELECT count(*) "
                        "FROM bioseq_cache_test.lookup_injected_coverage(" +
                            std::string(domain) +
                            ") "
                            "WHERE generation = " +
                            generation_a) == 1);
  CHECK(bioseq::cache_test::integer(
            database_a, "SELECT count(*) "
                        "FROM bioseq_cache_test.coverage_bucket_states() "
                        "WHERE current_role_oid = "
                        "'cache_namespace_b'::regrole") == 64);

  bioseq::cache_test::exec(admin, "DROP DATABASE cache_db_b", 30s);
  CHECK(bioseq::cache_test::integer(
            database_a, "SELECT count(*) "
                        "FROM bioseq_cache_test.lookup_injected_coverage(" +
                            std::string(domain) +
                            ") "
                            "WHERE generation = " +
                            generation_a) == 1);
  CHECK(bioseq::cache_test::integer(
            role_b, "SELECT count(*) "
                    "FROM bioseq_cache_test.lookup_injected_coverage(" +
                        std::string(domain) +
                        ") "
                        "WHERE generation = " +
                        role_generations[1]) == 1);

  bioseq::cache_test::exec(
      role_b, "SELECT bioseq_cache_test.mark_generation_evictable(" +
                  generation_a +
                  "); "
                  "SELECT bioseq_cache_test.request_janitor_sweep()");
  CHECK(bioseq::cache_test::boolean(
      database_a,
      "SELECT bioseq_cache_test.wait_for_generation_absent(" + generation_a +
          ", interval '10 seconds')",
      15s));
  CHECK(bioseq::cache_test::integer(
            role_b, "SELECT count(*) "
                    "FROM bioseq_cache_test.lookup_injected_coverage(" +
                        std::string(domain) +
                        ") "
                        "WHERE generation = " +
                        role_generations[1]) == 1);
  bioseq::postgres_test::record_migration_probe(
      "tap.005.cross_database.full_parity");
}

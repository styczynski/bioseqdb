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

[[nodiscard]] bioseq::libpq::client_connection
connect_as(const bioseq::postgres_test::private_cluster &cluster,
           std::string_view database, std::string_view user,
           std::string_view transcript_name) {
  bioseq::libpq::client_connection connection(
      bioseq::libpq::make_unix_socket_conninfo(cluster.socket_directory().string(),
                                          database, user, cluster.port()),
      10s);
  bioseq::postgres_test::attach_process_sql_transcript(connection,
                                                       transcript_name);
  return connection;
}

void create_namespaced_source(bioseq::libpq::client_connection &connection) {
  bioseq::cache_test::execute(
      connection, "CREATE EXTENSION bioseq_cache; "
                  "CREATE TABLE same_oid_shape("
                  "id integer PRIMARY KEY, value text); "
                  "INSERT INTO same_oid_shape VALUES (1, current_database()); "
                  "SET bioseq_cache.query_cache.mode = 'exact_query'; "
                  "SELECT * FROM same_oid_shape ORDER BY id; "
                  "SELECT * FROM same_oid_shape ORDER BY id");
}

void require_database_value(bioseq::libpq::client_connection &connection,
                            std::string_view expected) {
  const auto result = connection.execute("SELECT value FROM same_oid_shape");
  REQUIRE(result.row_count() == 1);
  REQUIRE(result.column_count() == 1);
  CHECK(result.text_value(0, 0) == expected);
}

} // namespace

TEST_CASE("database and role identities partition exact and coverage caches",
          "[component:cache][layer:integration]"
          "[feature:security][resource:postgres]") {
  bioseq::postgres_test::private_cluster_fixture fixture{
      bioseq::cache_test::cluster_options(
          false, bioseq::cache_test::preload_order::cache_only)};
  auto &admin = fixture.admin();
  bioseq::cache_test::execute(admin, "CREATE DATABASE cache_db_a", 30s);
  bioseq::cache_test::execute(admin, "CREATE DATABASE cache_db_b", 30s);
  bioseq::cache_test::execute(admin,
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
    bioseq::cache_test::execute(
        role_b, "SET bioseq_cache.query_cache.mode = 'exact_query'");
    const auto role_cold =
        role_b.execute("SELECT * FROM same_oid_shape ORDER BY id");
    const auto role_warm =
        role_b.execute("SELECT * FROM same_oid_shape ORDER BY id");
    REQUIRE(role_cold.row_count() == 1);
    REQUIRE(role_warm.row_count() == 1);
    CHECK(role_cold.text_value(0, 0) == "1");
    CHECK(role_cold.text_value(0, 1) == "cache_db_a");
    CHECK(bioseq::postgres_test::hash_ordered_result_rows(role_cold) ==
          bioseq::postgres_test::hash_ordered_result_rows(role_warm));
    const auto role_delta =
        bioseq::cache_test::read_cache_stats(database_a) - before;
    CHECK(role_delta.produced == 1);
    CHECK(role_delta.completed_hits == 1);

    generation_a = bioseq::cache_test::scalar(
        database_a, "SELECT bioseq_cache_test.inject_complete_coverage(" +
                        std::string(domain) + ", decode('00', 'hex'), " +
                        std::string(provider_digest) + ", " +
                        std::string(forced_digest) + ")");
    const auto crowded_database = database_b.execute(
        "SELECT bioseq_cache_test.inject_complete_coverage(" +
        std::string(domain) + ", decode(lpad(to_hex(i), 4, '0'), 'hex'), " +
        std::string(provider_digest) + ", " + std::string(forced_digest) +
        ") FROM generate_series(1, 65) AS g(i)");
    REQUIRE(crowded_database.row_count() == 65);
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

  const auto role_generation_result = role_b.execute(
      "SELECT bioseq_cache_test.inject_complete_coverage(" +
      std::string(domain) + ", decode(lpad(to_hex(i), 4, '0'), 'hex'), " +
      std::string(provider_digest) + ", " + std::string(forced_digest) +
      ") FROM generate_series(101, 165) AS g(i)");
  REQUIRE(role_generation_result.row_count() == 65);
  REQUIRE(role_generation_result.column_count() == 1);
  std::vector<std::string> role_generations;
  role_generations.reserve(65);
  for (int row = 0; row < role_generation_result.row_count(); ++row) {
    REQUIRE_FALSE(role_generation_result.cell_is_null(row, 0));
    role_generations.emplace_back(role_generation_result.text_value(row, 0));
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

  bioseq::cache_test::execute(
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

  bioseq::cache_test::execute(admin, "DROP DATABASE cache_db_b", 30s);
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

  bioseq::cache_test::execute(
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
}

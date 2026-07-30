#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

namespace {

[[nodiscard]] bioseq::postgres_test::cluster_options
bootstrap_options(std::optional<std::string> fault = std::nullopt) {
  auto options = bioseq::cache_test::cluster_options(
      false, bioseq::cache_test::preload_order::cache_only, {}, false,
      bioseq::cache_test::bootstrap_readiness::caller_managed);
  if (options.post_extension_readiness.has_value()) {
    throw std::logic_error(
        "bootstrap recovery must own lazy initialization");
  }
  if (fault.has_value()) {
    options.process_environment.emplace_back(
        "BIOSEQ_CACHE_TEST_BOOTSTRAP_FAULT", std::move(*fault));
  }
  return options;
}

[[nodiscard]] std::string status_sql(std::string_view predicate) {
  return "WITH q AS ("
         "SELECT bioseq_cache_test.bootstrap_status() AS s) "
         "SELECT (" +
         std::string(predicate) + ") FROM q";
}

[[nodiscard]] std::string_view attach_twice_sql() {
  return "WITH q AS ("
         "SELECT bioseq_cache_test.bootstrap_attach_twice() AS s) "
         "SELECT (s->>'first_attached')::boolean "
         "AND (s->>'second_attached')::boolean "
         "AND (s->>'same_area')::boolean "
         "AND (s->>'same_root')::boolean "
         "AND (s->>'mapping_pinned')::boolean "
         "AND (s->>'attempts_before')::bigint = "
         "(s->>'attempts_after')::bigint "
         "AND (s->>'attaches_after')::bigint - "
         "(s->>'attaches_before')::bigint = 1 "
         "FROM q";
}

template <typename Session>
[[nodiscard]] bool session_boolean(Session &session, std::string_view sql) {
  const auto result = session.exec(sql);
  if (result.rows() != 1 || result.columns() != 1 || result.is_null(0, 0)) {
    throw std::runtime_error("expected one non-null Boolean result");
  }
  if (result.value(0, 0) == "t") {
    return true;
  }
  if (result.value(0, 0) == "f") {
    return false;
  }
  throw std::runtime_error("expected PostgreSQL Boolean text");
}

template <typename Session>
[[nodiscard]] std::string session_scalar(Session &session,
                                         std::string_view sql) {
  const auto result = session.exec(sql);
  if (result.rows() != 1 || result.columns() != 1 || result.is_null(0, 0)) {
    throw std::runtime_error("expected one non-null scalar result");
  }
  return std::string(result.value(0, 0));
}

template <typename Session>
[[nodiscard]] bool
wait_for_status(Session &session, std::string_view predicate,
                std::chrono::steady_clock::time_point deadline) {
  const std::string query = status_sql(predicate);
  return bioseq::postgres_test::wait_until(
      [&session, &query]() { return session_boolean(session, query); },
      deadline, 20ms);
}

void create_source_table(bioseq::libpq::connection &connection,
                         std::string_view table) {
  const std::string identifier = connection.escape_identifier(table);
  bioseq::cache_test::exec(connection,
                           "CREATE TABLE " + identifier +
                               "(id integer PRIMARY KEY, value text); "
                               "INSERT INTO " +
                               identifier +
                               " VALUES "
                               "(10, 'ten'), (20, 'twenty'), (30, 'thirty')");
}

[[nodiscard]] std::string source_result(bioseq::libpq::connection &connection,
                                        std::string_view table) {
  const std::string identifier = connection.escape_identifier(table);
  const auto result =
      connection.exec("SET bioseq.query_cache_optimisation_mode = 'same'; "
                      "SELECT count(*), sum(id) FROM " +
                      identifier);
  if (result.rows() != 1 || result.columns() != 2 || result.is_null(0, 0) ||
      result.is_null(0, 1)) {
    throw std::runtime_error("bootstrap source result has wrong shape");
  }
  return std::string(result.value(0, 0)) + "|" +
         std::string(result.value(0, 1));
}

[[nodiscard]] bool all_passed_except_first(
    const std::vector<bioseq::postgres_test::session_task_outcome> &outcomes) {
  return outcomes.size() == 2 && !outcomes[0].passed &&
         !outcomes[0].timed_out && outcomes[1].passed && !outcomes[1].timed_out;
}

} // namespace

TEST_CASE("bootstrap attachment takeover and corruption fail open coherently",
          "[cache][integration][bootstrap][recovery][fault]") {
  bioseq::postgres_test::migration_probe_scope probes{
      "bootstrap attachment takeover and corruption fail open coherently"};

  {
    bioseq::postgres_test::postgres_fixture fixture{bootstrap_options()};
    auto &admin = fixture.admin();
    REQUIRE(wait_for_status(admin, "(s->>'state')::integer = 2",
                            std::chrono::steady_clock::now() + 10s));
    auto connection = fixture.connect();
    CHECK(bioseq::cache_test::boolean(connection, attach_twice_sql()));
    CHECK(bioseq::cache_test::boolean(
        connection, "WITH a AS MATERIALIZED ("
                    "SELECT bioseq_cache_test.bootstrap_attach_twice() "
                    "AS attached), "
                    "q AS ("
                    "SELECT bioseq_cache_test.bootstrap_status() AS s FROM a) "
                    "SELECT (s->>'state')::integer = 2 "
                    "AND (s->>'boot_nonce')::bigint <> 0 "
                    "AND (s->>'ready_handle_published')::boolean "
                    "AND (s->>'root_published')::boolean "
                    "AND (s->>'storage_attached')::boolean "
                    "AND (s->>'mapping_pinned')::boolean "
                    "AND (s->>'storage_metadata_in_dsa')::boolean "
                    "AND (s->>'reusable_wait_cvs_in_dsa')::boolean "
                    "AND (s->>'reusable_wait_cv_count')::integer > 0 "
                    "FROM q"));
  }

  {
    bioseq::postgres_test::postgres_fixture fixture{
        bootstrap_options("pause_backend_once")};
    std::string dead_token;
    std::string new_token;
    std::string cleanup_handle;
    bool owner_token_visible{};
    bool cleanup_visible{};
    bool takeover_valid{};
    bool disabled_writer_committed{};
    bool disabled_writer_preserved_owner{};
    std::vector<bioseq::postgres_test::session_task> tasks;
    tasks.push_back(bioseq::postgres_test::session_task{
        .name = "bootstrap-paused-initializer",
        .body = [](bioseq::postgres_test::session_context &session) {
          static_cast<void>(session.exec(attach_twice_sql()));
        }});
    tasks.push_back(bioseq::postgres_test::session_task{
        .name = "bootstrap-takeover-controller",
        .body = [&dead_token, &new_token, &cleanup_handle, &owner_token_visible,
                 &cleanup_visible, &takeover_valid,
                 &disabled_writer_committed,
                 &disabled_writer_preserved_owner](
                    bioseq::postgres_test::session_context &session) {
          owner_token_visible = wait_for_status(
              session,
              "(s->>'state')::integer = 1 "
              "AND (s->>'pause_reached')::boolean "
              "AND (s->>'defer_background_initializer')::boolean "
              "AND (s->>'initializer_pid')::integer > 0 "
              "AND (s->>'initializer_nonce')::bigint > 0 "
              "AND (s->>'initializer_started_at')::bigint <> 0 "
              "AND (s->>'initializer_claim')::bigint > 0 "
              "AND (s->>'cleanup_handle_published')::boolean "
              "AND (s->>'cleanup_handle_pinned')::integer = 1 "
              "AND (s->>'live_unpublished_areas')::integer = 1 "
              "AND NOT "
              "(s->>'ready_handle_published')::boolean "
              "AND NOT (s->>'root_published')::boolean",
              session.deadline());
          if (!owner_token_visible) {
            throw std::runtime_error(
                "paused initializer owner token was not visible");
          }
          dead_token = session_scalar(
              session, "WITH q AS ("
                       "SELECT bioseq_cache_test.bootstrap_status() AS s) "
                       "SELECT (s->>'initializer_proc') || '|' || "
                       "(s->>'initializer_pid') || '|' || "
                       "(s->>'initializer_nonce') || '|' || "
                       "(s->>'initializer_started_at') || '|' || "
                       "(s->>'initializer_claim') FROM q");
          static_cast<void>(session.exec(
              "SET bioseq.query_cache_optimisation_mode = 'disabled'; "
              "CREATE TABLE bootstrap_pre_ready_writer_rows("
              "id integer PRIMARY KEY, value text); "
              "INSERT INTO bootstrap_pre_ready_writer_rows VALUES "
              "(10, 'ten'), (20, 'twenty'), (30, 'thirty')"));
          disabled_writer_committed = true;
          const std::string token_after_write = session_scalar(
              session, "WITH q AS ("
                       "SELECT bioseq_cache_test.bootstrap_status() AS s) "
                       "SELECT (s->>'initializer_proc') || '|' || "
                       "(s->>'initializer_pid') || '|' || "
                       "(s->>'initializer_nonce') || '|' || "
                       "(s->>'initializer_started_at') || '|' || "
                       "(s->>'initializer_claim') FROM q");
          disabled_writer_preserved_owner =
              token_after_write == dead_token
              && session_boolean(
                  session,
                  status_sql(
                      "(s->>'state')::integer = 1 "
                      "AND (s->>'pause_reached')::boolean "
                      "AND "
                      "(s->>'initialization_attempts')::bigint = 1 "
                      "AND NOT "
                      "(s->>'ready_handle_published')::boolean "
                      "AND NOT (s->>'root_published')::boolean"));
          const std::string initializer_pid =
              session_scalar(session, "SELECT ("
                                      "bioseq_cache_test.bootstrap_status()->>"
                                      "'initializer_pid')");
          if (!session_boolean(session, "SELECT pg_terminate_backend(" +
                                            initializer_pid + ")")) {
            throw std::runtime_error(
                "paused initializer termination was rejected");
          }
          cleanup_visible = wait_for_status(
              session,
              "(s->>'state')::integer = 1 "
              "AND (s->>'cleanup_handle_pinned')::integer = 0 "
              "AND (s->>'last_cleanup_handle')::bigint > 0 "
              "AND (s->>'last_cleanup_was_pinned')::boolean "
              "AND (((s->>'live_unpublished_areas')::integer = 0 "
              "AND NOT "
              "(s->>'cleanup_handle_published')::boolean "
              "AND "
              "(s->>'last_cleanup_release_verified')::boolean) "
              "OR "
              "((s->>'live_unpublished_areas')::integer = 1 "
              "AND "
              "(s->>'cleanup_handle_published')::boolean "
              "AND NOT "
              "(s->>'last_cleanup_release_verified')::boolean))",
              session.deadline());
          if (!cleanup_visible) {
            throw std::runtime_error(
                "dead initializer cleanup was not visible");
          }
          cleanup_handle =
              session_scalar(session, "SELECT ("
                                      "bioseq_cache_test.bootstrap_status()->>"
                                      "'last_cleanup_handle')");
          takeover_valid = session_boolean(
              session, "WITH a AS MATERIALIZED ("
                       "SELECT "
                       "bioseq_cache_test.bootstrap_attach_twice() AS a), "
                       "q AS ("
                       "SELECT a.a, "
                       "bioseq_cache_test.bootstrap_status() AS s FROM a) "
                       "SELECT (a->>'first_attached')::boolean "
                       "AND (a->>'second_attached')::boolean "
                       "AND (a->>'same_area')::boolean "
                       "AND (a->>'same_root')::boolean "
                       "AND (a->>'mapping_pinned')::boolean "
                       "AND (a->>'attempts_before')::bigint = 1 "
                       "AND (a->>'attempts_after')::bigint = 2 "
                       "AND (a->>'attaches_after')::bigint - "
                       "(a->>'attaches_before')::bigint = 0 "
                       "AND (s->>'state')::integer = 2 "
                       "AND (s->>'takeovers')::bigint = 1 "
                       "AND (s->>'takeover_used')::integer = 1 "
                       "AND "
                       "(s->>'initialization_attempts')::bigint = 2 "
                       "AND "
                       "(s->>'live_unpublished_areas')::integer = 0 "
                       "AND "
                       "(s->>'cleanup_handle_pinned')::integer = 0 "
                       "AND NOT "
                       "(s->>'cleanup_handle_published')::boolean "
                       "AND (s->>'last_cleanup_handle')::bigint = " +
                           cleanup_handle +
                           " "
                           "AND "
                           "(s->>'last_cleanup_release_verified')::boolean "
                           "AND (s->>'ready_handle_published')::boolean "
                           "AND (s->>'root_published')::boolean "
                           "FROM q");
          new_token = session_scalar(
              session, "WITH q AS ("
                       "SELECT bioseq_cache_test.bootstrap_status() AS s) "
                       "SELECT (s->>'last_initializer_proc') || '|' || "
                       "(s->>'last_initializer_pid') || '|' || "
                       "(s->>'last_initializer_nonce') || '|' || "
                       "(s->>'last_initializer_started_at') || '|' || "
                       "(s->>'last_initializer_claim') FROM q");
        }});
    const auto outcomes = bioseq::postgres_test::run_independent_session_tasks(
        fixture.cluster(), tasks, 45s);
    REQUIRE(all_passed_except_first(outcomes));
    CHECK(owner_token_visible);
    CHECK(cleanup_visible);
    CHECK(takeover_valid);
    CHECK(disabled_writer_committed);
    CHECK(disabled_writer_preserved_owner);
    CHECK_FALSE(dead_token.empty());
    CHECK_FALSE(new_token.empty());
    CHECK(new_token != dead_token);

    auto &connection = fixture.admin();
    bioseq::cache_test::exec(connection, "SELECT bioseq_cache.reset_stats()");
    CHECK(bioseq::cache_test::boolean(
        connection,
        "SELECT bioseq_cache_test.cache_infrastructure_ready()"));
    CHECK(source_result(
              connection, "bootstrap_pre_ready_writer_rows")
          == "3|60");
    CHECK(source_result(
              connection, "bootstrap_pre_ready_writer_rows")
          == "3|60");
    const auto stats = bioseq::cache_test::read_cache_stats(connection);
    CHECK(stats.produced == 1);
    CHECK(stats.completed_hits == 1);
  }

  for (int ordinal = 1; ordinal <= 5; ++ordinal) {
    INFO("bootstrap allocation fault ordinal " << ordinal);
    bioseq::postgres_test::postgres_fixture fixture{
        bootstrap_options("alloc_" + std::to_string(ordinal))};
    auto &connection = fixture.admin();
    const std::string table =
        "bootstrap_alloc_" + std::to_string(ordinal) + "_rows";
    create_source_table(connection, table);
    CHECK(source_result(connection, table) == "3|60");
    REQUIRE(wait_for_status(connection, "(s->>'state')::integer = 3",
                            std::chrono::steady_clock::now() + 10s));
    CHECK(bioseq::cache_test::boolean(
        connection,
        status_sql("(s->>'failure')::integer = 34 "
                   "AND "
                   "(s->>'initialization_attempts')::bigint = 1 "
                   "AND (s->>'cleanups')::bigint = 1 "
                   "AND (s->>'validation_failures')::bigint = 1 "
                   "AND "
                   "(s->>'live_unpublished_areas')::integer = 0 "
                   "AND NOT "
                   "(s->>'cleanup_handle_published')::boolean "
                   "AND NOT "
                   "(s->>'ready_handle_published')::boolean "
                   "AND NOT (s->>'root_published')::boolean "
                   "AND (s->>'last_cleanup_handle')::bigint > 0 "
                   "AND "
                   "(s->>'last_cleanup_was_pinned')::boolean "
                   "AND "
                   "(s->>'last_cleanup_release_verified')::boolean")));
  }

  for (const auto &[fault, expected_failure] :
       std::vector<std::pair<std::string, int>>{{"root_magic", 36},
                                                {"root_abi", 35}}) {
    INFO("bootstrap root fault " << fault);
    bioseq::postgres_test::postgres_fixture fixture{bootstrap_options(fault)};
    auto &connection = fixture.admin();
    const std::string table = "bootstrap_" + fault + "_rows";
    create_source_table(connection, table);
    CHECK(source_result(connection, table) == "3|60");
    REQUIRE(wait_for_status(connection, "(s->>'state')::integer = 3",
                            std::chrono::steady_clock::now() + 10s));
    CHECK(bioseq::cache_test::boolean(
        connection,
        status_sql(
            "(s->>'failure')::integer = " + std::to_string(expected_failure) +
            " AND "
            "(s->>'initialization_attempts')::bigint = 1 "
            "AND (s->>'cleanups')::bigint = 1 "
            "AND (s->>'validation_failures')::bigint = 1 "
            "AND "
            "(s->>'live_unpublished_areas')::integer = 0 "
            "AND NOT "
            "(s->>'cleanup_handle_published')::boolean "
            "AND NOT "
            "(s->>'ready_handle_published')::boolean "
            "AND NOT (s->>'root_published')::boolean "
            "AND (s->>'last_cleanup_handle')::bigint > 0 "
            "AND "
            "(s->>'last_cleanup_was_pinned')::boolean "
            "AND "
            "(s->>'last_cleanup_release_verified')::boolean")));
  }

  for (const std::string_view field :
       {std::string_view{"magic"}, std::string_view{"abi"},
        std::string_view{"boot_nonce"}}) {
    INFO("published bootstrap corruption field " << field);
    bioseq::postgres_test::postgres_fixture fixture{bootstrap_options()};
    auto connection = fixture.connect();
    REQUIRE(wait_for_status(connection, "(s->>'state')::integer = 2",
                            std::chrono::steady_clock::now() + 10s));
    CHECK(bioseq::cache_test::boolean(
        connection, "SELECT "
                    "bioseq_cache_test.corrupt_bootstrap_manifest(" +
                        connection.escape_literal(field) + ")"));
    CHECK(bioseq::cache_test::boolean(
        connection, status_sql("(s->>'state')::integer = 3 "
                               "AND (s->>'failure')::integer = 26 "
                               "AND (s->>'validation_failures')::bigint = 1 "
                               "AND (s->>'ready_handle_published')::boolean "
                               "AND (s->>'root_published')::boolean "
                               "AND NOT (s->>'storage_attached')::boolean")));
    const std::string table =
        "bootstrap_corrupt_" + std::string(field) + "_rows";
    create_source_table(connection, table);
    CHECK(source_result(connection, table) == "3|60");
  }

  bioseq::postgres_test::record_migration_probe(
      "tap.012.bootstrap_recovery.full_parity");
}

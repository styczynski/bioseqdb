#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

namespace {

[[nodiscard]] bool all_passed(
    const std::vector<bioseq::postgres_test::session_task_outcome> &outcomes) {
  for (const auto &outcome : outcomes) {
    if (!outcome.passed || outcome.timed_out) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::vector<std::string> ordered_ids(int count) {
  std::vector<std::string> values;
  values.reserve(static_cast<std::size_t>(count));
  for (int value = 1; value <= count; ++value) {
    values.push_back(std::to_string(value));
  }
  return values;
}

[[nodiscard]] std::vector<std::string>
first_column(const bioseq::libpq::result &result) {
  if (result.columns() != 1) {
    throw std::runtime_error("expected one result column");
  }
  std::vector<std::string> values;
  values.reserve(static_cast<std::size_t>(result.rows()));
  for (int row = 0; row < result.rows(); ++row) {
    if (result.is_null(row, 0)) {
      throw std::runtime_error("ordered identifier result contains NULL");
    }
    values.emplace_back(result.value(row, 0));
  }
  return values;
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

struct storage_snapshot {
  std::int64_t hot_bytes{};
  std::int64_t file_bytes{};
  std::int64_t manifests{};
  std::int64_t waiters{};

  friend bool operator==(const storage_snapshot &,
                         const storage_snapshot &) = default;
};

[[nodiscard]] std::int64_t diagnostic_metric(std::string_view diagnostic,
                                             std::string_view label) {
  const std::size_t label_offset = diagnostic.find(label);
  if (label_offset == std::string_view::npos) {
    throw std::runtime_error("Cache Storage diagnostic is missing " +
                             std::string(label));
  }
  const std::size_t begin = label_offset + label.size();
  std::size_t end = begin;
  while (end < diagnostic.size() && diagnostic[end] >= '0' &&
         diagnostic[end] <= '9') {
    ++end;
  }
  std::int64_t value{};
  const auto [parsed_end, error] = std::from_chars(
      diagnostic.data() + begin, diagnostic.data() + end, value);
  if (error != std::errc{} || parsed_end != diagnostic.data() + end ||
      begin == end) {
    throw std::runtime_error("Cache Storage diagnostic has invalid " +
                             std::string(label));
  }
  return value;
}

template <typename Session>
[[nodiscard]] storage_snapshot read_storage_snapshot(Session &session) {
  const auto explain =
      session.exec("SET bioseq.query_cache_optimisation_mode = 'same'; "
                   "EXPLAIN (COSTS OFF) "
                   "SELECT id FROM entitlement_rows "
                   "WHERE id <= 100 ORDER BY id");
  std::string diagnostic;
  for (int row = 0; row < explain.rows(); ++row) {
    if (explain.is_null(row, 0)) {
      continue;
    }
    const std::string_view line = explain.value(row, 0);
    constexpr std::string_view prefix{"Cache Storage: "};
    const std::size_t offset = line.find(prefix);
    if (offset != std::string_view::npos) {
      diagnostic = std::string(line.substr(offset + prefix.size()));
      break;
    }
  }
  if (diagnostic.empty()) {
    throw std::runtime_error(
        "EXPLAIN did not expose Cache Storage diagnostics");
  }
  return storage_snapshot{.hot_bytes = diagnostic_metric(diagnostic, "hot="),
                          .file_bytes = diagnostic_metric(diagnostic, "file="),
                          .manifests =
                              diagnostic_metric(diagnostic, "manifests="),
                          .waiters = diagnostic_metric(diagnostic, "waiters=")};
}

[[nodiscard]] bool snapshot_reclaimed(bioseq::libpq::connection &connection,
                                      const storage_snapshot &baseline) {
  bioseq::cache_test::exec(connection,
                           "SELECT bioseq_cache_test.request_janitor_sweep()");
  return read_storage_snapshot(connection) == baseline;
}

} // namespace

TEST_CASE("terminated cache producer wakes its waiter without BUILDING leak",
          "[cache][integration][concurrency][recovery]") {
  bioseq::postgres_test::migration_probe_scope probes{
      "terminated cache producer wakes its waiter without BUILDING leak"};
  bioseq::postgres_test::postgres_fixture fixture{
      bioseq::cache_test::cluster_options(
          false, bioseq::cache_test::preload_order::cache_only)};
  auto &admin = fixture.admin();
  bioseq::cache_test::exec(
      admin, "CREATE TABLE crash_rows("
             "id integer PRIMARY KEY, value text); "
             "INSERT INTO crash_rows "
             "SELECT i, md5(i::text) "
             "FROM generate_series(1, 100) AS g(i); "
             "SELECT bioseq_cache.reset_stats(); "
             "SELECT bioseq_cache_test.arm_pause('after_building_publish')");

  std::atomic<bool> producer_paused{};
  std::string waiter_count;
  std::string waiter_sum;
  std::vector<bioseq::postgres_test::session_task> tasks;
  tasks.push_back(bioseq::postgres_test::session_task{
      .name = "backend-crash-producer",
      .body = [](bioseq::postgres_test::session_context &session) {
        static_cast<void>(
            session.exec("SET application_name = 'cache_crash_producer'; "
                         "SET bioseq.query_cache_optimisation_mode = 'same'; "
                         "SELECT count(*), sum(id) "
                         "FROM crash_rows WHERE id <= 10"));
      }});
  tasks.push_back(bioseq::postgres_test::session_task{
      .name = "backend-crash-waiter",
      .body = [&producer_paused, &waiter_count,
               &waiter_sum](bioseq::postgres_test::session_context &session) {
        if (!bioseq::postgres_test::wait_until(
                [&producer_paused]() {
                  return producer_paused.load(std::memory_order_acquire);
                },
                session.deadline(), 5ms)) {
          throw std::runtime_error("producer did not reach its pause");
        }
        const auto result =
            session.exec("SET application_name = 'cache_crash_waiter'; "
                         "SET bioseq.query_cache_optimisation_mode = 'same'; "
                         "SELECT count(*), sum(id) "
                         "FROM crash_rows WHERE id <= 10");
        if (result.rows() != 1 || result.columns() != 2) {
          throw std::runtime_error("waiter result has the wrong shape");
        }
        waiter_count = std::string(result.value(0, 0));
        waiter_sum = std::string(result.value(0, 1));
      }});
  tasks.push_back(bioseq::postgres_test::session_task{
      .name = "backend-crash-controller",
      .body = [&producer_paused](
                  bioseq::postgres_test::session_context &session) {
        static_cast<void>(
            session.exec("SELECT bioseq_cache_test.wait_until_paused("
                         "'after_building_publish')"));
        producer_paused.store(true, std::memory_order_release);
        if (!bioseq::postgres_test::wait_until(
                [&session]() {
                  return session_boolean(
                      session, "SELECT EXISTS ("
                               "SELECT 1 "
                               "FROM bioseq_cache_test.entry_states() "
                               "WHERE state = 'BUILDING' "
                               "AND waiter_count > 0)");
                },
                session.deadline(), 10ms)) {
          throw std::runtime_error(
              "waiter did not register against the producer");
        }
        if (!session_boolean(session, "SELECT pg_terminate_backend(("
                                      "SELECT producer_pid "
                                      "FROM bioseq_cache_test.entry_states() "
                                      "WHERE state = 'BUILDING' "
                                      "ORDER BY generation LIMIT 1))")) {
          throw std::runtime_error("producer backend termination was rejected");
        }
        static_cast<void>(session.exec("SELECT bioseq_cache_test.release_pause("
                                       "'after_building_publish')"));
      }});

  const auto outcomes = bioseq::postgres_test::run_independent_session_tasks(
      fixture.cluster(), tasks, 45s);
  REQUIRE(outcomes.size() == 3);
  CHECK_FALSE(outcomes[0].passed);
  CHECK_FALSE(outcomes[0].timed_out);
  CHECK(outcomes[1].passed);
  CHECK_FALSE(outcomes[1].timed_out);
  CHECK(outcomes[2].passed);
  CHECK(waiter_count == "10");
  CHECK(waiter_sum == "55");
  CHECK(bioseq::cache_test::integer(admin,
                                    "SELECT count(*) "
                                    "FROM bioseq_cache_test.entry_states() "
                                    "WHERE state = 'BUILDING'") == 0);
  CHECK(bioseq::cache_test::integer(
            admin, "SELECT count(*) FROM crash_rows WHERE id <= 10") == 10);
  bioseq::postgres_test::record_migration_probe(
      "tap.002.backend_crash.full_parity");
}

TEST_CASE("file write failure fails open and terminalizes the exact claim",
          "[cache][integration][file-tier][fault][concurrency]") {
  bioseq::postgres_test::migration_probe_scope probes{
      "file write failure fails open and terminalizes the exact claim"};
  bioseq::postgres_test::postgres_fixture fixture{
      bioseq::cache_test::cluster_options(
          false, bioseq::cache_test::preload_order::cache_only)};
  auto &admin = fixture.admin();
  bioseq::cache_test::exec(
      admin, "CREATE TABLE file_rows("
             "id integer PRIMARY KEY, payload text); "
             "INSERT INTO file_rows "
             "SELECT i, repeat(md5(i::text), 1024) "
             "FROM generate_series(1, 128) AS g(i); "
             "SELECT bioseq_cache.reset_stats(); "
             "SELECT bioseq_cache_test.arm_pause('after_building_publish')");

  std::atomic<bool> producer_paused{};
  std::vector<std::string> producer_rows;
  std::vector<std::string> waiter_rows;
  std::string failed_key;
  std::string failed_generation;
  std::string failed_claim_sequence;
  std::vector<bioseq::postgres_test::session_task> tasks;
  tasks.push_back(bioseq::postgres_test::session_task{
      .name = "file-failure-producer",
      .body =
          [&producer_rows](bioseq::postgres_test::session_context &session) {
            const auto result = session.exec(
                "SET application_name = 'cache_file_failure_producer'; "
                "SET bioseq.query_cache_optimisation_mode = 'same'; "
                "SELECT id FROM file_rows "
                "WHERE id <= 10 ORDER BY id");
            producer_rows = first_column(result);
          }});
  tasks.push_back(bioseq::postgres_test::session_task{
      .name = "file-failure-waiter",
      .body = [&producer_paused,
               &waiter_rows](bioseq::postgres_test::session_context &session) {
        if (!bioseq::postgres_test::wait_until(
                [&producer_paused]() {
                  return producer_paused.load(std::memory_order_acquire);
                },
                session.deadline(), 5ms)) {
          throw std::runtime_error("producer did not reach its pause");
        }
        const auto result =
            session.exec("SET application_name = 'cache_file_failure_waiter'; "
                         "SET bioseq.query_cache_optimisation_mode = 'same'; "
                         "SELECT id FROM file_rows "
                         "WHERE id <= 10 ORDER BY id");
        waiter_rows = first_column(result);
      }});
  tasks.push_back(bioseq::postgres_test::session_task{
      .name = "file-failure-controller",
      .body = [&producer_paused, &failed_key, &failed_generation,
               &failed_claim_sequence](
                  bioseq::postgres_test::session_context &session) {
        static_cast<void>(
            session.exec("SELECT bioseq_cache_test.wait_until_paused("
                         "'after_building_publish')"));
        const auto token =
            session.exec("SELECT entry_key, generation, claim_sequence "
                         "FROM bioseq_cache_test.entry_states() "
                         "WHERE state = 'BUILDING' "
                         "ORDER BY generation LIMIT 1");
        if (token.rows() != 1 || token.columns() != 3) {
          throw std::runtime_error("BUILDING producer token is not visible");
        }
        failed_key = std::string(token.value(0, 0));
        failed_generation = std::string(token.value(0, 1));
        failed_claim_sequence = std::string(token.value(0, 2));
        producer_paused.store(true, std::memory_order_release);
        if (!bioseq::postgres_test::wait_until(
                [&session]() {
                  return session_boolean(
                      session, "SELECT EXISTS ("
                               "SELECT 1 "
                               "FROM bioseq_cache_test.entry_states() "
                               "WHERE state = 'BUILDING' "
                               "AND waiter_count = 1)");
                },
                session.deadline(), 10ms)) {
          throw std::runtime_error(
              "waiter did not attach before write failure");
        }
        static_cast<void>(
            session.exec("SELECT bioseq_cache_test.fail_next_write('enospc'); "
                         "SELECT bioseq_cache_test.release_pause("
                         "'after_building_publish')"));
      }});

  const auto outcomes = bioseq::postgres_test::run_independent_session_tasks(
      fixture.cluster(), tasks, 45s);
  REQUIRE(all_passed(outcomes));
  CHECK(producer_rows == ordered_ids(10));
  CHECK(waiter_rows == ordered_ids(10));
  CHECK(bioseq::cache_test::integer(admin,
                                    "SELECT count(*) "
                                    "FROM bioseq_cache_test.entry_states() "
                                    "WHERE state = 'BUILDING'") == 0);
  REQUIRE_FALSE(failed_key.empty());
  const std::string failed_state_sql =
      "SELECT state "
      "FROM bioseq_cache_test.entry_states() "
      "WHERE entry_key = " +
      admin.escape_literal(failed_key) +
      " AND generation = " + failed_generation +
      " AND claim_sequence = " + failed_claim_sequence;
  CHECK(bioseq::cache_test::scalar(admin, failed_state_sql) == "FAILED");
  const auto stats = bioseq::cache_test::read_cache_stats(admin);
  CHECK(stats.inflight_waits >= 1);
  CHECK(stats.fallbacks >= 1);
  bioseq::postgres_test::record_migration_probe(
      "tap.007.file_failure.full_parity");
}

TEST_CASE("waiter entitlement pins TTL-zero storage and drains on every exit",
          "[cache][integration][concurrency][ttl][recovery]") {
  bioseq::postgres_test::migration_probe_scope probes{
      "waiter entitlement pins TTL-zero storage and drains on every exit"};
  bioseq::postgres_test::postgres_fixture fixture{
      bioseq::cache_test::cluster_options(
          false, bioseq::cache_test::preload_order::cache_only)};
  auto &admin = fixture.admin();
  bioseq::cache_test::exec(admin, "CREATE TABLE entitlement_rows("
                                  "id integer PRIMARY KEY, value text); "
                                  "INSERT INTO entitlement_rows "
                                  "SELECT i, md5(i::text) "
                                  "FROM generate_series(1, 100) AS g(i); "
                                  "SELECT bioseq_cache.reset_stats()");
  const storage_snapshot baseline = read_storage_snapshot(admin);

  bioseq::cache_test::exec(admin, "SELECT bioseq_cache_test.arm_pause("
                                  "'before_terminal_publish'); "
                                  "SELECT bioseq_cache_test.arm_pause("
                                  "'after_waiter_registered')");
  std::atomic<bool> producer_at_terminal{};
  std::vector<std::string> cancelled_cycle_producer_rows;
  storage_snapshot entitled{};
  storage_snapshot aggressively_swept{};
  storage_snapshot reclaimed{};
  bool published_waiter_owned{};
  bool terminated_waiter_drained{};
  std::vector<bioseq::postgres_test::session_task> cancelled_tasks;
  cancelled_tasks.push_back(bioseq::postgres_test::session_task{
      .name = "entitlement-cancel-producer",
      .body = [&cancelled_cycle_producer_rows](
                  bioseq::postgres_test::session_context &session) {
        const auto result =
            session.exec("SET application_name = 'cache_entitlement_producer'; "
                         "SET bioseq.query_cache_optimisation_mode = 'same'; "
                         "SET bioseq.query_cache_ttl = 0; "
                         "SELECT id FROM entitlement_rows "
                         "WHERE id <= 100 ORDER BY id");
        cancelled_cycle_producer_rows = first_column(result);
      }});
  cancelled_tasks.push_back(bioseq::postgres_test::session_task{
      .name = "entitlement-cancel-waiter",
      .body = [&producer_at_terminal](
                  bioseq::postgres_test::session_context &session) {
        if (!bioseq::postgres_test::wait_until(
                [&producer_at_terminal]() {
                  return producer_at_terminal.load(std::memory_order_acquire);
                },
                session.deadline(), 5ms)) {
          throw std::runtime_error(
              "producer did not reach terminal publication");
        }
        static_cast<void>(
            session.exec("SET application_name = 'cache_entitlement_waiter'; "
                         "SET bioseq.query_cache_optimisation_mode = 'same'; "
                         "SET bioseq.query_cache_ttl = 0; "
                         "SELECT id FROM entitlement_rows "
                         "WHERE id <= 100 ORDER BY id"));
      }});
  cancelled_tasks.push_back(bioseq::postgres_test::session_task{
      .name = "entitlement-cancel-controller",
      .body = [&producer_at_terminal, &baseline, &entitled, &aggressively_swept,
               &reclaimed, &published_waiter_owned, &terminated_waiter_drained](
                  bioseq::postgres_test::session_context &session) {
        static_cast<void>(
            session.exec("SELECT bioseq_cache_test.wait_until_paused("
                         "'before_terminal_publish')"));
        producer_at_terminal.store(true, std::memory_order_release);
        static_cast<void>(
            session.exec("SELECT bioseq_cache_test.wait_until_paused("
                         "'after_waiter_registered'); "
                         "SELECT bioseq_cache_test.release_pause("
                         "'before_terminal_publish')"));
        if (!bioseq::postgres_test::wait_until(
                [&session]() {
                  return session_boolean(
                      session, "SELECT count(*) = 1 "
                               "FROM bioseq_cache_test.entry_states() "
                               "WHERE state = 'COMPLETE' "
                               "AND waiter_count = 1 "
                               "AND reader_count = 0");
                },
                session.deadline(), 10ms)) {
          throw std::runtime_error(
              "published waiter entitlement is not visible");
        }
        published_waiter_owned = true;
        entitled = read_storage_snapshot(session);
        static_cast<void>(
            session.exec("SELECT bioseq_cache_test.request_janitor_sweep()"));
        std::this_thread::sleep_for(1200ms);
        aggressively_swept = read_storage_snapshot(session);

        const std::string waiter_pid = session_scalar(
            session, "SELECT pid::text FROM pg_stat_activity "
                     "WHERE application_name = 'cache_entitlement_waiter'");
        if (!session_boolean(session, "SELECT pg_terminate_backend(" +
                                          waiter_pid + ")")) {
          throw std::runtime_error("waiter backend termination was rejected");
        }
        terminated_waiter_drained = bioseq::postgres_test::wait_until(
            [&session, &waiter_pid]() {
              return session_boolean(
                  session, "WITH sweep AS MATERIALIZED ("
                           "SELECT "
                           "bioseq_cache_test.request_janitor_sweep()) "
                           "SELECT NOT EXISTS ("
                           "SELECT 1 FROM pg_stat_activity "
                           "WHERE pid = " +
                               waiter_pid +
                               ") "
                               "AND NOT EXISTS ("
                               "SELECT 1 "
                               "FROM bioseq_cache_test.entry_states() "
                               "WHERE waiter_count <> 0) "
                               "FROM sweep");
            },
            session.deadline(), 20ms);
        static_cast<void>(session.exec("SELECT bioseq_cache_test.release_pause("
                                       "'after_waiter_registered')"));
        if (!bioseq::postgres_test::wait_until(
                [&session, &baseline, &reclaimed]() {
                  static_cast<void>(session.exec(
                      "SELECT "
                      "bioseq_cache_test.request_janitor_sweep()"));
                  reclaimed = read_storage_snapshot(session);
                  return reclaimed == baseline;
                },
                session.deadline(), 20ms)) {
          throw std::runtime_error(
              "cancelled waiter storage was not reclaimed");
        }
      }});

  const auto cancelled_outcomes =
      bioseq::postgres_test::run_independent_session_tasks(
          fixture.cluster(), cancelled_tasks, 60s);
  REQUIRE(cancelled_outcomes.size() == 3);
  CHECK(cancelled_outcomes[0].passed);
  CHECK_FALSE(cancelled_outcomes[0].timed_out);
  CHECK_FALSE(cancelled_outcomes[1].passed);
  CHECK_FALSE(cancelled_outcomes[1].timed_out);
  CHECK(cancelled_outcomes[2].passed);
  CHECK(cancelled_cycle_producer_rows == ordered_ids(100));
  CHECK(published_waiter_owned);
  CHECK(entitled.manifests == baseline.manifests + 1);
  CHECK(entitled.waiters == baseline.waiters + 1);
  CHECK(entitled.hot_bytes + entitled.file_bytes >
        baseline.hot_bytes + baseline.file_bytes);
  CHECK(aggressively_swept.manifests == entitled.manifests);
  CHECK(aggressively_swept.waiters == entitled.waiters);
  CHECK(terminated_waiter_drained);
  CHECK(reclaimed == baseline);
  CHECK(bioseq::cache_test::integer(
            admin, "SELECT count(*) "
                   "FROM bioseq_cache_test.entry_states() "
                   "WHERE waiter_count <> 0 OR reader_count <> 0") == 0);

  bioseq::cache_test::exec(admin, "SELECT bioseq_cache.reset_stats(); "
                                  "SELECT bioseq_cache_test.arm_pause("
                                  "'before_terminal_publish'); "
                                  "SELECT bioseq_cache_test.arm_pause("
                                  "'after_waiter_registered')");
  std::atomic<bool> success_producer_at_terminal{};
  std::vector<std::string> success_producer_rows;
  std::vector<std::string> success_waiter_rows;
  bool successful_entitlement_owned{};
  bool successful_sweep_preserved{};
  std::vector<bioseq::postgres_test::session_task> success_tasks;
  success_tasks.push_back(bioseq::postgres_test::session_task{
      .name = "entitlement-success-producer",
      .body = [&success_producer_rows](
                  bioseq::postgres_test::session_context &session) {
        const auto result =
            session.exec("SET application_name = "
                         "'cache_entitlement_success_producer'; "
                         "SET bioseq.query_cache_optimisation_mode = 'same'; "
                         "SET bioseq.query_cache_ttl = 0; "
                         "SELECT id FROM entitlement_rows "
                         "WHERE id <= 25 ORDER BY id");
        success_producer_rows = first_column(result);
      }});
  success_tasks.push_back(bioseq::postgres_test::session_task{
      .name = "entitlement-success-waiter",
      .body = [&success_producer_at_terminal, &success_waiter_rows](
                  bioseq::postgres_test::session_context &session) {
        if (!bioseq::postgres_test::wait_until(
                [&success_producer_at_terminal]() {
                  return success_producer_at_terminal.load(
                      std::memory_order_acquire);
                },
                session.deadline(), 5ms)) {
          throw std::runtime_error(
              "success producer did not reach terminal publication");
        }
        const auto result =
            session.exec("SET application_name = "
                         "'cache_entitlement_success_waiter'; "
                         "SET bioseq.query_cache_optimisation_mode = 'same'; "
                         "SET bioseq.query_cache_ttl = 0; "
                         "SELECT id FROM entitlement_rows "
                         "WHERE id <= 25 ORDER BY id");
        success_waiter_rows = first_column(result);
      }});
  success_tasks.push_back(bioseq::postgres_test::session_task{
      .name = "entitlement-success-controller",
      .body = [&success_producer_at_terminal, &successful_entitlement_owned,
               &successful_sweep_preserved](
                  bioseq::postgres_test::session_context &session) {
        static_cast<void>(
            session.exec("SELECT bioseq_cache_test.wait_until_paused("
                         "'before_terminal_publish')"));
        success_producer_at_terminal.store(true, std::memory_order_release);
        static_cast<void>(
            session.exec("SELECT bioseq_cache_test.wait_until_paused("
                         "'after_waiter_registered'); "
                         "SELECT bioseq_cache_test.release_pause("
                         "'before_terminal_publish')"));
        successful_entitlement_owned = bioseq::postgres_test::wait_until(
            [&session]() {
              return session_boolean(session,
                                     "SELECT count(*) = 1 "
                                     "FROM bioseq_cache_test.entry_states() "
                                     "WHERE state = 'COMPLETE' "
                                     "AND waiter_count = 1 "
                                     "AND reader_count = 0");
            },
            session.deadline(), 10ms);
        static_cast<void>(
            session.exec("SELECT bioseq_cache_test.request_janitor_sweep()"));
        successful_sweep_preserved = session_boolean(
            session, "SELECT count(*) = 1 "
                     "FROM bioseq_cache_test.entry_states() "
                     "WHERE state = 'COMPLETE' AND waiter_count = 1");
        static_cast<void>(session.exec("SELECT bioseq_cache_test.release_pause("
                                       "'after_waiter_registered')"));
      }});

  const auto success_outcomes =
      bioseq::postgres_test::run_independent_session_tasks(fixture.cluster(),
                                                           success_tasks, 45s);
  REQUIRE(all_passed(success_outcomes));
  CHECK(successful_entitlement_owned);
  CHECK(successful_sweep_preserved);
  CHECK(success_producer_rows == ordered_ids(25));
  CHECK(success_waiter_rows == ordered_ids(25));
  const auto entitlement_stats = bioseq::cache_test::read_cache_stats(admin);
  CHECK(entitlement_stats.produced == 1);
  CHECK(entitlement_stats.completed_hits == 1);
  CHECK(entitlement_stats.inflight_waits == 1);

  const auto post_complete =
      admin.exec("SET bioseq.query_cache_optimisation_mode = 'same'; "
                 "SET bioseq.query_cache_ttl = 0; "
                 "SELECT id FROM entitlement_rows "
                 "WHERE id <= 25 ORDER BY id");
  CHECK(first_column(post_complete) == ordered_ids(25));
  const auto post_complete_stats = bioseq::cache_test::read_cache_stats(admin);
  CHECK(post_complete_stats.produced == 2);
  CHECK(post_complete_stats.completed_hits == 1);
  REQUIRE(bioseq::postgres_test::wait_until(
      [&admin, &baseline]() { return snapshot_reclaimed(admin, baseline); },
      std::chrono::steady_clock::now() + 10s, 20ms));
  CHECK(bioseq::cache_test::integer(
            admin, "SELECT count(*) "
                   "FROM bioseq_cache_test.entry_states() "
                   "WHERE waiter_count <> 0 OR reader_count <> 0") == 0);
  const auto source =
      admin.exec("SELECT count(*), sum(id) FROM entitlement_rows");
  REQUIRE(source.rows() == 1);
  CHECK(source.value(0, 0) == "100");
  CHECK(source.value(0, 1) == "5050");
  bioseq::postgres_test::record_migration_probe(
      "tap.010.waiter_entitlement.full_parity");
}

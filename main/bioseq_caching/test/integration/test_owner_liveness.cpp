#include <catch2/catch_test_macros.hpp>

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

namespace {

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

[[nodiscard]] std::string
storage_boot_nonce(bioseq::libpq::connection &connection) {
  return bioseq::cache_test::scalar(
      connection,
      "SELECT lpad(to_hex(("
      "bioseq_cache_test.bootstrap_status()->>'boot_nonce')::bigint), "
      "16, '0')");
}

[[nodiscard]] bool lower_hex(std::string_view text) {
  return std::ranges::all_of(text, [](char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f');
  });
}

[[nodiscard]] bool cache_file_name(std::string_view name) {
  constexpr std::size_t digest_size = 64;
  constexpr std::size_t nonce_size = 16;
  constexpr std::string_view suffix{".cache"};
  if (name.size() != digest_size + 1 + nonce_size + suffix.size()) {
    return false;
  }
  return lower_hex(name.substr(0, digest_size)) && name[digest_size] == '.' &&
         lower_hex(name.substr(digest_size + 1, nonce_size)) &&
         name.substr(digest_size + 1 + nonce_size) == suffix;
}

[[nodiscard]] std::vector<std::filesystem::path>
cache_files(const bioseq::postgres_test::postgres_fixture &fixture,
            std::string_view boot_nonce, std::string_view database_oid) {
  const std::filesystem::path directory =
      fixture.cluster().data_directory() / "pg_bioseq_cache" /
      std::string(boot_nonce) / std::string(database_oid);
  std::vector<std::filesystem::path> files;
  if (!std::filesystem::is_directory(directory)) {
    return files;
  }
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (entry.is_regular_file() &&
        cache_file_name(entry.path().filename().string())) {
      files.push_back(entry.path());
    }
  }
  std::ranges::sort(files);
  return files;
}

[[nodiscard]] bool server_accepts_connections(
    const bioseq::postgres_test::private_cluster &cluster) {
  try {
    auto connection = cluster.connect(1s);
    const auto result = connection.exec("SELECT 1", 1s);
    return result.rows() == 1 && result.columns() == 1 &&
           result.value(0, 0) == "1";
  } catch (...) {
    return false;
  }
}

} // namespace

TEST_CASE("dead producer and reader owners are recovered without stale storage",
          "[cache][integration][recovery][file-tier][owner]") {
  bioseq::postgres_test::migration_probe_scope probes{
      "dead producer and reader owners are recovered without stale storage"};
  bioseq::postgres_test::postgres_fixture fixture{
      bioseq::cache_test::cluster_options(
          false, bioseq::cache_test::preload_order::cache_only,
          {{"restart_after_crash", "on"}})};
  auto &admin = fixture.admin();
  bioseq::cache_test::exec(
      admin, "CREATE TABLE owner_rows("
             "id integer PRIMARY KEY, value text); "
             "INSERT INTO owner_rows "
             "SELECT i, md5(i::text) "
             "FROM generate_series(1, 64) AS g(i); "
             "SELECT bioseq_cache_test.set_force_file_tier(true); "
             "SELECT bioseq_cache_test.arm_pause('after_building_publish')");

  bool live_owner_visible{};
  std::vector<bioseq::postgres_test::session_task> owner_tasks;
  owner_tasks.push_back(bioseq::postgres_test::session_task{
      .name = "owner-liveness-producer",
      .body = [](bioseq::postgres_test::session_context &session) {
        static_cast<void>(
            session.exec("SET application_name = 'cache_dead_producer'; "
                         "SET bioseq.query_cache_optimisation_mode = 'same'; "
                         "SELECT count(*), sum(id) FROM owner_rows"));
      }});
  owner_tasks.push_back(bioseq::postgres_test::session_task{
      .name = "owner-liveness-controller",
      .body = [&live_owner_visible](
                  bioseq::postgres_test::session_context &session) {
        static_cast<void>(
            session.exec("SELECT bioseq_cache_test.wait_until_paused("
                         "'after_building_publish')"));
        live_owner_visible = session_boolean(
            session, "SELECT EXISTS ("
                     "SELECT 1 FROM bioseq_cache_test.entry_states() "
                     "WHERE state = 'BUILDING' AND producer_pid > 0)");
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
  const auto owner_outcomes =
      bioseq::postgres_test::run_independent_session_tasks(fixture.cluster(),
                                                           owner_tasks, 45s);
  REQUIRE(owner_outcomes.size() == 2);
  CHECK_FALSE(owner_outcomes[0].passed);
  CHECK_FALSE(owner_outcomes[0].timed_out);
  CHECK(owner_outcomes[1].passed);
  CHECK(live_owner_visible);
  REQUIRE(bioseq::postgres_test::wait_until(
      [&admin]() {
        return bioseq::cache_test::integer(
                   admin, "SELECT count(*) "
                          "FROM bioseq_cache_test.entry_states() "
                          "WHERE state = 'BUILDING'") == 0;
      },
      std::chrono::steady_clock::now() + 10s, 20ms));

  const auto recovered_owner =
      admin.exec("SET bioseq.query_cache_optimisation_mode = 'same'; "
                 "SELECT count(*), sum(id) FROM owner_rows");
  REQUIRE(recovered_owner.rows() == 1);
  CHECK(recovered_owner.value(0, 0) == "64");
  CHECK(recovered_owner.value(0, 1) == "2080");

  const std::string old_boot_nonce = storage_boot_nonce(admin);
  const std::string database_oid =
      bioseq::cache_test::scalar(admin, "SELECT oid::text FROM pg_database "
                                        "WHERE datname = current_database()");
  const auto old_files = cache_files(fixture, old_boot_nonce, database_oid);
  REQUIRE_FALSE(old_files.empty());

  bioseq::cache_test::exec(
      admin, "SELECT bioseq_cache_test.arm_pause('after_reader_pinned')");
  bool reader_ownership_visible{};
  bool reader_sigkilled{};
  std::vector<bioseq::postgres_test::session_task> reader_tasks;
  reader_tasks.push_back(bioseq::postgres_test::session_task{
      .name = "owner-liveness-reader",
      .body = [](bioseq::postgres_test::session_context &session) {
        static_cast<void>(
            session.exec("SET application_name = 'cache_dead_reader'; "
                         "SET bioseq.query_cache_optimisation_mode = 'same'; "
                         "SELECT count(*), sum(id) FROM owner_rows"));
      }});
  reader_tasks.push_back(bioseq::postgres_test::session_task{
      .name = "owner-liveness-killer",
      .body = [&reader_ownership_visible, &reader_sigkilled](
                  bioseq::postgres_test::session_context &session) {
        static_cast<void>(
            session.exec("SELECT bioseq_cache_test.wait_until_paused("
                         "'after_reader_pinned')"));
        reader_ownership_visible = session_boolean(
            session, "SELECT EXISTS ("
                     "SELECT 1 FROM bioseq_cache_test.entry_states() "
                     "WHERE state = 'COMPLETE' AND reader_count = 1) "
                     "AND (SELECT reader_pins = 1 "
                     "FROM bioseq_cache_test.storage_tier_stats())");
        const std::string reader_pid = session_scalar(
            session, "SELECT pid::text FROM pg_stat_activity "
                     "WHERE application_name = 'cache_dead_reader'");
        const auto pid = static_cast<pid_t>(std::stoll(reader_pid));
        if (pid <= 1) {
          throw std::runtime_error("reader backend PID is not signal-safe");
        }
        reader_sigkilled = ::kill(pid, SIGKILL) == 0;
        if (!reader_sigkilled) {
          throw std::runtime_error("reader backend SIGKILL was rejected");
        }
      }});
  const auto reader_outcomes =
      bioseq::postgres_test::run_independent_session_tasks(fixture.cluster(),
                                                           reader_tasks, 45s);
  REQUIRE(reader_outcomes.size() == 2);
  CHECK_FALSE(reader_outcomes[0].passed);
  CHECK_FALSE(reader_outcomes[0].timed_out);
  CHECK(reader_outcomes[1].passed);
  CHECK(reader_ownership_visible);
  CHECK(reader_sigkilled);

  REQUIRE(bioseq::postgres_test::wait_until(
      [&fixture]() { return server_accepts_connections(fixture.cluster()); },
      std::chrono::steady_clock::now() + 30s, 50ms));
  fixture.reconnect_admin();
  auto &recovered = fixture.admin();
  bioseq::cache_test::exec(
      recovered, "SELECT bioseq_cache_test.set_force_file_tier(true); "
                 "SELECT bioseq_cache.reset_stats()");
  CHECK(bioseq::cache_test::integer(
            recovered, "SELECT count(*) FROM bioseq_cache.entries()") == 0);
  for (const auto &old_file : old_files) {
    CHECK_FALSE(std::filesystem::exists(old_file));
  }
  CHECK(bioseq::cache_test::integer(
            recovered, "SELECT reader_pins "
                       "FROM bioseq_cache_test.storage_tier_stats()") == 0);

  const auto after_crash =
      recovered.exec("SET bioseq.query_cache_optimisation_mode = 'same'; "
                     "SELECT count(*), sum(id) FROM owner_rows");
  REQUIRE(after_crash.rows() == 1);
  CHECK(after_crash.value(0, 0) == "64");
  CHECK(after_crash.value(0, 1) == "2080");
  const auto after_crash_stats =
      bioseq::cache_test::read_cache_stats(recovered);
  CHECK(after_crash_stats.produced == 1);
  CHECK(after_crash_stats.completed_hits == 0);
  const std::string new_boot_nonce = storage_boot_nonce(recovered);
  CHECK(new_boot_nonce != old_boot_nonce);
  CHECK_FALSE(cache_files(fixture, new_boot_nonce, database_oid).empty());

  bioseq::cache_test::exec(
      recovered, "SELECT bioseq_cache_test.set_force_file_tier(false); "
                 "SELECT bioseq_cache.invalidate_all(); "
                 "SELECT bioseq_cache_test.request_janitor_sweep()");
  REQUIRE(bioseq::postgres_test::wait_until(
      [&recovered]() {
        bioseq::cache_test::exec(
            recovered, "SELECT bioseq_cache_test.request_janitor_sweep()");
        return bioseq::cache_test::integer(
                   recovered, "SELECT count(*) "
                              "FROM bioseq_cache_test.entry_states()") == 0 &&
               bioseq::cache_test::integer(
                   recovered,
                   "SELECT reader_pins "
                   "FROM bioseq_cache_test.storage_tier_stats()") == 0;
      },
      std::chrono::steady_clock::now() + 10s, 20ms));
  bioseq::postgres_test::record_migration_probe(
      "tap.008.owner_liveness.full_parity");
}

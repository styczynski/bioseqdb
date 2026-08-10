#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <unistd.h>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

namespace {

[[nodiscard]] std::string boot_nonce(
    bioseq::libpq::client_connection& connection) {
    return bioseq::cache_test::scalar(
        connection,
        "SELECT lpad(to_hex("
        "(bioseq_cache_test.bootstrap_status()->>'boot_nonce')::bigint), "
        "16, '0')");
}

[[nodiscard]] std::filesystem::path partial_file_path(
    bioseq::postgres_test::private_cluster_fixture& fixture,
    bioseq::libpq::client_connection& connection,
    std::string_view nonce) {
    const std::string database_oid = bioseq::cache_test::scalar(
        connection,
        "SELECT oid::text FROM pg_database "
        "WHERE datname = current_database()");
    const auto directory =
        fixture.cluster().data_directory() / "pg_bioseq_cache"
        / std::string(nonce) / database_oid;
    std::filesystem::create_directories(directory);
    return directory
        / ".stage-00000000-00000000-0000000000000000-"
          "0000000000000000-ffffffffffffffff.partial";
}

void require_cross_session_postmaster_descendant(
    const bioseq::postgres_test::private_cluster& cluster) {
    const std::uint64_t postmaster_pid =
        bioseq::postgres_test::read_postmaster_pid_file(
            cluster.data_directory());
    const auto postmaster =
        bioseq::postgres_test::capture_supervised_process_identity(
            postmaster_pid, {}, cluster.data_directory());
    const auto process_tree =
        bioseq::postgres_test::capture_postgres_process_tree_pids(
            postmaster_pid);
    REQUIRE(postmaster.has_value());
    REQUIRE(process_tree.has_value());
    REQUIRE(process_tree->size() > 1U);
    const pid_t postmaster_session =
        ::getsid(static_cast<pid_t>(postmaster_pid));
    REQUIRE(postmaster_session > 1);

    bool observed_cross_session_descendant = false;
    for (const std::uint64_t pid : *process_tree) {
        if (pid == postmaster_pid) {
            continue;
        }
        const auto descendant =
            bioseq::postgres_test::capture_supervised_process_identity(
                pid, {}, cluster.data_directory());
        if (!descendant) {
            continue;
        }
        const pid_t session =
            ::getsid(static_cast<pid_t>(pid));
        if (session == static_cast<pid_t>(pid)
            && descendant->process_group
                == static_cast<std::int64_t>(pid)
            && session != postmaster_session) {
            observed_cross_session_descendant = true;
            break;
        }
    }
    REQUIRE(observed_cross_session_descendant);
}

}  // namespace

TEST_CASE("file tier replays and janitor reclamation is coherent",
          "[component:cache][layer:integration]"
          "[feature:storage_lifecycle][resource:postgres][resource:filesystem]") {
    bioseq::postgres_test::private_cluster_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();
    bioseq::cache_test::execute(
        connection,
        "CREATE TABLE cache_tier_rows("
        "id integer PRIMARY KEY, payload text NOT NULL); "
        "INSERT INTO cache_tier_rows "
        "SELECT i, repeat(md5(i::text), 1024) "
        "FROM generate_series(1, 64) AS g(i); "
        "SELECT bioseq_cache.reset_stats(); "
        "SELECT bioseq_cache_test.set_force_file_tier(true); "
        "SET bioseq_cache.query_cache.mode = 'exact_query'");

    constexpr std::string_view query{
        "SELECT count(*), sum(id) "
        "FROM cache_tier_rows WHERE id <= 64"};
    const auto cold = connection.execute(query, 30s);
    const auto warm = connection.execute(query, 30s);
    REQUIRE(cold.row_count() == 1);
    CHECK(cold.text_value(0, 0) == "64");
    CHECK(cold.text_value(0, 1) == "2080");
    CHECK(bioseq::postgres_test::hash_ordered_result_rows(cold)
          == bioseq::postgres_test::hash_ordered_result_rows(warm));

    CHECK(bioseq::cache_test::boolean(
        connection,
        "SELECT force_file_tier "
        "FROM bioseq_cache_test.storage_tier_stats()"));
    CHECK(bioseq::cache_test::integer(
              connection,
              "SELECT file_blocks "
              "FROM bioseq_cache_test.storage_tier_stats()")
          > 0);
    CHECK(bioseq::cache_test::integer(
              connection,
              "SELECT reserved_file_bytes "
              "FROM bioseq_cache_test.storage_tier_stats()")
          > 0);
    CHECK(bioseq::cache_test::boolean(
        connection,
        "SELECT coherent "
        "FROM bioseq_cache_test.storage_tier_stats()"));
    CHECK(bioseq::cache_test::integer(
              connection,
              "SELECT reader_pins "
              "FROM bioseq_cache_test.storage_tier_stats()")
          == 0);
    const auto stats = bioseq::cache_test::read_cache_stats(connection);
    CHECK(stats.produced == 1);
    CHECK(stats.completed_hits == 1);

    bioseq::cache_test::execute(
        connection,
        "SELECT bioseq_cache_test.set_force_file_tier(false); "
        "SELECT bioseq_cache.invalidate_all(); "
        "SELECT bioseq_cache_test.request_janitor_sweep()");
    const bool reclaimed = bioseq::postgres_test::wait_until(
        [&connection]() {
            bioseq::cache_test::execute(
                connection,
                "SELECT bioseq_cache_test.request_janitor_sweep()");
            return bioseq::cache_test::integer(
                       connection,
                       "SELECT file_blocks "
                       "FROM bioseq_cache_test.storage_tier_stats()")
                       == 0
                && bioseq::cache_test::integer(
                       connection,
                       "SELECT reserved_file_bytes "
                       "FROM bioseq_cache_test.storage_tier_stats()")
                       == 0;
        },
        std::chrono::steady_clock::now() + 10s, 20ms);
    REQUIRE(reclaimed);
    CHECK(bioseq::cache_test::boolean(
        connection,
        "SELECT coherent "
        "FROM bioseq_cache_test.storage_tier_stats()"));

    bioseq::cache_test::execute(
        connection,
        "SET bioseq_cache.query_cache.completed_result_ttl_ms = 1; "
        "SELECT min(id), max(id) FROM cache_tier_rows");
    REQUIRE(bioseq::postgres_test::wait_until(
        [&connection]() {
            bioseq::cache_test::execute(
                connection,
                "SELECT bioseq_cache_test.request_janitor_sweep()");
            return bioseq::cache_test::integer(
                       connection,
                       "SELECT count(*) FROM bioseq_cache.entries()")
                == 0;
        },
        std::chrono::steady_clock::now() + 10s, 20ms));
}

TEST_CASE("restart removes boot-scoped entries and partial files",
          "[component:cache][layer:integration]"
          "[feature:storage_lifecycle][resource:postgres][resource:filesystem]") {
    bioseq::postgres_test::private_cluster_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();
    bioseq::cache_test::execute(
        connection,
        "CREATE TABLE cache_restart_rows("
        "id integer PRIMARY KEY, value text); "
        "INSERT INTO cache_restart_rows VALUES (1, 'one'), (2, 'two'); "
        "SELECT bioseq_cache_test.set_force_file_tier(true); "
        "SET bioseq_cache.query_cache.mode = 'exact_query'");
    static_cast<void>(connection.execute(
        "SELECT id, value FROM cache_restart_rows ORDER BY id"));
    static_cast<void>(connection.execute(
        "SELECT id, value FROM cache_restart_rows ORDER BY id"));
    CHECK(bioseq::cache_test::integer(
              connection,
              "SELECT count(*) FROM bioseq_cache.entries() "
              "WHERE state = 'COMPLETE'")
          >= 1);
    const std::string old_nonce = boot_nonce(connection);
    const std::filesystem::path partial =
        partial_file_path(fixture, connection, old_nonce);
    {
        std::ofstream output(partial, std::ios::trunc);
        REQUIRE(output.good());
        output << "incomplete cache block\n";
    }
    REQUIRE(std::filesystem::is_regular_file(partial));

    fixture.cluster().restart();
    fixture.reconnect_admin();
    auto& restarted = fixture.admin();
    CHECK(bioseq::cache_test::boolean(
        restarted,
        "SELECT (bioseq_cache_test.bootstrap_status()->>"
        "'storage_attached')::boolean "
        "AND bioseq_cache_test.cache_infrastructure_ready()"));
    CHECK(bioseq::cache_test::integer(
              restarted,
              "SELECT count(*) FROM bioseq_cache.entries()")
          == 0);
    CHECK_FALSE(std::filesystem::exists(partial));
    const std::string new_nonce = boot_nonce(restarted);
    CHECK(new_nonce != old_nonce);

    bioseq::cache_test::execute(
        restarted,
        "SELECT bioseq_cache_test.set_force_file_tier(true); "
        "SET bioseq_cache.query_cache.mode = 'exact_query'; "
        "SELECT bioseq_cache.reset_stats()");
    const auto result = restarted.execute(
        "SELECT id, value FROM cache_restart_rows ORDER BY id");
    REQUIRE(result.row_count() == 2);
    const auto fresh_stats =
        bioseq::cache_test::read_cache_stats(restarted);
    CHECK(fresh_stats.produced == 1);
    CHECK(fresh_stats.completed_hits == 0);
}

TEST_CASE("unclean postmaster death performs crash recovery",
          "[component:cache][layer:integration]"
          "[feature:storage_lifecycle][resource:postgres][resource:filesystem]") {
    bioseq::postgres_test::private_cluster_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();
    bioseq::cache_test::execute(
        connection,
        "CREATE TABLE cache_crash_recovery_rows(id integer PRIMARY KEY); "
        "INSERT INTO cache_crash_recovery_rows VALUES (1), (2); "
        "SET bioseq_cache.query_cache.mode = 'exact_query'");
    static_cast<void>(connection.execute(
        "SELECT id FROM cache_crash_recovery_rows ORDER BY id"));

    require_cross_session_postmaster_descendant(
        fixture.cluster());
    fixture.cluster().crash_postmaster();
    fixture.cluster().start();
    fixture.reconnect_admin();
    auto& recovered = fixture.admin();
    CHECK(bioseq::cache_test::boolean(
        recovered,
        "SELECT (bioseq_cache_test.bootstrap_status()->>"
        "'storage_attached')::boolean "
        "AND bioseq_cache_test.cache_infrastructure_ready()"));
    CHECK(bioseq::cache_test::integer(
              recovered,
              "SELECT count(*) FROM cache_crash_recovery_rows")
          == 2);
    CHECK(bioseq::cache_test::integer(
              recovered,
              "SELECT count(*) FROM bioseq_cache.entries()")
          == 0);
}

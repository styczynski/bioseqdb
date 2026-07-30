#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

namespace {

[[nodiscard]] std::string boot_nonce(
    bioseq::libpq::connection& connection) {
    return bioseq::cache_test::scalar(
        connection,
        "SELECT lpad(to_hex("
        "(bioseq_cache_test.bootstrap_status()->>'boot_nonce')::bigint), "
        "16, '0')");
}

[[nodiscard]] std::filesystem::path partial_file_path(
    bioseq::postgres_test::postgres_fixture& fixture,
    bioseq::libpq::connection& connection,
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

}  // namespace

TEST_CASE("file tier replays and janitor reclamation is coherent",
          "[cache][integration][storage][file-tier][ttl]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "file tier replays and janitor reclamation is coherent"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();
    bioseq::cache_test::exec(
        connection,
        "CREATE TABLE cache_tier_rows("
        "id integer PRIMARY KEY, payload text NOT NULL); "
        "INSERT INTO cache_tier_rows "
        "SELECT i, repeat(md5(i::text), 1024) "
        "FROM generate_series(1, 64) AS g(i); "
        "SELECT bioseq_cache.reset_stats(); "
        "SELECT bioseq_cache_test.set_force_file_tier(true); "
        "SET bioseq.query_cache_optimisation_mode = 'same'");

    constexpr std::string_view query{
        "SELECT count(*), sum(id) "
        "FROM cache_tier_rows WHERE id <= 64"};
    const auto cold = connection.exec(query, 30s);
    const auto warm = connection.exec(query, 30s);
    REQUIRE(cold.rows() == 1);
    CHECK(cold.value(0, 0) == "64");
    CHECK(cold.value(0, 1) == "2080");
    CHECK(bioseq::postgres_test::exact_row_hash(cold)
          == bioseq::postgres_test::exact_row_hash(warm));
    bioseq::postgres_test::record_migration_probe(
        "tap.011.file_tier.producer_rows");
    bioseq::postgres_test::record_migration_probe(
        "tap.011.file_tier.replay_rows");

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
    bioseq::postgres_test::record_migration_probe(
        "tap.011.file_tier.override_observable");
    bioseq::postgres_test::record_migration_probe(
        "tap.011.file_tier.file_blocks_reserved");
    bioseq::postgres_test::record_migration_probe(
        "tap.011.file_tier.storage_coherent");
    bioseq::postgres_test::record_migration_probe(
        "tap.011.file_tier.reader_pin_drained");

    bioseq::cache_test::exec(
        connection,
        "SELECT bioseq_cache_test.set_force_file_tier(false); "
        "SELECT bioseq_cache.invalidate_all(); "
        "SELECT bioseq_cache_test.request_janitor_sweep()");
    const bool reclaimed = bioseq::postgres_test::wait_until(
        [&connection]() {
            bioseq::cache_test::exec(
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
    bioseq::postgres_test::record_migration_probe(
        "tap.011.file_tier.janitor_unlinks_blocks");
    bioseq::postgres_test::record_migration_probe(
        "tap.011.file_tier.janitor_releases_bytes");

    bioseq::cache_test::exec(
        connection,
        "SET bioseq.query_cache_ttl = 1; "
        "SELECT min(id), max(id) FROM cache_tier_rows");
    REQUIRE(bioseq::postgres_test::wait_until(
        [&connection]() {
            bioseq::cache_test::exec(
                connection,
                "SELECT bioseq_cache_test.request_janitor_sweep()");
            return bioseq::cache_test::integer(
                       connection,
                       "SELECT count(*) FROM bioseq_cache.entries()")
                == 0;
        },
        std::chrono::steady_clock::now() + 10s, 20ms));
    bioseq::postgres_test::record_migration_probe(
        "tap.011.ttl.expired_entry_swept");
}

TEST_CASE("restart removes boot-scoped entries and partial files",
          "[cache][integration][storage][restart][recovery]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "restart removes boot-scoped entries and partial files"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();
    bioseq::cache_test::exec(
        connection,
        "CREATE TABLE cache_restart_rows("
        "id integer PRIMARY KEY, value text); "
        "INSERT INTO cache_restart_rows VALUES (1, 'one'), (2, 'two'); "
        "SELECT bioseq_cache_test.set_force_file_tier(true); "
        "SET bioseq.query_cache_optimisation_mode = 'same'");
    static_cast<void>(connection.exec(
        "SELECT id, value FROM cache_restart_rows ORDER BY id"));
    static_cast<void>(connection.exec(
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
    bioseq::postgres_test::record_migration_probe(
        "tap.003.restart.complete_entry_before_restart");
    bioseq::postgres_test::record_migration_probe(
        "tap.003.restart.synthetic_partial_exists");

    fixture.cluster().restart();
    fixture.reconnect_admin();
    auto& restarted = fixture.admin();
    CHECK(bioseq::cache_test::integer(
              restarted,
              "SELECT count(*) FROM bioseq_cache.entries()")
          == 0);
    CHECK_FALSE(std::filesystem::exists(partial));
    const std::string new_nonce = boot_nonce(restarted);
    CHECK(new_nonce != old_nonce);
    bioseq::postgres_test::record_migration_probe(
        "tap.003.restart.registry_is_boot_scoped");
    bioseq::postgres_test::record_migration_probe(
        "tap.003.restart.partial_removed");
    bioseq::postgres_test::record_migration_probe(
        "tap.003.restart.boot_nonce_changes");
    bioseq::postgres_test::record_migration_probe(
        "tap.004.partial_cleanup.orphan_removed");

    bioseq::cache_test::exec(
        restarted,
        "SELECT bioseq_cache_test.set_force_file_tier(true); "
        "SET bioseq.query_cache_optimisation_mode = 'same'; "
        "SELECT bioseq_cache.reset_stats()");
    const auto result = restarted.exec(
        "SELECT id, value FROM cache_restart_rows ORDER BY id");
    REQUIRE(result.rows() == 2);
    const auto fresh_stats =
        bioseq::cache_test::read_cache_stats(restarted);
    CHECK(fresh_stats.produced == 1);
    CHECK(fresh_stats.completed_hits == 0);
    bioseq::postgres_test::record_migration_probe(
        "tap.003.restart.first_query_is_fresh_miss");
}

TEST_CASE("unclean postmaster death performs crash recovery",
          "[cache][integration][storage][postmaster-crash][recovery]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "unclean postmaster death performs crash recovery"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();
    bioseq::cache_test::exec(
        connection,
        "CREATE TABLE cache_crash_recovery_rows(id integer PRIMARY KEY); "
        "INSERT INTO cache_crash_recovery_rows VALUES (1), (2); "
        "SET bioseq.query_cache_optimisation_mode = 'same'");
    static_cast<void>(connection.exec(
        "SELECT id FROM cache_crash_recovery_rows ORDER BY id"));

    fixture.cluster().crash_postmaster();
    fixture.cluster().start();
    fixture.reconnect_admin();
    auto& recovered = fixture.admin();
    CHECK(bioseq::cache_test::integer(
              recovered,
              "SELECT count(*) FROM cache_crash_recovery_rows")
          == 2);
    CHECK(bioseq::cache_test::integer(
              recovered,
              "SELECT count(*) FROM bioseq_cache.entries()")
          == 0);
    bioseq::postgres_test::record_migration_probe(
        "tap.003.restart.crash_recovery_preserves_source");
    bioseq::postgres_test::record_migration_probe(
        "tap.003.restart.crash_recovery_discards_cache");
}

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <string>
#include <string_view>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

TEST_CASE("cache install, preload diagnostics, and ACLs are native",
          "[cache][integration][install][security][guc]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "cache install, preload diagnostics, and ACLs are native"};

    SECTION("cache-required modes reject a non-preloaded postmaster") {
        bioseq::postgres_test::postgres_fixture fixture{
            bioseq::cache_test::cluster_options(
                false, bioseq::cache_test::preload_order::none)};
        auto& connection = fixture.admin();

        struct non_preloaded_mode {
            std::string_view mode;
            std::string_view probe;
        };
        constexpr std::array scenarios{
            non_preloaded_mode{
                "same", "tap.001.preload.same_requires_preload"},
            non_preloaded_mode{
                "reuse", "tap.001.preload.reuse_requires_preload"}};
        for (const non_preloaded_mode& scenario : scenarios) {
            DYNAMIC_SECTION("mode " << scenario.mode) {
                bioseq::postgres_test::migration_probe_scope mode_probes{
                    "cache install, preload diagnostics, and ACLs are native",
                    "cache-required modes reject a non-preloaded postmaster",
                    std::string(scenario.mode)};
                bool rejected = false;
                std::string sqlstate;
                std::string diagnostic;
                std::string hint;
                try {
                    bioseq::cache_test::exec(
                        connection,
                        "SET bioseq.query_cache_optimisation_mode = "
                            + connection.escape_literal(scenario.mode));
                } catch (const bioseq::libpq::query_error& error) {
                    rejected = true;
                    sqlstate = error.sqlstate();
                    diagnostic = error.what();
                    hint = error.hint();
                }
                REQUIRE(rejected);
                CHECK(sqlstate == "22023");
                CHECK(diagnostic
                      == "bioseq_cache shared memory is not registered");
                CHECK(hint.find("shared_preload_libraries")
                      != std::string::npos);
                CHECK(hint.find("restart PostgreSQL")
                      != std::string::npos);
                bioseq::postgres_test::record_migration_probe(
                    scenario.probe);
                bioseq::postgres_test::record_migration_probe(
                    "tap.001.preload.restart_hint");
                bioseq::postgres_test::record_migration_probe(
                    "tap.001.preload.sqlstate_and_message");
            }
        }
    }

    SECTION("preload, catalog identity, and diagnostics ACLs") {
        bioseq::postgres_test::postgres_fixture fixture{
            bioseq::cache_test::cluster_options(
                false, bioseq::cache_test::preload_order::cache_only,
                {{"bioseq.query_cache_optimisation_mode", "same"}})};
        auto& connection = fixture.admin();

        CHECK(bioseq::cache_test::scalar(
                  connection,
                  "SHOW bioseq.query_cache_optimisation_mode")
              == "same");
        CHECK_FALSE(bioseq::cache_test::scalar(
                        connection, "SELECT bioseq_cache.version()")
                        .empty());
        CHECK_FALSE(bioseq::cache_test::boolean(
            connection,
            "SELECT has_schema_privilege("
            "'public', 'bioseq_cache', 'CREATE')"));
        CHECK(bioseq::cache_test::integer(
                  connection,
                  "SELECT count(*) "
                  "FROM pg_class AS c "
                  "JOIN pg_namespace AS n ON n.oid = c.relnamespace "
                  "WHERE n.nspname = 'bioseq_cache' "
                  "AND c.relkind IN ('r', 'p')")
              == 0);
        bioseq::postgres_test::record_migration_probe(
            "tap.001.preload.startup_placeholder_replayed");
        bioseq::postgres_test::record_migration_probe(
            "tap.001.preload.version_exposed");
        bioseq::postgres_test::record_migration_probe(
            "tap.001.preload.schema_not_publicly_writable");
        bioseq::postgres_test::record_migration_probe(
            "tap.001.preload.no_metadata_table");

        bioseq::cache_test::exec(
            connection,
            "CREATE ROLE cache_monitor_granted; "
            "CREATE ROLE cache_monitor_execute_only; "
            "GRANT EXECUTE ON FUNCTION bioseq_cache.entries() "
            "TO cache_monitor_granted, cache_monitor_execute_only; "
            "GRANT USAGE ON SCHEMA bioseq_cache "
            "TO cache_monitor_granted; "
            "CREATE VIEW public.cache_entries_count "
            "WITH (security_invoker = true) "
            "AS SELECT count(*) AS entry_count "
            "FROM bioseq_cache.entries(); "
            "GRANT SELECT ON public.cache_entries_count "
            "TO cache_monitor_granted, cache_monitor_execute_only");

        bioseq::cache_test::exec(connection,
                                "SET ROLE cache_monitor_granted");
        CHECK(bioseq::cache_test::integer(
                  connection,
                  "SELECT entry_count "
                  "FROM public.cache_entries_count")
              >= 0);
        bioseq::cache_test::exec(connection, "RESET ROLE");
        bioseq::postgres_test::record_migration_probe(
            "tap.001.preload.explicit_acl_passes_boundary");

        bool denied = false;
        std::string denial;
        std::string denial_hint;
        try {
            static_cast<void>(connection.exec(
                "SET ROLE cache_monitor_execute_only; "
                "SELECT entry_count FROM public.cache_entries_count"));
        } catch (const bioseq::libpq::query_error& error) {
            denied = true;
            denial = error.what();
            denial_hint = error.hint();
        }
        CHECK(denied);
        CHECK(denial.find("permission denied to inspect bioseq_cache")
              != std::string::npos);
        CHECK(denial_hint.find("USAGE") != std::string::npos);
        bioseq::cache_test::exec(connection, "RESET ROLE");
        bioseq::postgres_test::record_migration_probe(
            "tap.001.preload.stored_oid_cannot_bypass_acl");
        bioseq::postgres_test::record_migration_probe(
            "tap.001.preload.denial_documents_two_part_grant");
    }
}

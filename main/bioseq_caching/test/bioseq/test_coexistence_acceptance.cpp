#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

namespace {

constexpr std::string_view acceptance_case{
    "coexistence executes Bioseq correctly across creation and cache modes"};
constexpr std::string_view acceptance_query{
    "SELECT id, "
    "bioseq.target_start(id), bioseq.target_end(id), "
    "bioseq.target_len(id), "
    "bioseq.query_start(id), bioseq.query_end(id), "
    "bioseq.query_len(id), bioseq.is_reverse(id) "
    "FROM cache_coexistence_acceptance "
    "WHERE mmsearch(target, query) "
    "ORDER BY id, bioseq.target_start(id), bioseq.target_end(id), "
    "bioseq.target_len(id), bioseq.query_start(id), "
    "bioseq.query_end(id), bioseq.query_len(id), "
    "bioseq.is_reverse(id)"};

void configure_cpu(bioseq::libpq::connection& connection) {
    bioseq::cache_test::exec(
        connection,
        "SET bioseq.aligner.vulkan_enabled = off; "
        "SET bioseq.aligner.vulkan_seed_hits_enabled = off; "
        "SET bioseq.aligner.vulkan_index_cache_enabled = off; "
        "SET bioseq.aligner.vulkan_fused_frontend_enabled = off; "
        "SET bioseq.aligner.vulkan_gpu_index_build_enabled = off; "
        "SET bioseq.aligner.vulkan_extend_enabled = off; "
        "SET bioseq.read_sequence_index = on");
}

void create_nonempty_fixture(bioseq::libpq::connection& connection) {
    configure_cpu(connection);
    bioseq::cache_test::exec(
        connection,
        "SET bioseq.update_index_on_insert = off; "
        "SELECT bioseq.create_query_target_sample_table("
        "'cache_coexistence_acceptance', 2, 100000, 5000.0, 50000000, "
        "0.001, 0.0005, 0.0005, 1.0, 1.0, 0.5, 3219876)",
        5min);
    bioseq::cache_test::exec(
        connection,
        "SELECT bioseq.update_index("
        "'cache_coexistence_acceptance'::regclass, "
        "'minimizers_per_fragment'); "
        "SELECT bioseq.set_active_read_index("
        "'cache_coexistence_acceptance'::regclass, "
        "'minimizers_per_fragment'); "
        "SET bioseq.read_sequence_index = on",
        5min);
}

void check_one_producer(
    const bioseq::cache_test::cache_stats& delta) {
    CHECK(delta.produced == 1);
    CHECK(delta.completed_hits == 0);
    CHECK(delta.inflight_waits == 0);
    CHECK(delta.reuse_hits == 0);
    CHECK(delta.fallbacks == 0);
}

void check_one_hit(const bioseq::cache_test::cache_stats& delta) {
    CHECK(delta.produced == 0);
    CHECK(delta.completed_hits == 1);
    CHECK(delta.inflight_waits == 0);
    CHECK(delta.reuse_hits == 0);
    CHECK(delta.fallbacks == 0);
}

}  // namespace

TEST_CASE(
    "coexistence executes Bioseq correctly across creation and cache modes",
    "[cache][integration][bioseq][coexistence][acceptance]") {
    SECTION("both SQL CREATE orders produce and hit") {
        struct creation_scenario {
            bool create_bioseq_first;
            std::string_view name;
        };
        constexpr std::array scenarios{
            creation_scenario{false, "cache_then_bioseq"},
            creation_scenario{true, "bioseq_then_cache"}};

        for (const creation_scenario& scenario : scenarios) {
            INFO("SQL CREATE extension order: " << scenario.name);
            bioseq::postgres_test::migration_probe_scope probes{
                std::string(acceptance_case),
                "both SQL CREATE orders produce and hit",
                std::string(scenario.name)};
            bioseq::postgres_test::postgres_fixture fixture{
                bioseq::cache_test::cluster_options(
                    true,
                    bioseq::cache_test::preload_order::cache_then_bioseq,
                    {}, scenario.create_bioseq_first)};
            auto& admin = fixture.admin();
            auto data = fixture.connect(30s);
            create_nonempty_fixture(admin);
            configure_cpu(data);

            bioseq::cache_test::exec(
                data,
                "SET bioseq.query_cache_optimisation_mode = 'disabled'");
            const auto oracle = data.exec(acceptance_query, 5min);
            REQUIRE(oracle.rows() > 0);
            const auto oracle_digest =
                bioseq::postgres_test::exact_row_hash(oracle);
            bioseq::postgres_test::record_migration_probe(
                "combined.coexistence.create_order_disabled_oracle_nonempty");

            bioseq::cache_test::exec(
                admin,
                "SELECT bioseq_cache.invalidate_all(); "
                "SELECT bioseq_cache.reset_stats()");
            bioseq::cache_test::exec(
                data,
                "SET bioseq.query_cache_optimisation_mode = 'same'");
            const auto before =
                bioseq::cache_test::read_cache_stats(admin);
            const auto cold = data.exec(acceptance_query, 5min);
            const auto after_cold =
                bioseq::cache_test::read_cache_stats(admin);
            const auto warm = data.exec(acceptance_query, 5min);
            const auto after_warm =
                bioseq::cache_test::read_cache_stats(admin);

            REQUIRE(
                bioseq::postgres_test::exact_row_hash(cold)
                == oracle_digest);
            REQUIRE(
                bioseq::postgres_test::exact_row_hash(warm)
                == oracle_digest);
            check_one_producer(after_cold - before);
            check_one_hit(after_warm - after_cold);
            bioseq::postgres_test::record_migration_probe(
                "combined.coexistence.create_order_cold_identity");
            bioseq::postgres_test::record_migration_probe(
                "combined.coexistence.create_order_warm_exact_hit");
        }
    }

    SECTION("Bioseq-only query returns a nonempty result") {
        bioseq::postgres_test::migration_probe_scope probes{
            std::string(acceptance_case),
            "Bioseq-only query returns a nonempty result"};
        const auto& configuration = bioseq::cache_test::configuration();
        REQUIRE(configuration.bioseq_control.has_value());
        REQUIRE(configuration.bioseq_install_sql.has_value());
        REQUIRE(configuration.bioseq_dso.has_value());
        bioseq::postgres_test::postgres_fixture fixture{
            bioseq::postgres_test::cluster_options{
                .postgres_bin_dir = configuration.postgres_bin_dir,
                .extensions = {
                    bioseq::postgres_test::extension_artifact{
                        .name = "bioseq",
                        .control_file = *configuration.bioseq_control,
                        .install_sql = *configuration.bioseq_install_sql,
                        .dso = *configuration.bioseq_dso}},
                .preload_libraries = {"bioseq"},
                .startup_gucs = {},
                .startup_timeout = 60s,
                .stop_timeout = 30s}};
        auto& connection = fixture.admin();
        create_nonempty_fixture(connection);
        const auto result = connection.exec(acceptance_query, 5min);
        REQUIRE(result.rows() > 0);
        bioseq::postgres_test::record_migration_probe(
            "combined.coexistence.bioseq_only_nonempty_query");
    }

    SECTION("disabled cache mode executes normally without preload") {
        bioseq::postgres_test::migration_probe_scope probes{
            std::string(acceptance_case),
            "disabled cache mode executes normally without preload"};
        bioseq::postgres_test::postgres_fixture fixture{
            bioseq::cache_test::cluster_options(
                true, bioseq::cache_test::preload_order::none)};
        auto& connection = fixture.admin();
        create_nonempty_fixture(connection);
        bioseq::cache_test::exec(
            connection,
            "SET bioseq.query_cache_optimisation_mode = 'disabled'");
        CHECK(
            bioseq::cache_test::scalar(
                connection,
                "SHOW bioseq.query_cache_optimisation_mode")
            == "disabled");
        const auto result = connection.exec(acceptance_query, 5min);
        REQUIRE(result.rows() > 0);
        bioseq::postgres_test::record_migration_probe(
            "combined.coexistence.disabled_without_preload_nonempty_query");
    }
}

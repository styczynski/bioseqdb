#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

namespace {

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

void configure_cpu(bioseq::libpq::client_connection& connection) {
    bioseq::cache_test::execute(
        connection,
        "SET bioseq.aligner.vulkan.enabled = off; "
        "SET bioseq.aligner.vulkan.seed_lookup.enabled = off; "
        "SET bioseq.aligner.vulkan.index_cache.enabled = off; "
        "SET bioseq.aligner.vulkan.fused_frontend.enabled = off; "
        "SET bioseq.aligner.vulkan.index_build.enabled = off; "
        "SET bioseq.aligner.vulkan.alignment_extension.enabled = off; "
        "SET bioseq.index.sequence_read.enabled = on");
}

void create_nonempty_fixture(bioseq::libpq::client_connection& connection) {
    configure_cpu(connection);
    bioseq::cache_test::execute(
        connection,
        "SET bioseq.index.insert_maintenance.enabled = off; "
        "SELECT bioseq.create_query_target_sample_table("
        "'cache_coexistence_acceptance', 2, 100000, 5000.0, 50000000, "
        "0.001, 0.0005, 0.0005, 1.0, 1.0, 0.5, 3219876)",
        5min);
    bioseq::cache_test::execute(
        connection,
        "SELECT bioseq.update_index("
        "'cache_coexistence_acceptance'::regclass, "
        "'minimizers_per_fragment'); "
        "SELECT bioseq.set_active_read_index("
        "'cache_coexistence_acceptance'::regclass, "
        "'minimizers_per_fragment'); "
        "SET bioseq.index.sequence_read.enabled = on",
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
    "[component:cache][layer:integration]"
          "[feature:provider][resource:postgres]") {
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
            bioseq::postgres_test::private_cluster_fixture fixture{
                bioseq::cache_test::cluster_options(
                    true,
                    bioseq::cache_test::preload_order::cache_then_bioseq,
                    {}, scenario.create_bioseq_first)};
            auto& admin = fixture.admin();
            auto data = fixture.connect(30s);
            create_nonempty_fixture(admin);
            configure_cpu(data);

            bioseq::cache_test::execute(
                data,
                "SET bioseq_cache.query_cache.mode = 'disabled'");
            const auto oracle = data.execute(acceptance_query, 5min);
            REQUIRE(oracle.row_count() > 0);
            const auto oracle_digest =
                bioseq::postgres_test::hash_ordered_result_rows(oracle);

            bioseq::cache_test::execute(
                admin,
                "SELECT bioseq_cache.invalidate_all(); "
                "SELECT bioseq_cache.reset_stats()");
            bioseq::cache_test::execute(
                data,
                "SET bioseq_cache.query_cache.mode = 'exact_query'");
            const auto before =
                bioseq::cache_test::read_cache_stats(admin);
            const auto cold = data.execute(acceptance_query, 5min);
            const auto after_cold =
                bioseq::cache_test::read_cache_stats(admin);
            const auto warm = data.execute(acceptance_query, 5min);
            const auto after_warm =
                bioseq::cache_test::read_cache_stats(admin);

            REQUIRE(
                bioseq::postgres_test::hash_ordered_result_rows(cold)
                == oracle_digest);
            REQUIRE(
                bioseq::postgres_test::hash_ordered_result_rows(warm)
                == oracle_digest);
            check_one_producer(after_cold - before);
            check_one_hit(after_warm - after_cold);
        }
    }

    SECTION("Bioseq-only query returns a nonempty result") {
        const auto& configuration = bioseq::cache_test::configuration();
        REQUIRE(configuration.bioseq_control.has_value());
        REQUIRE(configuration.bioseq_install_sql.has_value());
        REQUIRE(configuration.bioseq_dso.has_value());
        auto options =
            bioseq::cache_test::bioseq_only_cluster_options();
        REQUIRE(options.extensions.size() == 1);
        CHECK(options.extensions.front().name == "bioseq");
        REQUIRE(options.preload_libraries.size() == 1);
        CHECK(options.preload_libraries.front() == "bioseq");
        CHECK(options.allowed_runtime_dso_overrides.empty());
        bioseq::postgres_test::private_cluster_fixture fixture{
            std::move(options)};
        const auto& stage = fixture.cluster().staged_artifacts();
        REQUIRE(stage.extensions.size() == 1);
        CHECK(stage.extensions.front().name == "bioseq");
        const auto cache_paths =
            bioseq::cache_test::canonical_cache_stage_artifact_paths(stage);
        CHECK_FALSE(std::filesystem::exists(cache_paths[0]));
        CHECK_FALSE(std::filesystem::is_symlink(cache_paths[0]));
        CHECK_FALSE(std::filesystem::exists(cache_paths[1]));
        CHECK_FALSE(std::filesystem::is_symlink(cache_paths[1]));
        CHECK_FALSE(std::filesystem::exists(cache_paths[2]));
        CHECK_FALSE(std::filesystem::is_symlink(cache_paths[2]));
        auto& connection = fixture.admin();
        create_nonempty_fixture(connection);
        const auto result = connection.execute(acceptance_query, 5min);
        REQUIRE(result.row_count() > 0);
    }

    SECTION("disabled cache mode executes normally without preload") {
        bioseq::postgres_test::private_cluster_fixture fixture{
            bioseq::cache_test::cluster_options(
                true, bioseq::cache_test::preload_order::none)};
        auto& connection = fixture.admin();
        create_nonempty_fixture(connection);
        bioseq::cache_test::execute(
            connection,
            "SET bioseq_cache.query_cache.mode = 'disabled'");
        CHECK(
            bioseq::cache_test::scalar(
                connection,
                "SHOW bioseq_cache.query_cache.mode")
            == "disabled");
        const auto result = connection.execute(acceptance_query, 5min);
        REQUIRE(result.row_count() > 0);
    }
}

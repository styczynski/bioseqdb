#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

namespace {

struct coexistence_scenario {
    bioseq::cache_test::preload_order preload;
    bool create_bioseq_first;
    std::string_view name;
};

[[nodiscard]] bool contains_requires(
    const std::filesystem::path& control_file) {
    std::ifstream input(control_file);
    std::string line;
    while (std::getline(input, line)) {
        const auto first = line.find_first_not_of(" \t");
        if (first != std::string::npos
            && line.substr(first).starts_with("requires")) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("Bioseq and cache install and preload independently in either order",
          "[cache][integration][bioseq][coexistence][load-order]") {
    constexpr std::array<coexistence_scenario, 4> scenarios{{
        {bioseq::cache_test::preload_order::cache_then_bioseq,
         false, "preload-cache-first-create-cache-first"},
        {bioseq::cache_test::preload_order::cache_then_bioseq,
         true, "preload-cache-first-create-bioseq-first"},
        {bioseq::cache_test::preload_order::bioseq_then_cache,
         false, "preload-bioseq-first-create-cache-first"},
        {bioseq::cache_test::preload_order::bioseq_then_cache,
         true, "preload-bioseq-first-create-bioseq-first"},
    }};
    for (const auto& scenario : scenarios) {
        bioseq::postgres_test::migration_probe_scope probes{
            "Bioseq and cache install and preload independently in either order",
            "load and create order", std::string(scenario.name)};
        bioseq::postgres_test::postgres_fixture fixture{
            bioseq::cache_test::cluster_options(
                true, scenario.preload, {}, scenario.create_bioseq_first)};
        auto& connection = fixture.admin();

        CHECK(bioseq::cache_test::integer(
                  connection,
                  "SELECT count(*) FROM pg_extension "
                  "WHERE extname IN ('bioseq', 'bioseq_cache')")
              == 2);
        CHECK_FALSE(bioseq::cache_test::scalar(
                        connection, "SELECT bioseq_cache.version()")
                        .empty());
        CHECK(bioseq::cache_test::integer(
                  connection,
                  "SELECT count(*) "
                  "FROM pg_type AS t "
                  "JOIN pg_namespace AS n ON n.oid = t.typnamespace "
                  "WHERE n.nspname = 'bioseq' AND t.typname = 'seq'")
              == 1);
        CHECK(bioseq::cache_test::integer(
                  connection,
                  "SELECT count(*) "
                  "FROM pg_depend AS d "
                  "JOIN pg_extension AS child "
                  "  ON d.classid = 'pg_extension'::regclass "
                  " AND d.objid = child.oid "
                  "JOIN pg_extension AS parent "
                  "  ON d.refclassid = 'pg_extension'::regclass "
                  " AND d.refobjid = parent.oid "
                  "WHERE child.extname IN ('bioseq', 'bioseq_cache') "
                  "AND parent.extname IN ('bioseq', 'bioseq_cache') "
                  "AND child.oid <> parent.oid")
              == 0);
        bioseq::postgres_test::record_migration_probe(
            "combined.coexistence.two_extension_rows");
        bioseq::postgres_test::record_migration_probe(
            "combined.coexistence.cache_api_available");
        bioseq::postgres_test::record_migration_probe(
            "combined.coexistence.bioseq_type_available");
        bioseq::postgres_test::record_migration_probe(
            "combined.coexistence.no_extension_dependency");
    }

    const auto& configuration = bioseq::cache_test::configuration();
    REQUIRE(configuration.bioseq_control.has_value());
    CHECK_FALSE(contains_requires(configuration.cache_control));
    CHECK_FALSE(contains_requires(*configuration.bioseq_control));
    bioseq::postgres_test::migration_probe_scope controls{
        "Bioseq and cache install and preload independently in either order",
        "control metadata"};
    bioseq::postgres_test::record_migration_probe(
        "combined.coexistence.control_files_have_no_requires");
}

TEST_CASE("Bioseq-only server does not require the cache DSO",
          "[cache][integration][bioseq][coexistence][bioseq-only]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "Bioseq-only server does not require the cache DSO"};
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
    bioseq::postgres_test::postgres_fixture fixture{std::move(options)};
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
    CHECK(bioseq::cache_test::integer(
              connection,
              "SELECT count(*) FROM pg_extension "
              "WHERE extname = 'bioseq'")
          == 1);
    CHECK(bioseq::cache_test::integer(
              connection,
              "SELECT count(*) FROM pg_extension "
              "WHERE extname = 'bioseq_cache'")
          == 0);
    bioseq::postgres_test::record_migration_probe(
        "combined.coexistence.bioseq_only_create_extension");
}

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <string>
#include <string_view>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

namespace {

constexpr std::string_view small_query{
    "SELECT id, "
    "bioseq.target_start(id), bioseq.target_end(id), "
    "bioseq.target_len(id), "
    "bioseq.query_start(id), bioseq.query_end(id), "
    "bioseq.query_len(id), bioseq.is_reverse(id) "
    "FROM cache_bioseq_small "
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
        "SET bioseq.aligner.vulkan_extend_enabled = off");
}

void create_small_fixture(bioseq::libpq::connection& connection) {
    configure_cpu(connection);
    bioseq::cache_test::exec(
        connection,
        "SET bioseq.update_index_on_insert = off; "
        "SELECT bioseq.create_query_target_sample_table("
        "'cache_bioseq_small', 2, 100000, 5000.0, 50000000, "
        "0.001, 0.0005, 0.0005, 1.0, 1.0, 0.5, 3219876)",
        5min);
    bioseq::cache_test::exec(
        connection,
        "SELECT bioseq.update_index("
        "'cache_bioseq_small'::regclass, "
        "'minimizers_per_fragment'); "
        "SELECT bioseq.set_active_read_index("
        "'cache_bioseq_small'::regclass, "
        "'minimizers_per_fragment'); "
        "SET bioseq.read_sequence_index = on",
        5min);
}

}  // namespace

TEST_CASE("BioseqScan exact provider produces and hits in same and reuse modes",
          "[cache][integration][bioseq][provider][exact]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "BioseqScan exact provider produces and hits in same and reuse modes"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            true, bioseq::cache_test::preload_order::cache_then_bioseq)};
    auto& admin = fixture.admin();
    auto data = fixture.connect(30s);
    create_small_fixture(admin);
    configure_cpu(data);

    bioseq::cache_test::exec(
        admin,
        "SET bioseq.query_cache_optimisation_mode = 'disabled'");
    bioseq::cache_test::exec(
        data,
        "SET bioseq.query_cache_optimisation_mode = 'disabled'; "
        "SET bioseq.read_sequence_index = on");
    const auto oracle = data.exec(small_query, 5min);
    REQUIRE(oracle.rows() > 0);
    const auto oracle_digest =
        bioseq::postgres_test::exact_row_hash(oracle);
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_provider.disabled_oracle_nonempty");

    for (const std::string_view mode : {"same", "reuse"}) {
        bioseq::postgres_test::migration_probe_scope mode_probes{
            "BioseqScan exact provider produces and hits in same and reuse modes",
            "exact mode", std::string(mode)};
        bioseq::cache_test::exec(
            admin,
            "SELECT bioseq_cache.invalidate_all(); "
            "SELECT bioseq_cache.reset_stats()");
        bioseq::cache_test::exec(
            data,
            "SET bioseq.query_cache_optimisation_mode = "
                + data.escape_literal(mode));

        const auto before = bioseq::cache_test::read_cache_stats(admin);
        const auto cold = data.exec(small_query, 5min);
        const auto after_cold =
            bioseq::cache_test::read_cache_stats(admin);
        const auto child_executions_after_cold =
            bioseq::cache_test::integer(
                admin,
                "SELECT exact_child_executions "
                "FROM bioseq_cache_test.runtime_counters()");
        const auto warm = data.exec(small_query, 5min);
        const auto after_warm =
            bioseq::cache_test::read_cache_stats(admin);
        const auto child_executions_after_warm =
            bioseq::cache_test::integer(
                admin,
                "SELECT exact_child_executions "
                "FROM bioseq_cache_test.runtime_counters()");
        CHECK(bioseq::postgres_test::exact_row_hash(cold)
              == oracle_digest);
        CHECK(bioseq::postgres_test::exact_row_hash(warm)
              == oracle_digest);
        CHECK((after_cold - before).produced == 1);
        CHECK((after_cold - before).fallbacks == 0);
        CHECK((after_warm - after_cold).completed_hits == 1);
        CHECK((after_warm - after_cold).reuse_hits == 0);
        CHECK((after_warm - after_cold).fallbacks == 0);
        CHECK(child_executions_after_cold == 1);
        CHECK(child_executions_after_warm
              == child_executions_after_cold);
        bioseq::postgres_test::record_migration_probe(
            "combined.bioseq_provider.cold_identity");
        bioseq::postgres_test::record_migration_probe(
            "combined.bioseq_provider.warm_exact_hit");

        const auto explain = bioseq::postgres_test::run_explain(
            data, small_query, true, 5min);
        const auto cache_nodes = explain.find_plan_nodes(
            "Custom Plan Provider", "BioseqCacheScan");
        REQUIRE(cache_nodes.size() == 1);
        CHECK(bioseq::postgres_test::explain_property_string(
                  *cache_nodes.front(), "Actual Cache Action")
              == "[Hit] exact final result");
        bioseq::postgres_test::record_migration_probe(
            "combined.bioseq_provider.explain_exact_hit");
    }
}

TEST_CASE("performance GUCs preserve Bioseq exact-cache identity",
          "[cache][integration][bioseq][provider][guc-matrix]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "performance GUCs preserve Bioseq exact-cache identity"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            true, bioseq::cache_test::preload_order::bioseq_then_cache)};
    auto& admin = fixture.admin();
    auto data = fixture.connect(30s);
    create_small_fixture(admin);
    configure_cpu(data);
    bioseq::cache_test::exec(
        admin,
        "SET bioseq.query_cache_optimisation_mode = 'disabled'; "
        "SELECT bioseq_cache.invalidate_all(); "
        "SELECT bioseq_cache.reset_stats()");
    bioseq::cache_test::exec(
        data,
        "SET bioseq.query_cache_optimisation_mode = 'same'; "
        "SET bioseq.enable_column_cache = on; "
        "SET bioseq.enable_vectorization = on; "
        "SET bioseq.read_sequence_index = on");
    const auto producer = data.exec(small_query, 5min);
    REQUIRE(producer.rows() > 0);
    const auto digest =
        bioseq::postgres_test::exact_row_hash(producer);

    bioseq::cache_test::exec(
        data, "SET bioseq.enable_column_cache = off");
    const auto no_column_cache = data.exec(small_query, 5min);
    CHECK(bioseq::postgres_test::exact_row_hash(no_column_cache) == digest);
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_provider.column_cache_excluded_from_key");

    bioseq::cache_test::exec(
        data, "SET bioseq.enable_vectorization = off");
    const auto no_vectorization = data.exec(small_query, 5min);
    CHECK(bioseq::postgres_test::exact_row_hash(no_vectorization) == digest);
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_provider.vectorization_excluded_from_key");

    const auto before_semantic_change =
        bioseq::cache_test::read_cache_stats(admin);
    bioseq::cache_test::exec(
        data, "SET bioseq.read_sequence_index = off");
    const auto no_read_index = data.exec(small_query, 5min);
    const auto after_semantic_change =
        bioseq::cache_test::read_cache_stats(admin);
    CHECK(bioseq::postgres_test::exact_row_hash(no_read_index) == digest);
    CHECK((after_semantic_change - before_semantic_change).produced == 1);
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_provider.read_index_separates_semantic_key");
}

TEST_CASE("disabling Bioseq custom scans bypasses sibling caching safely",
          "[cache][integration][bioseq][provider][custom-scan]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "disabling Bioseq custom scans bypasses sibling caching safely"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            true, bioseq::cache_test::preload_order::cache_then_bioseq)};
    auto& connection = fixture.admin();
    create_small_fixture(connection);
    bioseq::cache_test::exec(
        connection,
        "SET bioseq.query_cache_optimisation_mode = 'same'; "
        "SET bioseq.enable_custom_scan = off; "
        "SELECT bioseq_cache.reset_stats()");
    const auto result = connection.exec(small_query, 5min);
    REQUIRE(result.rows() > 0);
    const auto plan = bioseq::postgres_test::run_explain(
        connection, small_query, false, 5min);
    CHECK(plan
              .find_plan_nodes("Custom Plan Provider", "BioseqCacheScan")
              .empty());
    CHECK(bioseq::cache_test::read_cache_stats(connection).produced == 0);
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_provider.custom_scan_off_bypasses");
}

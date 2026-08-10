#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
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

constexpr std::string_view batch_join_query{
    "SELECT target_sample.id, query_sample.id "
    "FROM cache_bioseq_small AS target_sample "
    "JOIN cache_bioseq_small AS query_sample "
    "  ON target_sample.id = query_sample.id "
    " AND mmsearch(target_sample.target, query_sample.query) "
    "ORDER BY target_sample.id, query_sample.id"};

struct preload_scenario {
    bioseq::cache_test::preload_order order;
};

struct semantic_guc_scenario {
    std::string_view set_a;
    std::string_view set_b;
    bool result_identity_expected;
};

void configure_cpu(bioseq::libpq::client_connection& connection) {
    bioseq::cache_test::execute(
        connection,
        "SET bioseq.aligner.vulkan.enabled = off; "
        "SET bioseq.aligner.vulkan.seed_lookup.enabled = off; "
        "SET bioseq.aligner.vulkan.index_cache.enabled = off; "
        "SET bioseq.aligner.vulkan.fused_frontend.enabled = off; "
        "SET bioseq.aligner.vulkan.index_build.enabled = off; "
        "SET bioseq.aligner.vulkan.alignment_extension.enabled = off");
}

void create_small_fixture(bioseq::libpq::client_connection& connection) {
    configure_cpu(connection);
    bioseq::cache_test::execute(
        connection,
        "SET bioseq.index.insert_maintenance.enabled = off; "
        "SELECT bioseq.create_query_target_sample_table("
        "'cache_bioseq_small', 2, 100000, 5000.0, 50000000, "
        "0.001, 0.0005, 0.0005, 1.0, 1.0, 0.5, 3219876)",
        5min);
    bioseq::cache_test::execute(
        connection,
        "SELECT bioseq.update_index("
        "'cache_bioseq_small'::regclass, "
        "'minimizers_per_fragment'); "
        "SELECT bioseq.set_active_read_index("
        "'cache_bioseq_small'::regclass, "
        "'minimizers_per_fragment'); "
        "SET bioseq.index.sequence_read.enabled = on",
        5min);
}

[[nodiscard]] std::int64_t exact_child_executions(
    bioseq::libpq::client_connection& admin) {
    return bioseq::cache_test::integer(
        admin,
        "SELECT bioseq_cache_test.exact_child_executions()");
}

void check_one_exact_producer(
    const bioseq::cache_test::cache_stats& delta) {
    CHECK(delta.produced == 1);
    CHECK(delta.completed_hits == 0);
    CHECK(delta.inflight_waits == 0);
    CHECK(delta.reuse_hits == 0);
    CHECK(delta.fallbacks == 0);
}

void check_one_completed_exact_hit(
    const bioseq::cache_test::cache_stats& delta) {
    CHECK(delta.produced == 0);
    CHECK(delta.completed_hits == 1);
    CHECK(delta.inflight_waits == 0);
    CHECK(delta.reuse_hits == 0);
    CHECK(delta.fallbacks == 0);
}

void check_batch_join_exact_provider(
    bioseq::libpq::client_connection& admin,
    bioseq::libpq::client_connection& data) {
    bioseq::cache_test::execute(
        data,
        "SET bioseq_cache.query_cache.mode = 'disabled'");
    const auto oracle = data.execute(batch_join_query, 5min);
    REQUIRE(oracle.row_count() > 0);
    const auto oracle_digest =
        bioseq::postgres_test::hash_ordered_result_rows(oracle);
    const auto uncached_plan = bioseq::postgres_test::execute_explain(
        data, batch_join_query, false, 5min);
    REQUIRE(
        uncached_plan.find_plan_nodes(
            "Custom Plan Provider", "BioseqBatchJoin").size()
        == 1);

    bioseq::cache_test::execute(
        admin,
        "SELECT bioseq_cache.invalidate_all(); "
        "SELECT bioseq_cache.reset_stats()");
    bioseq::cache_test::execute(
        data,
        "SET bioseq_cache.query_cache.mode = 'exact_query'");
    const auto cacheable_plan = bioseq::postgres_test::execute_explain(
        data, batch_join_query, false, 5min);
    const auto planned_cache_nodes = cacheable_plan.find_plan_nodes(
        "Custom Plan Provider", "BioseqCacheScan");
    REQUIRE(planned_cache_nodes.size() == 1);
    REQUIRE(bioseq::postgres_test::read_plan_boolean_property(
                *planned_cache_nodes.front(), "Cache Force Bypass")
            == false);
    REQUIRE(bioseq::postgres_test::read_plan_integer_property(
                *planned_cache_nodes.front(),
                "Provider Runtime Recipes")
            == 3);
    REQUIRE(
        cacheable_plan.find_plan_nodes(
            "Custom Plan Provider", "BioseqBatchJoin").size()
        == 1);
    const auto before = bioseq::cache_test::read_cache_stats(admin);
    const auto child_before = exact_child_executions(admin);
    const auto cold = data.execute(batch_join_query, 5min);
    const auto after_cold = bioseq::cache_test::read_cache_stats(admin);
    const auto child_after_cold = exact_child_executions(admin);
    const auto warm = data.execute(batch_join_query, 5min);
    const auto after_warm = bioseq::cache_test::read_cache_stats(admin);
    const auto child_after_warm = exact_child_executions(admin);

    REQUIRE(bioseq::postgres_test::hash_ordered_result_rows(cold) == oracle_digest);
    REQUIRE(bioseq::postgres_test::hash_ordered_result_rows(warm) == oracle_digest);
    check_one_exact_producer(after_cold - before);
    check_one_completed_exact_hit(after_warm - after_cold);
    CHECK(child_after_cold == child_before + 1);
    CHECK(child_after_warm == child_after_cold);

    const auto warm_explain = bioseq::postgres_test::execute_explain(
        data, batch_join_query, true, 5min);
    const auto cache_nodes = warm_explain.find_plan_nodes(
        "Custom Plan Provider", "BioseqCacheScan");
    REQUIRE(cache_nodes.size() == 1);
    CHECK(bioseq::postgres_test::read_plan_string_property(
              *cache_nodes.front(), "Actual Cache Action")
          == "[Hit] exact final result");
    REQUIRE(
        warm_explain.find_plan_nodes(
            "Custom Plan Provider", "BioseqBatchJoin").empty());
    const bioseq::postgres_test::explain_json_value* children =
        cache_nodes.front()->find("Plans");
    if (children != nullptr) {
        REQUIRE(children->as_array() != nullptr);
        for (const auto& child : *children->as_array()) {
            CHECK(bioseq::postgres_test::read_plan_number_property(
                      child, "Actual Loops")
                  == 0.0);
        }
    }
    check_one_completed_exact_hit(
        bioseq::cache_test::read_cache_stats(admin) - after_warm);
    CHECK(exact_child_executions(admin) == child_after_warm);
}

[[nodiscard]] std::string query_with_aligner_options(
    std::string_view arguments) {
    constexpr std::string_view needle{"mmsearch(target, query)"};
    std::string query{small_query};
    const std::size_t offset = query.find(needle);
    if (offset == std::string::npos) {
        throw std::logic_error("small Bioseq query lost its mmsearch call");
    }
    query.replace(
        offset, needle.size(),
        "mmsearch(target, query, aligner_options("
            + std::string(arguments) + "))");
    return query;
}

void prepare_query(bioseq::libpq::client_connection& connection,
                   std::string_view name,
                   std::string_view query) {
    bioseq::cache_test::execute(
        connection,
        "PREPARE " + std::string(name) + " AS " + std::string(query));
}

[[nodiscard]] bioseq::libpq::command_result execute_prepared(
    bioseq::libpq::client_connection& connection,
    std::string_view name) {
    return connection.execute("EXECUTE " + std::string(name), 5min);
}

}  // namespace

TEST_CASE("BioseqScan exact provider produces and hits in exact-query and reusable-fragment modes",
          "[component:cache][layer:integration]"
          "[feature:provider][resource:postgres]") {
    constexpr std::array preload_scenarios{
        preload_scenario{
            bioseq::cache_test::preload_order::cache_then_bioseq},
        preload_scenario{
            bioseq::cache_test::preload_order::bioseq_then_cache}};
    constexpr std::array<std::string_view, 2> modes{
        "exact_query", "reusable_fragments"};
    std::optional<bioseq::postgres_test::sha256_digest>
        reference_oracle_digest;
    std::array<std::optional<bioseq::cache_test::cache_stats>, modes.size()>
        reference_cold_deltas;
    std::array<std::optional<bioseq::cache_test::cache_stats>, modes.size()>
        reference_warm_deltas;
    std::array<std::optional<bioseq::cache_test::cache_stats>, modes.size()>
        reference_explain_deltas;

    for (const preload_scenario& scenario : preload_scenarios) {
        bioseq::postgres_test::private_cluster_fixture fixture{
            bioseq::cache_test::cluster_options(true, scenario.order)};
        auto& admin = fixture.admin();
        auto data = fixture.connect(30s);
        create_small_fixture(admin);
        configure_cpu(data);

        bioseq::cache_test::execute(
            admin,
            "SET bioseq_cache.query_cache.mode = 'disabled'");
        bioseq::cache_test::execute(
            data,
            "SET bioseq_cache.query_cache.mode = 'disabled'; "
            "SET bioseq.index.sequence_read.enabled = on");
        const auto oracle = data.execute(small_query, 5min);
        REQUIRE(oracle.row_count() > 0);
        const auto oracle_digest =
            bioseq::postgres_test::hash_ordered_result_rows(oracle);
        if (reference_oracle_digest.has_value()) {
            REQUIRE(oracle_digest == *reference_oracle_digest);
        } else {
            reference_oracle_digest = oracle_digest;
        }

        for (std::size_t mode_index = 0;
             mode_index < modes.size(); ++mode_index) {
            const std::string_view mode = modes[mode_index];
            bioseq::cache_test::execute(
                admin,
                "SELECT bioseq_cache.invalidate_all(); "
                "SELECT bioseq_cache.reset_stats()");
            bioseq::cache_test::execute(
                data,
                "SET bioseq_cache.query_cache.mode = "
                    + data.escape_literal(mode));

            const auto before =
                bioseq::cache_test::read_cache_stats(admin);
            const auto child_before = exact_child_executions(admin);
            const auto cold = data.execute(small_query, 5min);
            const auto after_cold =
                bioseq::cache_test::read_cache_stats(admin);
            const auto child_after_cold = exact_child_executions(admin);
            const auto warm = data.execute(small_query, 5min);
            const auto after_warm =
                bioseq::cache_test::read_cache_stats(admin);
            const auto child_after_warm = exact_child_executions(admin);
            const auto cold_delta = after_cold - before;
            const auto warm_delta = after_warm - after_cold;
            CHECK(bioseq::postgres_test::hash_ordered_result_rows(cold)
                  == oracle_digest);
            CHECK(bioseq::postgres_test::hash_ordered_result_rows(warm)
                  == oracle_digest);
            check_one_exact_producer(cold_delta);
            check_one_completed_exact_hit(warm_delta);
            CHECK(child_after_cold == child_before + 1);
            CHECK(child_after_warm == child_after_cold);
            if (reference_cold_deltas[mode_index].has_value()) {
                REQUIRE(
                    cold_delta == *reference_cold_deltas[mode_index]);
                REQUIRE(
                    warm_delta == *reference_warm_deltas[mode_index]);
            } else {
                reference_cold_deltas[mode_index] = cold_delta;
                reference_warm_deltas[mode_index] = warm_delta;
            }

            const auto explain = bioseq::postgres_test::execute_explain(
                data, small_query, true, 5min);
            const auto cache_nodes = explain.find_plan_nodes(
                "Custom Plan Provider", "BioseqCacheScan");
            REQUIRE(cache_nodes.size() == 1);
            CHECK(bioseq::postgres_test::read_plan_string_property(
                      *cache_nodes.front(), "Actual Cache Action")
                  == "[Hit] exact final result");
            const bioseq::postgres_test::explain_json_value* children =
                cache_nodes.front()->find("Plans");
            if (children != nullptr) {
                REQUIRE(children->as_array() != nullptr);
                REQUIRE_FALSE(children->as_array()->empty());
                CHECK(bioseq::postgres_test::read_plan_number_property(
                          children->as_array()->front(), "Actual Loops")
                      == 0.0);
            }
            const auto after_explain =
                bioseq::cache_test::read_cache_stats(admin);
            const auto explain_delta = after_explain - after_warm;
            check_one_completed_exact_hit(explain_delta);
            CHECK(exact_child_executions(admin) == child_after_warm);
            if (reference_explain_deltas[mode_index].has_value()) {
                REQUIRE(
                    explain_delta
                    == *reference_explain_deltas[mode_index]);
            } else {
                reference_explain_deltas[mode_index] = explain_delta;
            }
        }
        check_batch_join_exact_provider(admin, data);
    }
}

TEST_CASE("prepared Bioseq plans freeze semantic keys and exclude performance GUCs",
          "[component:cache][layer:integration]"
          "[feature:provider][resource:postgres]") {
    bioseq::postgres_test::private_cluster_fixture fixture{
        bioseq::cache_test::cluster_options(
            true, bioseq::cache_test::preload_order::bioseq_then_cache)};
    auto& admin = fixture.admin();
    auto data = fixture.connect(30s);
    create_small_fixture(admin);
    configure_cpu(data);
    bioseq::cache_test::execute(
        admin,
        "SET bioseq_cache.query_cache.mode = 'disabled'");
    bioseq::cache_test::execute(
        data,
        "SET bioseq_cache.query_cache.mode = 'exact_query'; "
        "SET bioseq.index.sequence_read.enabled = on");

    constexpr std::array semantic_scenarios{
        semantic_guc_scenario{
            "SET bioseq.aligner.chaining.maximum_skipped_anchors = 25",
            "SET bioseq.aligner.chaining.maximum_skipped_anchors = 26",
            false},
        semantic_guc_scenario{
            "SET bioseq.aligner.secondary.overlap_threshold = 0.5",
            "SET bioseq.aligner.secondary.overlap_threshold = 0.6",
            false},
        semantic_guc_scenario{
            "SET bioseq.aligner.chaining.algorithm = 'scalar_minimap2'",
            "SET bioseq.aligner.chaining.algorithm = 'simd_minimap2'",
            false},
        semantic_guc_scenario{
            "SET bioseq.aligner.vulkan.index_cache.enabled = off",
            "SET bioseq.aligner.vulkan.index_cache.enabled = on",
            true},
        semantic_guc_scenario{
            "SET bioseq.index.sequence_read.enabled = on",
            "SET bioseq.index.sequence_read.enabled = off",
            true}};

    for (const semantic_guc_scenario& scenario : semantic_scenarios) {
        bioseq::cache_test::execute(
            data, "DEALLOCATE ALL; " + std::string(scenario.set_a));
        bioseq::cache_test::execute(
            admin,
            "SELECT bioseq_cache.invalidate_all(); "
            "SELECT bioseq_cache.reset_stats()");
        prepare_query(data, "bioseq_semantic_matrix", small_query);

        const auto before_a =
            bioseq::cache_test::read_cache_stats(admin);
        const auto child_before = exact_child_executions(admin);
        const auto result_a =
            execute_prepared(data, "bioseq_semantic_matrix");
        const auto after_a =
            bioseq::cache_test::read_cache_stats(admin);
        REQUIRE(result_a.row_count() > 0);
        const auto digest_a =
            bioseq::postgres_test::hash_ordered_result_rows(result_a);
        check_one_exact_producer(after_a - before_a);
        CHECK(exact_child_executions(admin) == child_before + 1);

        bioseq::cache_test::execute(data, scenario.set_b);
        const auto result_b =
            execute_prepared(data, "bioseq_semantic_matrix");
        const auto after_b =
            bioseq::cache_test::read_cache_stats(admin);
        REQUIRE(result_b.row_count() > 0);
        if (scenario.result_identity_expected) {
            CHECK(bioseq::postgres_test::hash_ordered_result_rows(result_b)
                  == digest_a);
        }
        check_one_exact_producer(after_b - after_a);
        CHECK(exact_child_executions(admin) == child_before + 2);

        bioseq::cache_test::execute(data, scenario.set_a);
        const auto result_a_again =
            execute_prepared(data, "bioseq_semantic_matrix");
        const auto after_a_again =
            bioseq::cache_test::read_cache_stats(admin);
        CHECK(bioseq::postgres_test::hash_ordered_result_rows(result_a_again)
              == digest_a);
        check_one_completed_exact_hit(after_a_again - after_b);
        CHECK(exact_child_executions(admin) == child_before + 2);
    }

    {
        bioseq::cache_test::execute(
            data,
            "DEALLOCATE ALL; "
            "SET bioseq.index.sequence_read.enabled = on");
        bioseq::cache_test::execute(
            admin,
            "SELECT bioseq_cache.invalidate_all(); "
            "SELECT bioseq_cache.reset_stats()");
        const std::string allow_query =
            query_with_aligner_options("index_read_requirement => 0");
        const std::string absent_query =
            query_with_aligner_options("index_read_requirement => 1");
        const std::string persisted_query =
            query_with_aligner_options("index_read_requirement => 2");
        prepare_query(data, "bioseq_index_allow", allow_query);
        prepare_query(data, "bioseq_index_absent", absent_query);
        prepare_query(data, "bioseq_index_persisted", persisted_query);

        const auto before_allow =
            bioseq::cache_test::read_cache_stats(admin);
        const auto child_before = exact_child_executions(admin);
        const auto allow =
            execute_prepared(data, "bioseq_index_allow");
        const auto after_allow =
            bioseq::cache_test::read_cache_stats(admin);
        REQUIRE(allow.row_count() > 0);
        const auto allow_digest =
            bioseq::postgres_test::hash_ordered_result_rows(allow);
        check_one_exact_producer(after_allow - before_allow);
        CHECK(exact_child_executions(admin) == child_before + 1);

        const auto absent =
            execute_prepared(data, "bioseq_index_absent");
        const auto after_absent =
            bioseq::cache_test::read_cache_stats(admin);
        REQUIRE(absent.row_count() > 0);
        CHECK(bioseq::postgres_test::hash_ordered_result_rows(absent)
              == allow_digest);
        check_one_exact_producer(after_absent - after_allow);
        CHECK(exact_child_executions(admin) == child_before + 2);

        const auto allow_again =
            execute_prepared(data, "bioseq_index_allow");
        const auto after_allow_again =
            bioseq::cache_test::read_cache_stats(admin);
        CHECK(bioseq::postgres_test::hash_ordered_result_rows(allow_again)
              == allow_digest);
        check_one_completed_exact_hit(after_allow_again - after_absent);
        CHECK(exact_child_executions(admin) == child_before + 2);

        const auto before_persisted =
            bioseq::cache_test::read_cache_stats(admin);
        const auto persisted_error =
            bioseq::postgres_test::execute_expecting_sql_state(
                data, "EXECUTE bioseq_index_persisted", "55000", 5min);
        const auto after_persisted =
            bioseq::cache_test::read_cache_stats(admin);
        CHECK(persisted_error.message.find(
                  "required compatible persisted minimizer index unavailable")
              != std::string::npos);
        CHECK((after_persisted - before_persisted).produced == 0);
        CHECK((after_persisted - before_persisted).completed_hits == 0);
    }

    {
        bioseq::cache_test::execute(
            data,
            "DEALLOCATE ALL; "
            "SET bioseq.aligner.chaining.maximum_skipped_anchors = 25");
        bioseq::cache_test::execute(
            admin,
            "SELECT bioseq_cache.invalidate_all(); "
            "SELECT bioseq_cache.reset_stats()");
        const std::string override_query = query_with_aligner_options(
            "max_chain_skip => 31, index_read_requirement => 0");
        prepare_query(data, "bioseq_options_precedence", override_query);

        const auto before_a =
            bioseq::cache_test::read_cache_stats(admin);
        const auto child_before = exact_child_executions(admin);
        const auto result_a =
            execute_prepared(data, "bioseq_options_precedence");
        const auto after_a =
            bioseq::cache_test::read_cache_stats(admin);
        REQUIRE(result_a.row_count() > 0);
        const auto digest =
            bioseq::postgres_test::hash_ordered_result_rows(result_a);
        check_one_exact_producer(after_a - before_a);
        CHECK(exact_child_executions(admin) == child_before + 1);

        bioseq::cache_test::execute(
            data,
            "SET bioseq.aligner.chaining.maximum_skipped_anchors = 26");
        const auto result_b =
            execute_prepared(data, "bioseq_options_precedence");
        const auto after_b =
            bioseq::cache_test::read_cache_stats(admin);
        CHECK(bioseq::postgres_test::hash_ordered_result_rows(result_b) == digest);
        check_one_completed_exact_hit(after_b - after_a);
        CHECK(exact_child_executions(admin) == child_before + 1);

        bioseq::cache_test::execute(
            data,
            "SET bioseq.aligner.chaining.maximum_skipped_anchors = 25");
        const auto result_a_again =
            execute_prepared(data, "bioseq_options_precedence");
        const auto after_a_again =
            bioseq::cache_test::read_cache_stats(admin);
        CHECK(bioseq::postgres_test::hash_ordered_result_rows(result_a_again)
              == digest);
        check_one_completed_exact_hit(after_a_again - after_b);
        CHECK(exact_child_executions(admin) == child_before + 1);
    }

    {
        bioseq::cache_test::execute(
            data,
            "DEALLOCATE ALL; "
            "SET bioseq.column_cache.enabled = on; "
            "SET bioseq.vectorization.enabled = on; "
            "SET bioseq.aligner.profiling.enabled = off");
        bioseq::cache_test::execute(
            admin,
            "SELECT bioseq_cache.invalidate_all(); "
            "SELECT bioseq_cache.reset_stats()");
        prepare_query(data, "bioseq_performance_matrix", small_query);

        const auto before =
            bioseq::cache_test::read_cache_stats(admin);
        const auto child_before = exact_child_executions(admin);
        const auto producer =
            execute_prepared(data, "bioseq_performance_matrix");
        const auto after_producer =
            bioseq::cache_test::read_cache_stats(admin);
        REQUIRE(producer.row_count() > 0);
        const auto digest =
            bioseq::postgres_test::hash_ordered_result_rows(producer);
        check_one_exact_producer(after_producer - before);
        CHECK(exact_child_executions(admin) == child_before + 1);

        bioseq::cache_test::execute(
            data, "SET bioseq.column_cache.enabled = off");
        const auto no_column_cache =
            execute_prepared(data, "bioseq_performance_matrix");
        const auto after_column_cache =
            bioseq::cache_test::read_cache_stats(admin);
        CHECK(bioseq::postgres_test::hash_ordered_result_rows(no_column_cache)
              == digest);
        check_one_completed_exact_hit(
            after_column_cache - after_producer);

        bioseq::cache_test::execute(
            data, "SET bioseq.vectorization.enabled = off");
        const auto no_vectorization =
            execute_prepared(data, "bioseq_performance_matrix");
        const auto after_vectorization =
            bioseq::cache_test::read_cache_stats(admin);
        CHECK(bioseq::postgres_test::hash_ordered_result_rows(no_vectorization)
              == digest);
        check_one_completed_exact_hit(
            after_vectorization - after_column_cache);

        bioseq::cache_test::execute(
            data, "SET bioseq.aligner.profiling.enabled = on");
        const auto profiling =
            execute_prepared(data, "bioseq_performance_matrix");
        const auto after_profiling =
            bioseq::cache_test::read_cache_stats(admin);
        CHECK(bioseq::postgres_test::hash_ordered_result_rows(profiling) == digest);
        check_one_completed_exact_hit(
            after_profiling - after_vectorization);

        bioseq::cache_test::execute(
            data,
            "SET bioseq.column_cache.enabled = on; "
            "SET bioseq.vectorization.enabled = on; "
            "SET bioseq.aligner.profiling.enabled = off");
        const auto baseline_again =
            execute_prepared(data, "bioseq_performance_matrix");
        const auto after_baseline_again =
            bioseq::cache_test::read_cache_stats(admin);
        CHECK(bioseq::postgres_test::hash_ordered_result_rows(baseline_again)
              == digest);
        check_one_completed_exact_hit(
            after_baseline_again - after_profiling);
        CHECK(exact_child_executions(admin) == child_before + 1);
    }
}

TEST_CASE("disabling Bioseq custom scans bypasses sibling caching safely",
          "[component:cache][layer:integration]"
          "[feature:provider][resource:postgres]") {
    bioseq::postgres_test::private_cluster_fixture fixture{
        bioseq::cache_test::cluster_options(
            true, bioseq::cache_test::preload_order::cache_then_bioseq)};
    auto& connection = fixture.admin();
    create_small_fixture(connection);
    bioseq::cache_test::execute(
        connection,
        "SET bioseq_cache.query_cache.mode = 'exact_query'; "
        "SET bioseq.planner.custom_scan.enabled = off; "
        "SELECT bioseq_cache.reset_stats()");
    const auto before =
        bioseq::cache_test::read_cache_stats(connection);
    const auto plan = bioseq::postgres_test::execute_explain(
        connection, small_query, false, 5min);
    const auto cache_nodes = plan.find_plan_nodes(
        "Custom Plan Provider", "BioseqCacheScan");
    REQUIRE(cache_nodes.size() == 1);
    CHECK(bioseq::postgres_test::read_plan_boolean_property(
              *cache_nodes.front(), "Cache Force Bypass")
          == true);
    CHECK(bioseq::postgres_test::read_plan_string_property(
              *cache_nodes.front(), "Cache Bypass Reason")
          == "unsafe finalized source path");
    const auto error = bioseq::postgres_test::execute_expecting_sql_state(
        connection, small_query, "0A000", 5min);
    CHECK(error.message.find("no Bioseq executor owner")
          != std::string::npos);
    const auto delta =
        bioseq::cache_test::read_cache_stats(connection) - before;
    CHECK(delta.produced == 0);
    CHECK(delta.completed_hits == 0);
    CHECK(delta.inflight_waits == 0);
    CHECK(delta.reuse_hits == 0);
    CHECK(delta.fallbacks == 1);
    CHECK(delta.invalidations == 0);
    CHECK(delta.evictions == 0);
    CHECK(delta.logical_bytes == 0);
    CHECK(delta.stored_bytes == 0);
}

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

constexpr std::string_view exact_provider_test_case{
    "BioseqScan exact provider produces and hits in same and reuse modes"};
constexpr std::string_view prepared_semantics_test_case{
    "prepared Bioseq plans freeze semantic keys and exclude performance GUCs"};

struct preload_scenario {
    bioseq::cache_test::preload_order order;
    std::string_view name;
};

struct semantic_guc_scenario {
    std::string_view name;
    std::string_view set_a;
    std::string_view set_b;
    std::string_view probe;
    bool result_identity_expected;
};

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

[[nodiscard]] std::int64_t exact_child_executions(
    bioseq::libpq::connection& admin) {
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
    bioseq::libpq::connection& admin,
    bioseq::libpq::connection& data) {
    bioseq::cache_test::exec(
        data,
        "SET bioseq.query_cache_optimisation_mode = 'disabled'");
    const auto oracle = data.exec(batch_join_query, 5min);
    REQUIRE(oracle.rows() > 0);
    const auto oracle_digest =
        bioseq::postgres_test::exact_row_hash(oracle);
    const auto uncached_plan = bioseq::postgres_test::run_explain(
        data, batch_join_query, false, 5min);
    REQUIRE(
        uncached_plan.find_plan_nodes(
            "Custom Plan Provider", "BioseqBatchJoin").size()
        == 1);

    bioseq::cache_test::exec(
        admin,
        "SELECT bioseq_cache.invalidate_all(); "
        "SELECT bioseq_cache.reset_stats()");
    bioseq::cache_test::exec(
        data,
        "SET bioseq.query_cache_optimisation_mode = 'same'");
    const auto cacheable_plan = bioseq::postgres_test::run_explain(
        data, batch_join_query, false, 5min);
    const auto planned_cache_nodes = cacheable_plan.find_plan_nodes(
        "Custom Plan Provider", "BioseqCacheScan");
    REQUIRE(planned_cache_nodes.size() == 1);
    REQUIRE(bioseq::postgres_test::explain_property_bool(
                *planned_cache_nodes.front(), "Cache Force Bypass")
            == false);
    REQUIRE(bioseq::postgres_test::explain_property_integer(
                *planned_cache_nodes.front(),
                "Provider Runtime Recipes")
            == 3);
    REQUIRE(
        cacheable_plan.find_plan_nodes(
            "Custom Plan Provider", "BioseqBatchJoin").size()
        == 1);
    const auto before = bioseq::cache_test::read_cache_stats(admin);
    const auto child_before = exact_child_executions(admin);
    const auto cold = data.exec(batch_join_query, 5min);
    const auto after_cold = bioseq::cache_test::read_cache_stats(admin);
    const auto child_after_cold = exact_child_executions(admin);
    const auto warm = data.exec(batch_join_query, 5min);
    const auto after_warm = bioseq::cache_test::read_cache_stats(admin);
    const auto child_after_warm = exact_child_executions(admin);

    REQUIRE(bioseq::postgres_test::exact_row_hash(cold) == oracle_digest);
    REQUIRE(bioseq::postgres_test::exact_row_hash(warm) == oracle_digest);
    check_one_exact_producer(after_cold - before);
    check_one_completed_exact_hit(after_warm - after_cold);
    CHECK(child_after_cold == child_before + 1);
    CHECK(child_after_warm == child_after_cold);

    const auto warm_explain = bioseq::postgres_test::run_explain(
        data, batch_join_query, true, 5min);
    const auto cache_nodes = warm_explain.find_plan_nodes(
        "Custom Plan Provider", "BioseqCacheScan");
    REQUIRE(cache_nodes.size() == 1);
    CHECK(bioseq::postgres_test::explain_property_string(
              *cache_nodes.front(), "Actual Cache Action")
          == "[Hit] exact final result");
    REQUIRE(
        warm_explain.find_plan_nodes(
            "Custom Plan Provider", "BioseqBatchJoin").size()
        == 1);
    const bioseq::postgres_test::json_value* children =
        cache_nodes.front()->find("Plans");
    REQUIRE(children != nullptr);
    REQUIRE(children->as_array() != nullptr);
    REQUIRE_FALSE(children->as_array()->empty());
    CHECK(bioseq::postgres_test::explain_property_number(
              children->as_array()->front(), "Actual Loops")
          == 0.0);
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

void prepare_query(bioseq::libpq::connection& connection,
                   std::string_view name,
                   std::string_view query) {
    bioseq::cache_test::exec(
        connection,
        "PREPARE " + std::string(name) + " AS " + std::string(query));
}

[[nodiscard]] bioseq::libpq::result execute_prepared(
    bioseq::libpq::connection& connection,
    std::string_view name) {
    return connection.exec("EXECUTE " + std::string(name), 5min);
}

}  // namespace

TEST_CASE("BioseqScan exact provider produces and hits in same and reuse modes",
          "[cache][integration][bioseq][provider][exact]") {
    bioseq::postgres_test::migration_probe_scope probes{
        std::string(exact_provider_test_case)};
    constexpr std::array preload_scenarios{
        preload_scenario{
            bioseq::cache_test::preload_order::cache_then_bioseq,
            "cache_then_bioseq"},
        preload_scenario{
            bioseq::cache_test::preload_order::bioseq_then_cache,
            "bioseq_then_cache"}};
    constexpr std::array<std::string_view, 2> modes{"same", "reuse"};
    std::optional<bioseq::postgres_test::sha256_digest>
        reference_oracle_digest;
    std::array<std::optional<bioseq::cache_test::cache_stats>, modes.size()>
        reference_cold_deltas;
    std::array<std::optional<bioseq::cache_test::cache_stats>, modes.size()>
        reference_warm_deltas;
    std::array<std::optional<bioseq::cache_test::cache_stats>, modes.size()>
        reference_explain_deltas;

    for (const preload_scenario& scenario : preload_scenarios) {
        bioseq::postgres_test::migration_probe_scope order_probes{
            std::string(exact_provider_test_case),
            "preload order", std::string(scenario.name)};
        bioseq::postgres_test::postgres_fixture fixture{
            bioseq::cache_test::cluster_options(true, scenario.order)};
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
        if (reference_oracle_digest.has_value()) {
            REQUIRE(oracle_digest == *reference_oracle_digest);
        } else {
            reference_oracle_digest = oracle_digest;
        }
        bioseq::postgres_test::record_migration_probe(
            "combined.bioseq_provider.disabled_oracle_nonempty");

        for (std::size_t mode_index = 0;
             mode_index < modes.size(); ++mode_index) {
            const std::string_view mode = modes[mode_index];
            std::string permutation{scenario.name};
            permutation += "/";
            permutation += mode;
            bioseq::postgres_test::migration_probe_scope mode_probes{
                std::string(exact_provider_test_case),
                "preload order and exact mode", permutation};
            bioseq::cache_test::exec(
                admin,
                "SELECT bioseq_cache.invalidate_all(); "
                "SELECT bioseq_cache.reset_stats()");
            bioseq::cache_test::exec(
                data,
                "SET bioseq.query_cache_optimisation_mode = "
                    + data.escape_literal(mode));

            const auto before =
                bioseq::cache_test::read_cache_stats(admin);
            const auto child_before = exact_child_executions(admin);
            const auto cold = data.exec(small_query, 5min);
            const auto after_cold =
                bioseq::cache_test::read_cache_stats(admin);
            const auto child_after_cold = exact_child_executions(admin);
            const auto warm = data.exec(small_query, 5min);
            const auto after_warm =
                bioseq::cache_test::read_cache_stats(admin);
            const auto child_after_warm = exact_child_executions(admin);
            const auto cold_delta = after_cold - before;
            const auto warm_delta = after_warm - after_cold;
            CHECK(bioseq::postgres_test::exact_row_hash(cold)
                  == oracle_digest);
            CHECK(bioseq::postgres_test::exact_row_hash(warm)
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
            const bioseq::postgres_test::json_value* children =
                cache_nodes.front()->find("Plans");
            if (children != nullptr) {
                REQUIRE(children->as_array() != nullptr);
                REQUIRE_FALSE(children->as_array()->empty());
                CHECK(bioseq::postgres_test::explain_property_number(
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
            bioseq::postgres_test::record_migration_probe(
                "combined.bioseq_provider.explain_exact_hit");
            bioseq::postgres_test::record_migration_probe(
                "combined.bioseq_provider.warm_child_not_executed");
        }
        check_batch_join_exact_provider(admin, data);
    }
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_provider.load_order_equivalent_exact");
}

TEST_CASE("prepared Bioseq plans freeze semantic keys and exclude performance GUCs",
          "[cache][integration][bioseq][provider][guc-matrix]") {
    bioseq::postgres_test::migration_probe_scope probes{
        std::string(prepared_semantics_test_case)};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            true, bioseq::cache_test::preload_order::bioseq_then_cache)};
    auto& admin = fixture.admin();
    auto data = fixture.connect(30s);
    create_small_fixture(admin);
    configure_cpu(data);
    bioseq::cache_test::exec(
        admin,
        "SET bioseq.query_cache_optimisation_mode = 'disabled'");
    bioseq::cache_test::exec(
        data,
        "SET bioseq.query_cache_optimisation_mode = 'same'; "
        "SET bioseq.read_sequence_index = on");

    constexpr std::array semantic_scenarios{
        semantic_guc_scenario{
            "integer",
            "SET bioseq.aligner.max_chain_skip = 25",
            "SET bioseq.aligner.max_chain_skip = 26",
            "combined.bioseq_provider.prepared_integer_a_b_a",
            false},
        semantic_guc_scenario{
            "float",
            "SET bioseq.aligner.mask_level = 0.5",
            "SET bioseq.aligner.mask_level = 0.6",
            "combined.bioseq_provider.prepared_float_a_b_a",
            false},
        semantic_guc_scenario{
            "enum",
            "SET bioseq.aligner.chaining_algorithm = 'scalar_mm2'",
            "SET bioseq.aligner.chaining_algorithm = 'simd_mm2'",
            "combined.bioseq_provider.prepared_enum_a_b_a",
            false},
        semantic_guc_scenario{
            "boolean",
            "SET bioseq.aligner.vulkan_index_cache_enabled = off",
            "SET bioseq.aligner.vulkan_index_cache_enabled = on",
            "combined.bioseq_provider.prepared_boolean_a_b_a",
            true},
        semantic_guc_scenario{
            "read_sequence_index",
            "SET bioseq.read_sequence_index = on",
            "SET bioseq.read_sequence_index = off",
            "combined.bioseq_provider.read_index_separates_semantic_key",
            true}};

    for (const semantic_guc_scenario& scenario : semantic_scenarios) {
        bioseq::postgres_test::migration_probe_scope scenario_probes{
            std::string(prepared_semantics_test_case),
            "prepared semantic A-B-A", std::string(scenario.name)};
        bioseq::cache_test::exec(
            data, "DEALLOCATE ALL; " + std::string(scenario.set_a));
        bioseq::cache_test::exec(
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
        REQUIRE(result_a.rows() > 0);
        const auto digest_a =
            bioseq::postgres_test::exact_row_hash(result_a);
        check_one_exact_producer(after_a - before_a);
        CHECK(exact_child_executions(admin) == child_before + 1);

        bioseq::cache_test::exec(data, scenario.set_b);
        const auto result_b =
            execute_prepared(data, "bioseq_semantic_matrix");
        const auto after_b =
            bioseq::cache_test::read_cache_stats(admin);
        REQUIRE(result_b.rows() > 0);
        if (scenario.result_identity_expected) {
            CHECK(bioseq::postgres_test::exact_row_hash(result_b)
                  == digest_a);
        }
        check_one_exact_producer(after_b - after_a);
        CHECK(exact_child_executions(admin) == child_before + 2);

        bioseq::cache_test::exec(data, scenario.set_a);
        const auto result_a_again =
            execute_prepared(data, "bioseq_semantic_matrix");
        const auto after_a_again =
            bioseq::cache_test::read_cache_stats(admin);
        CHECK(bioseq::postgres_test::exact_row_hash(result_a_again)
              == digest_a);
        check_one_completed_exact_hit(after_a_again - after_b);
        CHECK(exact_child_executions(admin) == child_before + 2);
        bioseq::postgres_test::record_migration_probe(scenario.probe);
    }

    {
        bioseq::postgres_test::migration_probe_scope requirement_probes{
            std::string(prepared_semantics_test_case),
            "prepared index-read requirement", "allow-absent-allow-persisted"};
        bioseq::cache_test::exec(
            data,
            "DEALLOCATE ALL; "
            "SET bioseq.read_sequence_index = on");
        bioseq::cache_test::exec(
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
        REQUIRE(allow.rows() > 0);
        const auto allow_digest =
            bioseq::postgres_test::exact_row_hash(allow);
        check_one_exact_producer(after_allow - before_allow);
        CHECK(exact_child_executions(admin) == child_before + 1);

        const auto absent =
            execute_prepared(data, "bioseq_index_absent");
        const auto after_absent =
            bioseq::cache_test::read_cache_stats(admin);
        REQUIRE(absent.rows() > 0);
        CHECK(bioseq::postgres_test::exact_row_hash(absent)
              == allow_digest);
        check_one_exact_producer(after_absent - after_allow);
        CHECK(exact_child_executions(admin) == child_before + 2);

        const auto allow_again =
            execute_prepared(data, "bioseq_index_allow");
        const auto after_allow_again =
            bioseq::cache_test::read_cache_stats(admin);
        CHECK(bioseq::postgres_test::exact_row_hash(allow_again)
              == allow_digest);
        check_one_completed_exact_hit(after_allow_again - after_absent);
        CHECK(exact_child_executions(admin) == child_before + 2);

        const auto before_persisted =
            bioseq::cache_test::read_cache_stats(admin);
        const auto persisted_error =
            bioseq::postgres_test::expect_sqlstate(
                data, "EXECUTE bioseq_index_persisted", "55000", 5min);
        const auto after_persisted =
            bioseq::cache_test::read_cache_stats(admin);
        CHECK(persisted_error.message.find(
                  "required compatible persisted minimizer index unavailable")
              != std::string::npos);
        CHECK((after_persisted - before_persisted).produced == 0);
        CHECK((after_persisted - before_persisted).completed_hits == 0);
        bioseq::postgres_test::record_migration_probe(
            "combined.bioseq_provider.prepared_index_requirement_matrix");
    }

    {
        bioseq::postgres_test::migration_probe_scope precedence_probes{
            std::string(prepared_semantics_test_case),
            "prepared aligner_options precedence", "GUC-A-B-A"};
        bioseq::cache_test::exec(
            data,
            "DEALLOCATE ALL; "
            "SET bioseq.aligner.max_chain_skip = 25");
        bioseq::cache_test::exec(
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
        REQUIRE(result_a.rows() > 0);
        const auto digest =
            bioseq::postgres_test::exact_row_hash(result_a);
        check_one_exact_producer(after_a - before_a);
        CHECK(exact_child_executions(admin) == child_before + 1);

        bioseq::cache_test::exec(
            data, "SET bioseq.aligner.max_chain_skip = 26");
        const auto result_b =
            execute_prepared(data, "bioseq_options_precedence");
        const auto after_b =
            bioseq::cache_test::read_cache_stats(admin);
        CHECK(bioseq::postgres_test::exact_row_hash(result_b) == digest);
        check_one_completed_exact_hit(after_b - after_a);
        CHECK(exact_child_executions(admin) == child_before + 1);

        bioseq::cache_test::exec(
            data, "SET bioseq.aligner.max_chain_skip = 25");
        const auto result_a_again =
            execute_prepared(data, "bioseq_options_precedence");
        const auto after_a_again =
            bioseq::cache_test::read_cache_stats(admin);
        CHECK(bioseq::postgres_test::exact_row_hash(result_a_again)
              == digest);
        check_one_completed_exact_hit(after_a_again - after_b);
        CHECK(exact_child_executions(admin) == child_before + 1);
        bioseq::postgres_test::record_migration_probe(
            "combined.bioseq_provider.prepared_aligner_options_precedence");
    }

    {
        bioseq::postgres_test::migration_probe_scope performance_probes{
            std::string(prepared_semantics_test_case),
            "prepared performance exclusions"};
        bioseq::cache_test::exec(
            data,
            "DEALLOCATE ALL; "
            "SET bioseq.enable_column_cache = on; "
            "SET bioseq.enable_vectorization = on; "
            "SET bioseq.aligner.enable_profiling = off");
        bioseq::cache_test::exec(
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
        REQUIRE(producer.rows() > 0);
        const auto digest =
            bioseq::postgres_test::exact_row_hash(producer);
        check_one_exact_producer(after_producer - before);
        CHECK(exact_child_executions(admin) == child_before + 1);

        bioseq::cache_test::exec(
            data, "SET bioseq.enable_column_cache = off");
        const auto no_column_cache =
            execute_prepared(data, "bioseq_performance_matrix");
        const auto after_column_cache =
            bioseq::cache_test::read_cache_stats(admin);
        CHECK(bioseq::postgres_test::exact_row_hash(no_column_cache)
              == digest);
        check_one_completed_exact_hit(
            after_column_cache - after_producer);
        bioseq::postgres_test::record_migration_probe(
            "combined.bioseq_provider.column_cache_excluded_from_key");

        bioseq::cache_test::exec(
            data, "SET bioseq.enable_vectorization = off");
        const auto no_vectorization =
            execute_prepared(data, "bioseq_performance_matrix");
        const auto after_vectorization =
            bioseq::cache_test::read_cache_stats(admin);
        CHECK(bioseq::postgres_test::exact_row_hash(no_vectorization)
              == digest);
        check_one_completed_exact_hit(
            after_vectorization - after_column_cache);
        bioseq::postgres_test::record_migration_probe(
            "combined.bioseq_provider.vectorization_excluded_from_key");

        bioseq::cache_test::exec(
            data, "SET bioseq.aligner.enable_profiling = on");
        const auto profiling =
            execute_prepared(data, "bioseq_performance_matrix");
        const auto after_profiling =
            bioseq::cache_test::read_cache_stats(admin);
        CHECK(bioseq::postgres_test::exact_row_hash(profiling) == digest);
        check_one_completed_exact_hit(
            after_profiling - after_vectorization);
        bioseq::postgres_test::record_migration_probe(
            "combined.bioseq_provider.profiling_excluded_from_key");

        bioseq::cache_test::exec(
            data,
            "SET bioseq.enable_column_cache = on; "
            "SET bioseq.enable_vectorization = on; "
            "SET bioseq.aligner.enable_profiling = off");
        const auto baseline_again =
            execute_prepared(data, "bioseq_performance_matrix");
        const auto after_baseline_again =
            bioseq::cache_test::read_cache_stats(admin);
        CHECK(bioseq::postgres_test::exact_row_hash(baseline_again)
              == digest);
        check_one_completed_exact_hit(
            after_baseline_again - after_profiling);
        CHECK(exact_child_executions(admin) == child_before + 1);
        bioseq::postgres_test::record_migration_probe(
            "combined.bioseq_provider.prepared_performance_a_b_a");
    }
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
    const auto before =
        bioseq::cache_test::read_cache_stats(connection);
    const auto plan = bioseq::postgres_test::run_explain(
        connection, small_query, false, 5min);
    const auto cache_nodes = plan.find_plan_nodes(
        "Custom Plan Provider", "BioseqCacheScan");
    REQUIRE(cache_nodes.size() == 1);
    CHECK(bioseq::postgres_test::explain_property_bool(
              *cache_nodes.front(), "Cache Force Bypass")
          == true);
    CHECK(bioseq::postgres_test::explain_property_string(
              *cache_nodes.front(), "Cache Bypass Reason")
          == "unsafe finalized source path");
    const auto error = bioseq::postgres_test::expect_sqlstate(
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
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_provider.custom_scan_off_bypasses");
}

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

namespace {

enum class validity_change {
    runtime_digest_mismatch,
    provider_unregistered,
};

constexpr std::string_view validity_case{
    "frozen reusable plans fail open when provider validity changes"};

void create_provider_rows(bioseq::libpq::connection& connection) {
    bioseq::cache_test::exec(
        connection,
        R"SQL(
            CREATE TABLE cache_provider_validity_rows(
                row_id bigint PRIMARY KEY,
                base_id bigint NOT NULL,
                occurrence_no integer NOT NULL,
                nullable_value integer,
                payload text NOT NULL,
                capability_mask bigint NOT NULL
            );
            INSERT INTO cache_provider_validity_rows VALUES
                (1, 10, 0, NULL, 'null', 3),
                (2, 11, 0, 0,    'zero', 3),
                (3, 12, 0, 5,    'match-a', 3),
                (4, 12, 1, 5,    'match-b', 3),
                (5, 13, 0, 10,   'ten', 3),
                (6, 14, 0, 20,   'twenty', 3);
            SELECT bioseq_cache.invalidate_all();
            SELECT bioseq_cache_test.request_janitor_sweep();
            SELECT bioseq_cache.reset_stats();
            SELECT bioseq_cache_test.provider_init();
            SELECT bioseq_cache_test.provider_use_relation(
                'cache_provider_validity_rows');
            SET bioseq.query_cache_optimisation_mode = 'reuse';
            SET plan_cache_mode = force_generic_plan;
        )SQL");
}

void require_ids(
    const bioseq::libpq::result& result,
    const std::array<std::string_view, 4>& expected) {
    REQUIRE(result.rows() == 4);
    REQUIRE(result.columns() == 1);
    for (std::size_t index = 0; index < expected.size(); ++index) {
        REQUIRE_FALSE(result.is_null(
            static_cast<int>(index), 0));
        CHECK(result.value(static_cast<int>(index), 0)
              == expected[index]);
    }
}

void require_references_drained(
    bioseq::libpq::connection& connection) {
    const auto public_references = connection.exec(
        "SELECT coalesce(sum(waiters), 0), "
        "       coalesce(sum(readers), 0), "
        "       coalesce(sum("
        "         (dependencies->>'planner_pins')::bigint), 0) "
        "FROM bioseq_cache.entries() "
        "WHERE kind = 'ReusableCoverage'");
    REQUIRE(public_references.rows() == 1);
    REQUIRE(public_references.columns() == 3);
    CHECK(public_references.value(0, 0) == "0");
    CHECK(public_references.value(0, 1) == "0");
    CHECK(public_references.value(0, 2) == "0");
    CHECK(bioseq::cache_test::integer(
              connection,
              "SELECT reader_pins + waiter_references "
              "     + admission_reservations "
              "FROM bioseq_cache_test.planner_reference_counts()")
          == 0);
}

void run_validity_permutation(validity_change change) {
    const std::string permutation =
        change == validity_change::runtime_digest_mismatch
        ? "runtime-digest-mismatch"
        : "provider-unregistered";
    bioseq::postgres_test::migration_probe_scope probes{
        std::string(validity_case), {}, permutation};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();
    create_provider_rows(connection);

    bioseq::cache_test::exec(
        connection,
        "PREPARE cache_provider_validity_source AS "
        "SELECT row_id "
        "FROM cache_provider_validity_rows "
        "WHERE nullable_value <= 10 "
        "ORDER BY row_id");
    const auto source_plan = bioseq::postgres_test::run_explain(
        connection,
        "EXECUTE cache_provider_validity_source", false, 30s);
    const auto source_provider_nodes = source_plan.find_plan_nodes(
        "Custom Plan Provider",
        "BioseqCacheGenericTestProvider");
    const auto source_reusable_nodes = source_plan.find_plan_nodes(
        "Cache Entry Kind", "ReusableCoverage");
    REQUIRE(source_provider_nodes.size() == 1);
    REQUIRE(source_reusable_nodes.size() == 1);
    const auto source_recipe_valid =
        bioseq::postgres_test::explain_property_bool(
            *source_reusable_nodes.front(),
            "Provider Runtime Recipe Valid");
    const auto source_selected_branches =
        bioseq::postgres_test::explain_property_number(
            *source_reusable_nodes.front(),
            "Selected Cache Branches");
    REQUIRE(source_recipe_valid.has_value());
    REQUIRE(source_selected_branches.has_value());
    CHECK(*source_recipe_valid);
    CHECK(*source_selected_branches == 0.0);
    const auto source =
        connection.exec("EXECUTE cache_provider_validity_source", 30s);
    require_ids(source, {"2", "3", "4", "5"});
    CHECK(bioseq::cache_test::integer(
              connection,
              "SELECT count(*) "
              "FROM bioseq_cache.entries() "
              "WHERE kind = 'ReusableCoverage' "
              "  AND state = 'COMPLETE'")
          == 1);

    bioseq::cache_test::exec(
        connection,
        "SELECT bioseq_cache_test.provider_reset(); "
        "SELECT bioseq_cache.reset_stats(); "
        "PREPARE cache_provider_validity AS "
        "SELECT row_id "
        "FROM cache_provider_validity_rows "
        "WHERE nullable_value >= 5 "
        "ORDER BY row_id");
    const auto frozen_plan = bioseq::postgres_test::run_explain(
        connection, "EXECUTE cache_provider_validity", false, 30s);
    const auto frozen_provider_nodes = frozen_plan.find_plan_nodes(
        "Custom Plan Provider",
        "BioseqCacheGenericTestProvider");
    const auto frozen_reusable_nodes = frozen_plan.find_plan_nodes(
        "Cache Entry Kind", "ReusableCoverage");
    REQUIRE(frozen_provider_nodes.size() == 1);
    REQUIRE(frozen_reusable_nodes.size() == 1);
    const auto frozen_recipe_valid =
        bioseq::postgres_test::explain_property_bool(
            *frozen_reusable_nodes.front(),
            "Provider Runtime Recipe Valid");
    const auto frozen_selected_branches =
        bioseq::postgres_test::explain_property_number(
            *frozen_reusable_nodes.front(),
            "Selected Cache Branches");
    REQUIRE(frozen_recipe_valid.has_value());
    REQUIRE(frozen_selected_branches.has_value());
    CHECK(*frozen_recipe_valid);
    CHECK(*frozen_selected_branches == 1.0);

    if (change == validity_change::runtime_digest_mismatch) {
        bioseq::cache_test::exec(
            connection,
            "SELECT bioseq_cache_test.provider_set_digest_fault("
            "'runtime_mismatch')");
    } else {
        bioseq::cache_test::exec(
            connection,
            "SELECT bioseq_cache_test.provider_shutdown()");
        const auto replanned = bioseq::postgres_test::run_explain(
            connection, "EXECUTE cache_provider_validity", false, 30s);
        CHECK(replanned
                  .find_plan_nodes(
                      "Custom Plan Provider",
                      "BioseqCacheGenericTestProvider")
                  .empty());
        CHECK(replanned
                  .find_plan_nodes(
                      "Cache Entry Kind", "ReusableCoverage")
                  .empty());
    }

    const auto retained_child =
        connection.exec("EXECUTE cache_provider_validity", 30s);
    require_ids(retained_child, {"3", "4", "5", "6"});
    const auto counters = connection.exec(
        "SELECT "
        "  (stats->>'executor_rows')::bigint, "
        "  (stats->>'late_fetch_batch')::bigint, "
        "  (stats->>'late_fetch_references')::bigint, "
        "  cache_stats.fallbacks "
        "FROM (SELECT bioseq_cache_test.provider_stats() AS stats) "
        "     AS provider "
        "CROSS JOIN bioseq_cache.stats() AS cache_stats");
    REQUIRE(counters.rows() == 1);
    REQUIRE(counters.columns() == 4);
    CHECK(counters.value(0, 1) == "0");
    CHECK(counters.value(0, 2) == "0");
    if (change == validity_change::runtime_digest_mismatch) {
        CHECK(counters.value(0, 0) == "4");
        CHECK(counters.value(0, 3) == "1");
    } else {
        CHECK(counters.value(0, 0) == "0");
        CHECK(counters.value(0, 3) == "0");
    }
    require_references_drained(connection);

    if (change == validity_change::runtime_digest_mismatch) {
        bioseq::cache_test::exec(
            connection,
            "SELECT bioseq_cache_test.provider_set_digest_fault('none'); "
            "SELECT bioseq_cache_test.provider_shutdown()");
    }
    bioseq::cache_test::exec(connection, "DEALLOCATE ALL");
    bioseq::postgres_test::record_migration_probe(
        "isolation.cache_reuse_provider_validity.fail_open");
}

}  // namespace

TEST_CASE("frozen reusable plans fail open when provider validity changes",
          "[cache][integration][isolation][provider][validity]") {
    run_validity_permutation(
        validity_change::runtime_digest_mismatch);
    run_validity_permutation(
        validity_change::provider_unregistered);
}

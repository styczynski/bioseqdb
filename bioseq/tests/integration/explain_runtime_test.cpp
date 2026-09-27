/**
 * @file
 * @brief Exercises transformed-owner and DSM runtime EXPLAIN contracts.
 */
#include <libpq-fe.h>
#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;
import bioseq.tests.integration.explain_test_support;

using namespace bioseq_int_tests;
using namespace bioseq_int_tests::explain_test_support;

TEST_CASE("transformed INNER mmsearch explains one per-row BatchJoin owner",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    constexpr std::string_view target = "explain_slice_join_target";
    auto ctx = fresh_context({target});
    seed_small_bioseq_column(ctx, target);
    exec_sql(ctx,
             "CREATE TEMP TABLE explain_slice_join_query "
             "(id int, seq bioseq.seq('dna4',32));");
    exec_sql(ctx,
             "INSERT INTO explain_slice_join_query VALUES "
             "(1, 'ACGTACGT'::bioseq.seq('dna4',32));");

    const auto plan = explain_with_options(ctx,
                                           "VERBOSE, COSTS OFF",
                                           fmt::format("SELECT q.id, t.pk FROM {} AS t "
                                                       "JOIN explain_slice_join_query AS q "
                                                       "ON mmsearch(t.seq0[0:], q.seq[0:]);",
                                                       target));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(plan, "Bioseq Mmsearch Ownership: per-row recipe"));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE_FALSE(plan_contains(plan, "Join Filter: mmsearch"));
}

TEST_CASE("mmsearch slice boundaries follow the selected minimizer route",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    constexpr std::string_view target = "explain_slice_join_materialization_target";
    auto ctx = fresh_context({target});
    seed_small_bioseq_column(ctx, target);
    exec_sql(ctx,
             fmt::format("TRUNCATE {}; "
                         "INSERT INTO {} VALUES "
                         "(1, repeat('ACGT',64)::bioseq.seq('dna4',32));",
                         target,
                         target));
    exec_sql(ctx,
             "CREATE TEMP TABLE explain_slice_join_materialization_query "
             "(id int, seq bioseq.seq('dna4',32)); "
             "INSERT INTO explain_slice_join_materialization_query VALUES "
             "(1, repeat('ACGT',64)::bioseq.seq('dna4',32)); "
             "SET max_parallel_workers_per_gather=0; "
             "SET bioseq.index.sequence_read.enabled=off; "
             "SET bioseq.aligner.vulkan.enabled=off;");

    const auto sql = fmt::format("SELECT q.id, t.pk FROM {} AS t "
                                 "JOIN explain_slice_join_materialization_query AS q "
                                 "ON mmsearch("
                                 "t.seq0[1:128] OPERATOR(bioseq.||) t.seq0[128:], "
                                 "q.seq[1:128] OPERATOR(bioseq.||) q.seq[128:]);",
                                 target);

    {
        guc_scope scalar(ctx, "bioseq.aligner.minimizer.algorithm", "scalar");
        const auto streaming = explain_with_options(ctx, "ANALYZE, TIMING OFF, COSTS OFF", sql);
        REQUIRE(plan_contains(streaming, "Custom Scan (BioseqBatchJoin)"));
        REQUIRE(plan_contains(streaming, "Custom Scan (BioseqSliceProject)"));
        REQUIRE_FALSE(plan_contains(streaming, "Reason: mmsearch target"));
        REQUIRE_FALSE(plan_contains(streaming, "Reason: mmsearch query"));
        REQUIRE_FALSE(plan_contains(streaming, "Mechanism: packed region"));
        REQUIRE_FALSE(plan_contains(streaming, "Mechanism: packed full"));
    }

    {
        guc_scope admitted(ctx, "bioseq.aligner.minimizer.algorithm", "two_stack_nthash");
        const auto resident = explain_with_options(ctx, "ANALYZE, TIMING OFF, COSTS OFF", sql);
        REQUIRE(plan_contains(resident, "Custom Scan (BioseqBatchJoin)"));
        REQUIRE(plan_contains(resident, "Custom Scan (BioseqSliceProject)"));
        REQUIRE_FALSE(plan_contains(resident, "Mechanism: packed full"));
        REQUIRE(boundary_actual_calls(resident, "packed region", "mmsearch target") == 1u);
        REQUIRE(boundary_actual_bytes(resident, "packed region", "mmsearch target") == 64u);
        REQUIRE(boundary_actual_calls(resident, "packed region", "mmsearch query") == 1u);
        REQUIRE(boundary_actual_bytes(resident, "packed region", "mmsearch query") == 64u);
    }
}

#ifdef BIOSEQ_TEST_INDEXES
namespace {

/** EXPLAIN formats exercised by DSM snapshot property checks. */
enum class dsm_explain_format {
    text,
    json,
    yaml,
    xml,
};

/** Require one DSM snapshot property with format-specific serialization. */
void require_dsm_snapshot_property(std::span<const std::string> plan,
                                   dsm_explain_format format,
                                   std::string_view name,
                                   std::string_view value,
                                   bool string_value = false) {
    switch (format) {
    case dsm_explain_format::text:
        REQUIRE(plan_contains(plan, fmt::format("{}: {}", name, value)));
        return;
    case dsm_explain_format::yaml:
        REQUIRE(plan_contains(plan,
                              string_value ? fmt::format("{}: \"{}\"", name, value)
                                           : fmt::format("{}: {}", name, value)));
        return;
    case dsm_explain_format::json:
        REQUIRE(plan_contains(plan,
                              string_value ? fmt::format("\"{}\": \"{}\"", name, value)
                                           : fmt::format("\"{}\": {}", name, value)));
        return;
    case dsm_explain_format::xml: {
        std::string tag{name};
        std::replace(tag.begin(), tag.end(), ' ', '-');
        REQUIRE(plan_contains(plan, fmt::format("<{}>{}</{}>", tag, value, tag)));
        return;
    }
    }
}

/** Arm one test-only DSM snapshot fixture in the current backend. */
void arm_dsm_snapshot(const connection_test_context& ctx, std::string_view mode) {
    exec_sql(ctx, fmt::format("SELECT bioseq_internal.arm_slice_dsm_fixture('{}');", mode));
}

} // namespace

TEST_CASE("malformed DSM snapshots reject before counter dereference",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);
    arm_dsm_snapshot(ctx, "none");
    const auto sql = fmt::format("SELECT seq0[0:4] FROM {} WHERE pk = 1;", k_table);
    const std::array formats{
        std::pair{dsm_explain_format::text, std::string_view{"ANALYZE, TIMING OFF, COSTS OFF"}},
        std::pair{dsm_explain_format::json,
                  std::string_view{"ANALYZE, TIMING OFF, COSTS OFF, FORMAT JSON"}},
        std::pair{dsm_explain_format::yaml,
                  std::string_view{"ANALYZE, TIMING OFF, COSTS OFF, FORMAT YAML"}},
        std::pair{dsm_explain_format::xml,
                  std::string_view{"ANALYZE, TIMING OFF, COSTS OFF, FORMAT XML"}},
    };

    for (const auto& [format, options] : formats) {
        arm_dsm_snapshot(ctx, "malformed");
        const auto plan = explain_with_options(ctx, options, sql);
        require_dsm_snapshot_property(plan, format, "DSM Test Fixture", "malformed", true);
        require_dsm_snapshot_property(plan, format, "DSM Fixture Validated", "true");
        require_dsm_snapshot_property(plan, format, "DSM Async Lifecycle Validated", "true");
        require_dsm_snapshot_property(plan, format, "DSM Fixture Checks", "6");
        require_dsm_snapshot_property(plan, format, "DSM Fixture Counter Records Read", "0");
        require_dsm_snapshot_property(plan, format, "DSM Async Cleanup Callbacks", "3");
        require_dsm_snapshot_property(plan, format, "Counter Overflow", "true");
        REQUIRE_FALSE(plan_contains(plan, "Incomplete Workers"));
        REQUIRE_FALSE(plan_contains(plan, "Incomplete-Workers"));
    }
    const auto unarmed = explain_with_options(ctx, "ANALYZE, TIMING OFF, COSTS OFF", sql);
    REQUIRE_FALSE(plan_contains(unarmed, "DSM Test Fixture"));

    const auto failing_sql =
        fmt::format("SELECT seq0[0:4], 1 / (pk - pk) FROM {} WHERE pk = 1;", k_table);
    const auto failing_plan = explain_with_options(ctx, "COSTS OFF", failing_sql);
    REQUIRE(plan_contains(failing_plan, "Custom Scan (BioseqSliceProject)"));
    arm_dsm_snapshot(ctx, "malformed");
    const auto execution_error =
        sql_error(ctx, fmt::format("EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF) {}", failing_sql));
    REQUIRE(execution_error.sqlstate == "22012");
    const auto after_error = explain_with_options(ctx, "ANALYZE, TIMING OFF, COSTS OFF", sql);
    REQUIRE_FALSE(plan_contains(after_error, "DSM Test Fixture"));
    arm_dsm_snapshot(ctx, "none");
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("two-cycle DSM rescan counts incomplete work exactly once",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);
    exec_sql(ctx, "SET max_parallel_workers_per_gather=0;");
    arm_dsm_snapshot(ctx, "none");
    const auto sql = fmt::format("SELECT seq0[0:4] FROM {} WHERE pk = 1;", k_table);
    const std::array formats{
        std::pair{dsm_explain_format::text, std::string_view{"ANALYZE, TIMING OFF, COSTS OFF"}},
        std::pair{dsm_explain_format::json,
                  std::string_view{"ANALYZE, TIMING OFF, COSTS OFF, FORMAT JSON"}},
        std::pair{dsm_explain_format::yaml,
                  std::string_view{"ANALYZE, TIMING OFF, COSTS OFF, FORMAT YAML"}},
        std::pair{dsm_explain_format::xml,
                  std::string_view{"ANALYZE, TIMING OFF, COSTS OFF, FORMAT XML"}},
    };

    for (const auto& [format, options] : formats) {
        arm_dsm_snapshot(ctx, "incomplete");
        const auto plan = explain_with_options(ctx, options, sql);
        require_dsm_snapshot_property(plan, format, "DSM Test Fixture", "incomplete", true);
        require_dsm_snapshot_property(plan, format, "DSM Fixture Validated", "true");
        require_dsm_snapshot_property(plan, format, "DSM Async Lifecycle Validated", "true");
        require_dsm_snapshot_property(plan, format, "DSM Fixture Checks", "5");
        require_dsm_snapshot_property(plan, format, "DSM Fixture Slot Headers Read", "9");
        require_dsm_snapshot_property(plan, format, "DSM Fixture Counter Records Read", "6");
        require_dsm_snapshot_property(plan, format, "DSM Async Cleanup Callbacks", "3");
        require_dsm_snapshot_property(plan, format, "Incomplete Workers", "2");
        require_dsm_snapshot_property(plan, format, "Counter Overflow", "false");
        require_dsm_snapshot_property(plan, format, "Actual Calls", "15");
        require_dsm_snapshot_property(plan, format, "Actual Bytes", "150");
        if (format == dsm_explain_format::text) {
            REQUIRE(boundary_actual_calls(plan, "text", "client output") == 15u);
            REQUIRE(boundary_actual_bytes(plan, "text", "client output") == 150u);
            REQUIRE(boundary_actual_calls(plan, "binary", "client output") == 15u);
            REQUIRE(boundary_actual_bytes(plan, "binary", "client output") == 150u);
        }
    }
    const auto unarmed = explain_with_options(ctx, "ANALYZE, TIMING OFF, COSTS OFF", sql);
    REQUIRE_FALSE(plan_contains(unarmed, "DSM Test Fixture"));
    arm_dsm_snapshot(ctx, "none");
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}
#endif

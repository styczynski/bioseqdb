/**
 * @file
 * @brief Checks BioseqScan plans and their EXPLAIN output.
 */
#include <libpq-fe.h>
#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;
import bioseq.tests.integration.explain_test_support;

using namespace bioseq_int_tests;
using namespace bioseq_int_tests::explain_test_support;

TEST_CASE("EXPLAIN names the custom scan on a USING bioseq table",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);

    const auto plan = explain_text<false>(ctx, explain_target_sql());
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
    REQUIRE_FALSE(plan_contains(plan, "Filter: mmsearch"));
}

TEST_CASE("EXPLAIN's Bioseq Projected Columns property lists exactly the SELECTed attrs",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);

    const auto plan = explain_text<false>(ctx, explain_target_sql());
    const auto value = find_explain_property(plan, "Bioseq Projected Columns");
    REQUIRE(value.has_value());
    // Keep projected columns in the SELECT-list, order used by explain_target_sql.
    REQUIRE(value.value() == "pk, seq0");
}

namespace {
/** Checks that BioseqScan is still selected with this configuration setting. */
void check_explain_keeps_custom_scan(std::string_view guc, std::string_view value) {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);

    guc_scope set(ctx, guc, value);
    const auto plan = explain_text<false>(ctx, explain_target_sql());
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
}
} // namespace

TEST_CASE("EXPLAIN: BioseqScan persists with planner.index_scan.enabled=on",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    check_explain_keeps_custom_scan("bioseq.planner.index_scan.enabled", "on");
}
TEST_CASE("EXPLAIN: BioseqScan persists with planner.index_scan.enabled=off",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    check_explain_keeps_custom_scan("bioseq.planner.index_scan.enabled", "off");
}
TEST_CASE("EXPLAIN: BioseqScan persists with vectorization.enabled=on",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    check_explain_keeps_custom_scan("bioseq.vectorization.enabled", "on");
}
TEST_CASE("EXPLAIN: BioseqScan persists with vectorization.enabled=off",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    check_explain_keeps_custom_scan("bioseq.vectorization.enabled", "off");
}
TEST_CASE("EXPLAIN: BioseqScan persists with column_cache.enabled=on",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    check_explain_keeps_custom_scan("bioseq.column_cache.enabled", "on");
}
TEST_CASE("EXPLAIN: BioseqScan persists with column_cache.enabled=off",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    check_explain_keeps_custom_scan("bioseq.column_cache.enabled", "off");
}

TEST_CASE("EXPLAIN ANALYZE on mmsearch query reports the BioseqScan rows",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);

    const auto plan =
        explain_text<true>(ctx,
                           fmt::format("SELECT pk, bioseq.cigar(pk) FROM {} WHERE mmsearch(seq0, "
                                       "'AAAA'::bioseq.seq('dna4', 64)) ORDER BY pk;",
                                       k_table));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
    REQUIRE(plan_contains(plan, "actual time"));
}

TEST_CASE("EXPLAIN accepts empty scan projections in both vectorization modes",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);

    for (const std::string_view vectorization : {"on", "off"}) {
        guc_scope setting(ctx, "bioseq.vectorization.enabled", vectorization);
        for (const std::string_view projection : {"1", "(ARRAY[1,2,3])[1]", "count(*)"}) {
            const auto sql = fmt::format("SELECT {} FROM {};", projection, k_table);
            CAPTURE(vectorization, sql);
            for (const std::string_view options : {"VERBOSE, COSTS OFF",
                                                   "ANALYZE, VERBOSE, COSTS OFF, TIMING OFF"}) {
                const auto plan = explain_with_options(ctx, options, sql);
                REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
                REQUIRE_FALSE(find_explain_property(plan, "Bioseq Retained Alignment Operands").has_value());
            }
        }
    }
}

TEST_CASE("EXPLAIN finds retained alignment operands inside projection expressions",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    seed_small_bioseq_column(ctx, k_table);
    constexpr std::array<std::pair<std::string_view, std::string_view>, 3> projections{{
        {"bioseq.len(bioseq_internal.alignment_query_sequence(seq0))", "query"},
        {"bioseq.len(bioseq_internal.alignment_reference_sequence(seq0))", "reference"},
        {"bioseq.len(bioseq_internal.alignment_query_sequence(seq0)) + "
         "bioseq.len(bioseq_internal.alignment_reference_sequence(seq0))", "query, reference"},
    }};
    for (const std::string_view vectorization : {"on", "off"}) {
        guc_scope setting(ctx, "bioseq.vectorization.enabled", vectorization);
        for (const auto& [projection, expected] : projections) {
            const auto sql = fmt::format(
                "SELECT {} FROM {} WHERE mmsearch(seq0, "
                "'TTTTAAAAGGGGCCCC'::bioseq.seq('dna4',64));", projection, k_table);
            CAPTURE(vectorization, sql);
            const auto plan = explain_text<false>(ctx, sql);
            REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
            REQUIRE(find_explain_property(plan, "Bioseq Retained Alignment Operands") == expected);
        }
    }
}

/**
 * @file
 * @brief Exercises base Bioseq planner and scan EXPLAIN contracts.
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
    // The property orders columns by their SELECT-list position; "pk, seq0"
    // is the canonical order for the SELECT in `explain_target_sql`. The
    // the exact value to enforce stable ordering.
    REQUIRE(value.value() == "pk, seq0");
}

namespace {
/** Require the Bioseq custom scan to survive one planner GUC setting. */
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
                                       "'AAAA'::bioseq.seq('dna4', 32)) ORDER BY pk;",
                                       k_table));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
    REQUIRE(plan_contains(plan, "actual time"));
}

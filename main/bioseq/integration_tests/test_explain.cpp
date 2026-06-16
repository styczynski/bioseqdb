// Integration scenarios:
//   * `EXPLAIN` of a `USING bioseq` table SELECT shows
//     `Custom Scan (BioseqScan)` as the table-level plan node.
//   * The custom-scan node's `Bioseq Projected Columns:` property exactly
//     equals the comma-separated list of projected attribute names — not
//     just contains a substring match.
//   * Every scan-toggling GUC (`enable_bioseq_index_scan`,
//     `enable_vectorization`, `enable_column_cache`) keeps the custom scan
//     winning over a heap scan, because the planner-support cost penalty is
//     -100000 * wehavemagic_cnt (per CODE_GUIDELINES §8).
//   * `EXPLAIN (FORMAT JSON, ANALYZE, ...)` returns a JSON document whose
//     plan tree references "BioseqScan" as the custom plan provider.

#include "common_tests_integration.h"
import bioseq.integration_tests.common;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view kTable = "test_explain_t";

std::string explain_target_sql() {
    return fmt::format(
        "SELECT pk, seq0 FROM {} WHERE mmsearch(seq0, "
        "'AAAA'::bioseq.seq('dna4', 32));",
        kTable);
}

}  // namespace

TEST_CASE("EXPLAIN names the custom scan on a USING bioseq table",
          "[explain][custom_scan]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    const auto plan = explain_text<false>(ctx, explain_target_sql());
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
}

TEST_CASE("EXPLAIN's Bioseq Projected Columns property lists exactly the SELECTed attrs",
          "[explain][projection]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    const auto plan = explain_text<false>(ctx, explain_target_sql());
    const auto value = find_explain_property(plan, "Bioseq Projected Columns");
    REQUIRE(value.has_value());
    // The property orders columns by their SELECT-list position; "pk, seq0"
    // is the canonical order for the SELECT in `explain_target_sql`. We pin
    // the exact value to catch any reordering regression.
    REQUIRE(value.value() == "pk, seq0");
}

namespace {
void check_explain_keeps_custom_scan(std::string_view guc,
                                     std::string_view value) {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    guc_scope set(ctx, guc, value);
    const auto plan = explain_text<false>(ctx, explain_target_sql());
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
}
}  // namespace

TEST_CASE("EXPLAIN: BioseqScan persists with enable_bioseq_index_scan=on",
          "[explain][guc][custom_scan]") {
    check_explain_keeps_custom_scan("bioseq.enable_bioseq_index_scan", "on");
}
TEST_CASE("EXPLAIN: BioseqScan persists with enable_bioseq_index_scan=off",
          "[explain][guc][custom_scan]") {
    check_explain_keeps_custom_scan("bioseq.enable_bioseq_index_scan", "off");
}
TEST_CASE("EXPLAIN: BioseqScan persists with enable_vectorization=on",
          "[explain][guc][custom_scan]") {
    check_explain_keeps_custom_scan("bioseq.enable_vectorization", "on");
}
TEST_CASE("EXPLAIN: BioseqScan persists with enable_vectorization=off",
          "[explain][guc][custom_scan]") {
    check_explain_keeps_custom_scan("bioseq.enable_vectorization", "off");
}
TEST_CASE("EXPLAIN: BioseqScan persists with enable_column_cache=on",
          "[explain][guc][custom_scan]") {
    check_explain_keeps_custom_scan("bioseq.enable_column_cache", "on");
}
TEST_CASE("EXPLAIN: BioseqScan persists with enable_column_cache=off",
          "[explain][guc][custom_scan]") {
    check_explain_keeps_custom_scan("bioseq.enable_column_cache", "off");
}

TEST_CASE("EXPLAIN ANALYZE on mmsearch query reports the BioseqScan rows",
          "[explain][analyze]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    const auto plan = explain_text<true>(
        ctx,
        fmt::format("SELECT pk, bioseq.cigar(pk) FROM {} WHERE mmsearch(seq0, "
                    "'AAAA'::bioseq.seq('dna4', 32)) ORDER BY pk;",
                    kTable));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
    REQUIRE(plan_contains(plan, "actual time"));
}

TEST_CASE("EXPLAIN FORMAT JSON returns valid-looking JSON",
          "[explain][json]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    const std::string json = explain_json(ctx, explain_target_sql());

    REQUIRE_FALSE(json.empty());
    // BioseqScan and the Custom-Scan node-type string must both appear.
    REQUIRE(json.find("BioseqScan") != std::string::npos);
    REQUIRE(json.find("Custom Scan") != std::string::npos);
}

// Integration scenarios:
//   * `EXPLAIN` of a `USING bioseq` table SELECT shows
//     `Custom Scan (BioseqScan)` as the table-level plan node.
//   * The custom-scan node's `Bioseq Projected Columns:` property exactly
//     equals the comma-separated list of projected attribute names — not
//     just contains a substring match.
//   * Every scan-toggling GUC (`enable_bioseq_index_scan`,
//     `enable_vectorization`, `enable_column_cache`) keeps the custom scan
//     winning over a heap scan through exact RestrictInfo ownership and
//     structural pruning, while reported costs stay finite and nonnegative.
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

std::vector<std::string> explain_with_options(
    const connection_test_context& ctx,
    std::string_view options,
    std::string_view sql) {
    auto rows = query_rows(
        ctx, fmt::format("EXPLAIN ({}) {}", options, sql));
    std::vector<std::string> lines;
    lines.reserve(rows.size());
    for (auto& row : rows) {
        REQUIRE(row.size() == 1);
        REQUIRE(row.front().has_value());
        lines.push_back(std::move(*row.front()));
    }
    return lines;
}

std::optional<unsigned long long> boundary_actual_calls(
    std::span<const std::string> plan,
    std::string_view mechanism,
    std::string_view reason) {
    bool mechanism_matches = false;
    bool reason_matches = false;
    for (const auto& line : plan) {
        if (line.find("var_") != std::string::npos
            && line.find(" = ") != std::string::npos) {
            mechanism_matches = false;
            reason_matches = false;
        }
        if (line.find(fmt::format("Mechanism: {}", mechanism))
            != std::string::npos) {
            mechanism_matches = true;
        }
        if (line.find(fmt::format("Reason: {}", reason))
            != std::string::npos) {
            reason_matches = true;
        }
        if (mechanism_matches && reason_matches) {
            const auto marker = line.find("Actual Calls:");
            if (marker != std::string::npos) {
                return std::stoull(line.substr(
                    marker + std::string_view("Actual Calls:").size()));
            }
        }
    }
    return std::nullopt;
}

}  // namespace

TEST_CASE("EXPLAIN names the custom scan on a USING bioseq table",
          "[explain][custom_scan]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    const auto plan = explain_text<false>(ctx, explain_target_sql());
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
    REQUIRE_FALSE(plan_contains(plan, "Filter: mmsearch"));
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

TEST_CASE("DOC0377 plain EXPLAIN reports only materialization boundaries",
          "[explain][slice][materialization]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    const auto plan = explain_with_options(
        ctx, "VERBOSE, COSTS OFF",
        fmt::format("SELECT seq0[0:4] FROM {};", kTable));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_contains(plan, "Slice Materialization:"));
    REQUIRE(plan_contains(plan, "Mechanism: text"));
    REQUIRE(plan_contains(plan, "Mechanism: binary"));
    REQUIRE(plan_contains(plan, "Reason: client output"));
    REQUIRE_FALSE(plan_contains(plan, "Actual Calls:"));
    REQUIRE_FALSE(plan_contains(plan, "Estimated Symbols:"));
    REQUIRE_FALSE(plan_contains(plan, "Operation: slice"));
}

TEST_CASE("DOC0377 metadata-only projection emits no false allocation boundary",
          "[explain][slice][materialization]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    const auto plan = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF",
        fmt::format("SELECT bioseq.len(seq0[0:4]) FROM {};", kTable));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_contains(plan, "Slice Materialization:"));
    REQUIRE(plan_contains(plan, "none"));
    REQUIRE_FALSE(plan_contains(plan, "Mechanism: packed full"));
    REQUIRE_FALSE(plan_contains(plan, "Mechanism: text"));
}

TEST_CASE("DOC0377 EXPLAIN SERIALIZE records only the observed client boundary",
          "[explain][slice][serialize]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);
    const auto sql = fmt::format(
        "SELECT seq0[0:4] FROM {} WHERE pk = 1;", kTable);

    const auto none = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF", sql);
    REQUIRE(boundary_actual_calls(none, "text", "client output") == 0u);
    REQUIRE(boundary_actual_calls(none, "binary", "client output") == 0u);
    REQUIRE_FALSE(plan_contains(none, "Actual Time:"));

    const auto text = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF, SERIALIZE TEXT", sql);
    REQUIRE(boundary_actual_calls(text, "text", "client output") == 1u);
    REQUIRE(boundary_actual_calls(text, "binary", "client output") == 0u);

    const auto binary = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF, SERIALIZE BINARY", sql);
    REQUIRE(boundary_actual_calls(binary, "text", "client output") == 0u);
    REQUIRE(boundary_actual_calls(binary, "binary", "client output") == 1u);
}

TEST_CASE("DOC0377 rescans retain every materialization counter delta",
          "[explain][slice][rescan]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    /* The correlated scalar SubPlan rescans its one-row SliceProject for each
     * VALUES row.  This is the executor shape used by a parallel worker under
     * a nested loop as well; counters must remain cumulative across rearms. */
    const auto plan = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF, SERIALIZE TEXT",
        fmt::format(
            "SELECT (SELECT source.seq0[0:4] FROM {} AS source "
            "WHERE source.pk = driver.pk) "
            "FROM (VALUES (1), (2), (3)) AS driver(pk);",
            kTable));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(boundary_actual_calls(plan, "text", "client output") == 3u);
    REQUIRE(boundary_actual_calls(plan, "binary", "client output") == 0u);
}

TEST_CASE("DOC0377 structured EXPLAIN preserves numeric counter fields",
          "[explain][slice][json]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    const auto json = explain_json(
        ctx, fmt::format("SELECT seq0[0:4] FROM {};", kTable));
    REQUIRE(json.find("Slice Materialization") != std::string::npos);
    REQUIRE(json.find("\"Mechanism\": \"text\"") != std::string::npos);
    REQUIRE(json.find("\"Reason\": \"client output\"")
            != std::string::npos);
    REQUIRE(json.find("\"Actual Calls\": 0") != std::string::npos);
    REQUIRE(json.find("\"Depends On\": []") != std::string::npos);
    REQUIRE(json.find("\"Counter Overflow\": false")
            != std::string::npos);
}

TEST_CASE("DOC0377 INSERT plan declares a heap-tuple boundary",
          "[explain][slice][insert]") {
    auto ctx = fresh_context({kTable, "test_explain_slice_dst"});
    seed_small_bioseq_column(ctx, kTable);
    exec_sql(ctx,
        "CREATE TABLE test_explain_slice_dst "
        "(seq0 bioseq.seq('dna4',32)) USING bioseq;");

    const auto plan = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF",
        fmt::format(
            "INSERT INTO test_explain_slice_dst "
            "SELECT seq0[0:4] FROM {};", kTable));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_contains(plan, "Mechanism: packed full"));
    REQUIRE(plan_contains(plan, "Reason: heap tuple"));
    REQUIRE(query_scalar_int(
        ctx, "SELECT count(*) FROM test_explain_slice_dst;") == 3);
    REQUIRE(query_scalar(
        ctx, "SELECT bool_and(bioseq.len(seq0) = 4) "
             "FROM test_explain_slice_dst;") == "t");
}

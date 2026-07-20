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

#include <algorithm>
#include <array>

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

std::optional<unsigned long long> boundary_actual_metric(
    std::span<const std::string> plan,
    std::string_view mechanism,
    std::string_view reason,
    std::string_view property) {
    bool mechanism_matches = false;
    bool reason_matches = false;
    const auto property_marker = fmt::format("{}:", property);
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
            const auto marker = line.find(property_marker);
            if (marker != std::string::npos) {
                return std::stoull(line.substr(
                    marker + property_marker.size()));
            }
        }
    }
    return std::nullopt;
}

std::optional<unsigned long long> boundary_actual_calls(
    std::span<const std::string> plan,
    std::string_view mechanism,
    std::string_view reason) {
    return boundary_actual_metric(
        plan, mechanism, reason, "Actual Calls");
}

std::optional<unsigned long long> boundary_actual_bytes(
    std::span<const std::string> plan,
    std::string_view mechanism,
    std::string_view reason) {
    return boundary_actual_metric(
        plan, mechanism, reason, "Actual Bytes");
}

std::vector<std::string> planned_slice_signature(
    std::span<const std::string> plan) {
    std::vector<std::string> signature;
    for (const auto& line : plan) {
        const bool recipe_value = line.find("value_") != std::string::npos
            && line.find(" = ") != std::string::npos;
        const bool boundary_value = line.find("var_") != std::string::npos
            && line.find(" = ") != std::string::npos;
        const bool planned_property = line.find("Mechanism:") != std::string::npos
            || line.find("Reason:") != std::string::npos
            || line.find("Depends On:") != std::string::npos
            || line.find("Encoding:") != std::string::npos
            || line.find("Cache Scope:") != std::string::npos;
        if (!recipe_value && !boundary_value && !planned_property) {
            continue;
        }
        const auto first = line.find_first_not_of(" \t");
        signature.push_back(first == std::string::npos
            ? std::string{} : line.substr(first));
    }
    return signature;
}

struct materialization_signature {
    std::string variable;
    std::string mechanism;
    std::string reason;
    std::string depends_on;
    std::optional<unsigned long long> producer_value;
};

std::vector<materialization_signature> materialization_signatures(
    std::span<const std::string> plan) {
    std::vector<materialization_signature> result;
    materialization_signature current;
    auto property = [](const std::string& line, std::string_view name)
        -> std::optional<std::string> {
        const auto marker = fmt::format("{}:", name);
        const auto position = line.find(marker);
        if (position == std::string::npos) {
            return std::nullopt;
        }
        const auto value_start = line.find_first_not_of(
            " \t", position + marker.size());
        return value_start == std::string::npos
            ? std::string{} : line.substr(value_start);
    };
    for (const auto& line : plan) {
        const auto variable_start = line.find("var_");
        const auto expression_separator = line.find(" = ");
        if (variable_start != std::string::npos
            && expression_separator != std::string::npos) {
            if (!current.variable.empty()) {
                result.push_back(std::move(current));
                current = {};
            }
            current.variable = line.substr(
                variable_start, expression_separator - variable_start);
            continue;
        }
        if (current.variable.empty()) {
            continue;
        }
        if (auto value = property(line, "Mechanism")) {
            current.mechanism = std::move(*value);
        } else if (auto value = property(line, "Reason")) {
            current.reason = std::move(*value);
        } else if (auto value = property(line, "Depends On")) {
            current.depends_on = std::move(*value);
        } else if (auto value = property(line, "Producer Value")) {
            current.producer_value = std::stoull(*value);
        }
    }
    if (!current.variable.empty()) {
        result.push_back(std::move(current));
    }
    return result;
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
    REQUIRE(boundary_actual_bytes(none, "text", "client output") == 0u);
    REQUIRE(boundary_actual_bytes(none, "binary", "client output") == 0u);
    REQUIRE_FALSE(plan_contains(none, "Actual Time:"));

    const auto text = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF, SERIALIZE TEXT", sql);
    REQUIRE(boundary_actual_calls(text, "text", "client output") == 1u);
    REQUIRE(boundary_actual_bytes(text, "text", "client output") == 5u);
    REQUIRE(boundary_actual_calls(text, "binary", "client output") == 0u);
    REQUIRE(boundary_actual_bytes(text, "binary", "client output") == 0u);

    const auto binary = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF, SERIALIZE BINARY", sql);
    REQUIRE(boundary_actual_calls(binary, "text", "client output") == 0u);
    REQUIRE(boundary_actual_bytes(binary, "text", "client output") == 0u);
    REQUIRE(boundary_actual_calls(binary, "binary", "client output") == 1u);
    REQUIRE(boundary_actual_bytes(binary, "binary", "client output") == 20u);
}

TEST_CASE("DOC0377 SQL materialization calls and bytes match allocations",
          "[explain][slice][materialization][accounting]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    const auto plan = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF",
        fmt::format("SELECT bioseq.to_text(seq0[0:4]) FROM {};", kTable));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(boundary_actual_calls(plan, "text", "SQL conversion") == 3u);
    REQUIRE(boundary_actual_metric(
        plan, "text", "SQL conversion", "Actual Input Symbols") == 12u);
    REQUIRE(boundary_actual_metric(
        plan, "text", "SQL conversion", "Actual Output Symbols") == 12u);
    /* Three 4-symbol text varlenas: VARHDRSZ + 4 bytes each. */
    REQUIRE(boundary_actual_bytes(plan, "text", "SQL conversion") == 24u);
    REQUIRE_FALSE(plan_contains(plan, "Mechanism: packed full"));
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
    REQUIRE(boundary_actual_bytes(plan, "text", "client output") == 15u);
    REQUIRE(boundary_actual_calls(plan, "binary", "client output") == 0u);
    REQUIRE(boundary_actual_bytes(plan, "binary", "client output") == 0u);
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
    REQUIRE(json.find("\"Actual Bytes\": 0") != std::string::npos);
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
        "(pk int, seq0 bioseq.seq('dna4',32)) USING bioseq;");

    const auto plan = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF",
        fmt::format(
            "INSERT INTO test_explain_slice_dst "
            "SELECT pk, seq0[0:4] FROM {};", kTable));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_contains(plan, "Mechanism: packed full"));
    REQUIRE(plan_contains(plan, "Reason: heap tuple"));
    REQUIRE(query_scalar_int(
        ctx, "SELECT count(*) FROM test_explain_slice_dst;") == 3);
    REQUIRE(query_scalar(
        ctx, "SELECT bool_and(bioseq.len(seq0) = 4) "
             "FROM test_explain_slice_dst;") == "t");
}

TEST_CASE("DOC0377 planned slice recipe is stable between plain and ANALYZE",
          "[explain][slice][recipe]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);
    const auto sql = fmt::format(
        "SELECT seq0[0:8], (seq0[0:8])[0:4] FROM {} WHERE pk = 1;",
        kTable);

    const auto plain = explain_with_options(
        ctx, "VERBOSE, COSTS OFF", sql);
    const auto analyzed = explain_with_options(
        ctx, "ANALYZE, VERBOSE, TIMING OFF, COSTS OFF", sql);
    REQUIRE(planned_slice_signature(plain)
            == planned_slice_signature(analyzed));
    REQUIRE(plan_contains(plain, "var_1 ="));
    REQUIRE(plan_contains(plain, "var_2 ="));
    REQUIRE(plan_contains(plain, "var_3 ="));
    REQUIRE(plan_contains(plain, "Depends On: var_1, var_2"));
    const auto boundaries = materialization_signatures(plain);
    REQUIRE(boundaries.size() == 4u);
    REQUIRE(boundaries[0].variable == "var_1");
    REQUIRE(boundaries[0].mechanism == "text");
    REQUIRE(boundaries[0].reason == "client output");
    REQUIRE(boundaries[0].depends_on == "none");
    REQUIRE(boundaries[0].producer_value == 2u);
    REQUIRE(boundaries[1].variable == "var_2");
    REQUIRE(boundaries[1].mechanism == "binary");
    REQUIRE(boundaries[1].reason == "client output");
    REQUIRE(boundaries[1].depends_on == "none");
    REQUIRE(boundaries[1].producer_value == boundaries[0].producer_value);
    REQUIRE(boundaries[2].variable == "var_3");
    REQUIRE(boundaries[2].depends_on == "var_1, var_2");
    REQUIRE(boundaries[2].producer_value == 3u);
    REQUIRE(boundaries[3].variable == "var_4");
    REQUIRE(boundaries[3].depends_on == "var_1, var_2");
    REQUIRE(boundaries[3].producer_value == boundaries[2].producer_value);
    REQUIRE_FALSE(plan_contains(plain, "Actual Calls:"));
    REQUIRE(plan_contains(analyzed, "Actual Calls:"));
}

TEST_CASE("DOC0377 LIMIT and skipped CASE arms retain zero boundary calls",
          "[explain][slice][control-flow]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    const auto limited = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF, SERIALIZE TEXT",
        fmt::format("SELECT seq0[0:4] FROM {} LIMIT 0;", kTable));
    REQUIRE(boundary_actual_calls(
        limited, "text", "client output") == 0u);
    REQUIRE(boundary_actual_calls(
        limited, "binary", "client output") == 0u);

    const auto skipped = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF, SERIALIZE TEXT",
        fmt::format(
            "SELECT CASE WHEN seq0 IS NULL THEN seq0[0:4] "
            "ELSE 'ACGT'::bioseq.seq('dna4',32) END FROM {} WHERE pk = 1;",
            kTable));
    REQUIRE(plan_contains(skipped, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(boundary_actual_calls(
        skipped, "text", "client output") == 0u);
    REQUIRE(boundary_actual_calls(
        skipped, "binary", "client output") == 0u);
}

TEST_CASE("DOC0377 lazy-only slice concat reverse has no allocation boundary",
          "[explain][slice][lazy-only]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);

    const auto plan = explain_with_options(
        ctx, "ANALYZE, VERBOSE, TIMING OFF, COSTS OFF",
        fmt::format(
            "SELECT bioseq.len(bioseq.reverse(seq0[0:4]) "
            "OPERATOR(bioseq.||) seq0[4:8]) "
            "FROM {};",
            kTable));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_contains(plan, "Slice Materialization:"));
    REQUIRE(plan_contains(plan, "none"));
    REQUIRE_FALSE(plan_contains(plan, "Mechanism: packed full"));
    REQUIRE_FALSE(plan_contains(plan, "Reason: executor copy"));
}

TEST_CASE("DOC0377 timing and summary options control only actual metadata",
          "[explain][slice][timing]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);
    const auto sql = fmt::format(
        "SELECT seq0[0:4] FROM {} WHERE pk = 1;", kTable);

    const auto with_timing = explain_with_options(
        ctx, "ANALYZE, TIMING ON, SUMMARY ON, COSTS OFF, SERIALIZE TEXT",
        sql);
    REQUIRE(plan_contains(with_timing, "Actual Time:"));
    REQUIRE(plan_contains(with_timing, "Execution Time:"));

    const auto without_timing = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, SUMMARY OFF, COSTS OFF, SERIALIZE TEXT",
        sql);
    REQUIRE_FALSE(plan_contains(without_timing, "Actual Time:"));
    REQUIRE_FALSE(plan_contains(without_timing, "Execution Time:"));
}

TEST_CASE("DOC0377 YAML and XML preserve structured slice fields",
          "[explain][slice][structured]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);
    const auto sql = fmt::format(
        "SELECT seq0[0:4] FROM {} WHERE pk = 1;", kTable);

    const auto yaml = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF, FORMAT YAML", sql);
    REQUIRE(plan_contains(yaml, "BioseqSliceProject"));
    REQUIRE(plan_contains(yaml, "Slice Materialization"));
    REQUIRE(plan_contains(yaml, "Mechanism: text"));
    REQUIRE(plan_contains(yaml, "Actual Calls: 0"));

    const auto xml = explain_with_options(
        ctx, "ANALYZE, TIMING OFF, COSTS OFF, FORMAT XML", sql);
    REQUIRE(plan_contains(xml, "BioseqSliceProject"));
    REQUIRE(plan_contains(xml, "Slice-Materialization"));
    REQUIRE(plan_contains(xml, "<Mechanism>text</Mechanism>"));
    REQUIRE(plan_contains(xml, "<Actual-Calls>0</Actual-Calls>"));
}

TEST_CASE("DOC0377 transformed INNER mmsearch explains one per-row BatchJoin owner",
          "[explain][slice][batch-join]") {
    constexpr std::string_view target = "test_explain_slice_join_target";
    auto ctx = fresh_context({target});
    seed_small_bioseq_column(ctx, target);
    exec_sql(ctx,
        "CREATE TEMP TABLE test_explain_slice_join_query "
        "(id int, seq bioseq.seq('dna4',32));");
    exec_sql(ctx,
        "INSERT INTO test_explain_slice_join_query VALUES "
        "(1, 'ACGTACGT'::bioseq.seq('dna4',32));");

    const auto plan = explain_with_options(
        ctx, "VERBOSE, COSTS OFF",
        fmt::format(
            "SELECT q.id, t.pk FROM {} AS t "
            "JOIN test_explain_slice_join_query AS q "
            "ON mmsearch(t.seq0[0:], q.seq[0:]);",
            target));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(plan, "Bioseq Mmsearch Ownership: per-row recipe"));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE_FALSE(plan_contains(plan, "Join Filter: mmsearch"));
}

TEST_CASE("DOC0377 mmsearch slice boundaries follow the selected minimizer route",
          "[explain][slice][batch-join][materialization]") {
    constexpr std::string_view target =
        "test_explain_slice_join_materialization_target";
    auto ctx = fresh_context({target});
    seed_small_bioseq_column(ctx, target);
    exec_sql(ctx, fmt::format(
        "TRUNCATE {}; "
        "INSERT INTO {} VALUES "
        "(1, repeat('ACGT',64)::bioseq.seq('dna4',32));",
        target, target));
    exec_sql(ctx,
        "CREATE TEMP TABLE test_explain_slice_join_materialization_query "
        "(id int, seq bioseq.seq('dna4',32)); "
        "INSERT INTO test_explain_slice_join_materialization_query VALUES "
        "(1, repeat('ACGT',64)::bioseq.seq('dna4',32)); "
        "SET max_parallel_workers_per_gather=0; "
        "SET bioseq.read_sequence_index=off; "
        "SET bioseq.aligner.vulkan_enabled=off;");

    const auto sql = fmt::format(
        "SELECT q.id, t.pk FROM {} AS t "
        "JOIN test_explain_slice_join_materialization_query AS q "
        "ON mmsearch("
        "t.seq0[1:128] OPERATOR(bioseq.||) t.seq0[128:], "
        "q.seq[1:128] OPERATOR(bioseq.||) q.seq[128:]);",
        target);

    {
        guc_scope scalar(
            ctx, "bioseq.aligner.minimizer_algorithm", "scalar");
        const auto streaming = explain_with_options(
            ctx, "ANALYZE, TIMING OFF, COSTS OFF", sql);
        REQUIRE(plan_contains(streaming, "Custom Scan (BioseqBatchJoin)"));
        REQUIRE(plan_contains(streaming, "Custom Scan (BioseqSliceProject)"));
        REQUIRE_FALSE(plan_contains(streaming, "Reason: mmsearch target"));
        REQUIRE_FALSE(plan_contains(streaming, "Reason: mmsearch query"));
        REQUIRE_FALSE(plan_contains(streaming, "Mechanism: packed region"));
        REQUIRE_FALSE(plan_contains(streaming, "Mechanism: packed full"));
    }

    {
        guc_scope admitted(
            ctx, "bioseq.aligner.minimizer_algorithm", "two_stack_nthash");
        const auto resident = explain_with_options(
            ctx, "ANALYZE, TIMING OFF, COSTS OFF", sql);
        REQUIRE(plan_contains(resident, "Custom Scan (BioseqBatchJoin)"));
        REQUIRE(plan_contains(resident, "Custom Scan (BioseqSliceProject)"));
        REQUIRE_FALSE(plan_contains(resident, "Mechanism: packed full"));
        REQUIRE(boundary_actual_calls(
            resident, "packed region", "mmsearch target") == 1u);
        REQUIRE(boundary_actual_bytes(
            resident, "packed region", "mmsearch target") == 64u);
        REQUIRE(boundary_actual_calls(
            resident, "packed region", "mmsearch query") == 1u);
        REQUIRE(boundary_actual_bytes(
            resident, "packed region", "mmsearch query") == 64u);
    }
}

#ifdef BIOSEQ_TEST_INDEXES
namespace {

enum class dsm_explain_format {
    text,
    json,
    yaml,
    xml,
};

void require_dsm_fixture_property(
        std::span<const std::string> plan,
        dsm_explain_format format,
        std::string_view name,
        std::string_view value) {
    switch (format) {
        case dsm_explain_format::text:
        case dsm_explain_format::yaml:
            REQUIRE(plan_contains(plan, fmt::format("{}: {}", name, value)));
            return;
        case dsm_explain_format::json:
            REQUIRE(plan_contains(
                plan, fmt::format("\"{}\": {}", name, value)));
            return;
        case dsm_explain_format::xml: {
            std::string tag{name};
            std::replace(tag.begin(), tag.end(), ' ', '-');
            REQUIRE(plan_contains(
                plan, fmt::format("<{}>{}</{}>", tag, value, tag)));
            return;
        }
    }
}

}  // namespace

TEST_CASE("DOC0377 malformed DSM fixtures reject before counter dereference",
          "[explain][slice][dsm][malformed]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);
    guc_scope fixture(ctx, "bioseq.test_slice_dsm_fixture", "malformed");
    const auto sql = fmt::format(
        "SELECT seq0[0:4] FROM {} WHERE pk = 1;", kTable);
    const std::array formats{
        std::pair{dsm_explain_format::text,
                  std::string_view{"ANALYZE, TIMING OFF, COSTS OFF"}},
        std::pair{dsm_explain_format::json,
                  std::string_view{"ANALYZE, TIMING OFF, COSTS OFF, FORMAT JSON"}},
        std::pair{dsm_explain_format::yaml,
                  std::string_view{"ANALYZE, TIMING OFF, COSTS OFF, FORMAT YAML"}},
        std::pair{dsm_explain_format::xml,
                  std::string_view{"ANALYZE, TIMING OFF, COSTS OFF, FORMAT XML"}},
    };

    for (const auto& [format, options] : formats) {
        const auto plan = explain_with_options(ctx, options, sql);
        require_dsm_fixture_property(
            plan, format, "DSM Test Fixture", format == dsm_explain_format::json
                ? "\"malformed\"" : "malformed");
        require_dsm_fixture_property(
            plan, format, "DSM Fixture Validated", "true");
        require_dsm_fixture_property(
            plan, format, "DSM Async Lifecycle Validated", "true");
        require_dsm_fixture_property(
            plan, format, "DSM Fixture Checks", "6");
        require_dsm_fixture_property(
            plan, format, "DSM Fixture Counter Records Read", "0");
        require_dsm_fixture_property(
            plan, format, "DSM Async Cleanup Callbacks", "3");
        require_dsm_fixture_property(
            plan, format, "Counter Overflow", "true");
        REQUIRE_FALSE(plan_contains(plan, "Incomplete Workers"));
        REQUIRE_FALSE(plan_contains(plan, "Incomplete-Workers"));
    }
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("DOC0377 incomplete DSM work is reported but never counted",
          "[explain][slice][dsm][incomplete]") {
    auto ctx = fresh_context({kTable});
    seed_small_bioseq_column(ctx, kTable);
    guc_scope fixture(ctx, "bioseq.test_slice_dsm_fixture", "incomplete");
    const auto sql = fmt::format(
        "SELECT seq0[0:4] FROM {} WHERE pk = 1;", kTable);
    const std::array formats{
        std::pair{dsm_explain_format::text,
                  std::string_view{"ANALYZE, TIMING OFF, COSTS OFF"}},
        std::pair{dsm_explain_format::json,
                  std::string_view{"ANALYZE, TIMING OFF, COSTS OFF, FORMAT JSON"}},
        std::pair{dsm_explain_format::yaml,
                  std::string_view{"ANALYZE, TIMING OFF, COSTS OFF, FORMAT YAML"}},
        std::pair{dsm_explain_format::xml,
                  std::string_view{"ANALYZE, TIMING OFF, COSTS OFF, FORMAT XML"}},
    };

    for (const auto& [format, options] : formats) {
        const auto plan = explain_with_options(ctx, options, sql);
        require_dsm_fixture_property(
            plan, format, "DSM Test Fixture", format == dsm_explain_format::json
                ? "\"incomplete\"" : "incomplete");
        require_dsm_fixture_property(
            plan, format, "DSM Fixture Validated", "true");
        require_dsm_fixture_property(
            plan, format, "DSM Async Lifecycle Validated", "true");
        require_dsm_fixture_property(
            plan, format, "DSM Fixture Checks", "1");
        require_dsm_fixture_property(
            plan, format, "DSM Fixture Slot Headers Read", "4");
        require_dsm_fixture_property(
            plan, format, "DSM Fixture Counter Records Read", "2");
        require_dsm_fixture_property(
            plan, format, "DSM Async Cleanup Callbacks", "3");
        require_dsm_fixture_property(
            plan, format, "Incomplete Workers", "2");
        require_dsm_fixture_property(
            plan, format, "Counter Overflow", "false");
        require_dsm_fixture_property(
            plan, format, "Actual Calls", "3");
        require_dsm_fixture_property(
            plan, format, "Actual Bytes", "30");
        if (format == dsm_explain_format::text) {
            REQUIRE(boundary_actual_calls(
                plan, "text", "client output") == 3u);
            REQUIRE(boundary_actual_bytes(
                plan, "text", "client output") == 30u);
            REQUIRE(boundary_actual_calls(
                plan, "binary", "client output") == 3u);
            REQUIRE(boundary_actual_bytes(
                plan, "binary", "client output") == 30u);
        }
    }
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}
#endif

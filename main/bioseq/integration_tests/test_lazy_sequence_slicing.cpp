// DOC0377: SQL-level slicing/letter/algebra/bridge and durable-boundary tests.

#include "common_tests_integration.h"
#include "headers/pg_build_config.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

import bioseq.integration_tests.common;

using namespace bioseq_int_tests;

namespace {

std::optional<std::string> boundary_explain_property(
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
        if (!mechanism_matches || !reason_matches) {
            continue;
        }
        const auto marker = line.find(property_marker);
        if (marker == std::string::npos) {
            continue;
        }
        const auto value_start = line.find_first_not_of(
            " \t", marker + property_marker.size());
        return value_start == std::string::npos
            ? std::string{} : line.substr(value_start);
    }
    return std::nullopt;
}

}  // namespace

TEST_CASE("DOC0377 trace ABI counters scopes refs and reusable slots are exact",
          "[seq][slice][trace][unit]") {
    auto ctx = fresh_context();
    REQUIRE(query_scalar(
        ctx, "SELECT bioseq_internal.slice_trace_self_test();") == "t");
}

TEST_CASE("DOC0377 Python coordinates and letter access are exact",
          "[seq][slice][letter]") {
    auto ctx = fresh_context();
    const auto row = query_one(
        ctx,
        "WITH v(s) AS (VALUES "
        " ('CTAGT'::bioseq.seq('dna4',32))) "
        "SELECT s[:]::text, s[-4:-1]::text, s[-100:100]::text, "
        "       s[3:1]::text, s[0]::text, s[-1]::text, "
        "       s[5] IS NULL, s[-6] IS NULL "
        "FROM v;");
    REQUIRE(row[0] == "CTAGT");
    REQUIRE(row[1] == "TAG");
    REQUIRE(row[2] == "CTAGT");
    REQUIRE(row[3] == "");
    REQUIRE(row[4] == "C");
    REQUIRE(row[5] == "T");
    REQUIRE(row[6] == "t");
    REQUIRE(row[7] == "t");

    const auto nulls = query_one(
        ctx,
        "WITH v(s) AS (VALUES ('ACGT'::bioseq.seq('dna4',32))) "
        "SELECT s[NULL::bigint:2]::text, s[:NULL::bigint]::text, "
        "       s[NULL::bigint]::text FROM v;");
    REQUIRE_FALSE(nulls[0].has_value());
    REQUIRE_FALSE(nulls[1].has_value());
    REQUIRE_FALSE(nulls[2].has_value());

    REQUIRE(query_scalar(ctx,
        "SELECT ('AC'::bioseq.seq)[9] IS NULL;") == "t");
    const auto unbound = sql_error(
        ctx, "SELECT ('AC'::bioseq.seq)[0];");
    REQUIRE(unbound.sqlstate == "42804");
}

TEST_CASE("DOC0377 letter validation hook chains and walks nested queries",
          "[seq][letter][hook][typmod]") {
    auto ctx = fresh_context({"doc0377_letter_hook_ctas"});
    exec_sql(ctx, "SET statement_timeout='2s';");

    REQUIRE(query_scalar(
        ctx,
        "SELECT current_setting('shared_preload_libraries') LIKE "
        "       '%auth_permission_dialog%' "
        "   AND current_setting('shared_preload_libraries') LIKE '%bioseq%';")
        == "t");

    const auto simple_before = query_scalar_int(
        ctx, "SELECT bioseq_internal.letter_hook_chain_calls();");
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
    const auto simple_after = query_scalar_int(
        ctx, "SELECT bioseq_internal.letter_hook_chain_calls();");
    // The second diagnostic query contributes one call itself.  The other
    // delta entry is the probe SELECT, proving bioseq is reached exactly once
    // through the installed preload-hook chain rather than skipped or
    // recursively re-entered.  This remains exact whichever preload hook was
    // installed last and therefore owns the outermost callback.
    REQUIRE(simple_after - simple_before == 2);

    const auto nested_before = query_scalar_int(
        ctx, "SELECT bioseq_internal.letter_hook_chain_calls();");
    REQUIRE(query_scalar_int(
        ctx,
        "WITH first_level(n) AS (SELECT 1), "
        "second_level(n) AS (SELECT n + 1 FROM first_level) "
        "SELECT n FROM second_level;") == 2);
    const auto nested_after = query_scalar_int(
        ctx, "SELECT bioseq_internal.letter_hook_chain_calls();");
    REQUIRE(nested_after - nested_before == 2);

    for (const char* utility_sql : {
             "EXPLAIN SELECT 'A'::bioseq.letter;",
             "PREPARE doc0377_letter_hook_prepare AS "
             "SELECT 'A'::bioseq.letter;",
             "CREATE TABLE public.doc0377_letter_hook_ctas AS "
             "SELECT 'A'::bioseq.letter AS value;",
         }) {
        CAPTURE(utility_sql);
        const auto error = sql_error(ctx, utility_sql);
        REQUIRE(error.sqlstate == "22023");
        REQUIRE(error.contains(
            "bioseq.letter input requires an alphabet typmod"));
    }
    exec_sql(ctx, "BEGIN;");
    const auto cursor_error = sql_error(
        ctx,
        "DECLARE doc0377_letter_hook_cursor CURSOR FOR "
        "SELECT 'A'::bioseq.letter;");
    REQUIRE(cursor_error.sqlstate == "22023");
    exec_sql(ctx, "ROLLBACK;");
    exec_sql(ctx, "RESET statement_timeout;");
}

TEST_CASE("DOC0377 letter typmods resolve literals and validate physical alphabets",
          "[seq][letter][typmod][array]") {
    auto ctx = fresh_context({"doc0377_typmodless_letter",
                              "doc0377_typed_letter"});

    REQUIRE(query_scalar(
        ctx, "SELECT ('A'::bioseq.letter('dna4'))::text;") == "A");
    REQUIRE(query_scalar(
        ctx,
        "SELECT ((ARRAY['A'::bioseq.letter('dna4'), "
        "               'C'::bioseq.letter('dna4')])[2])::text;") == "C");

    // Element expressions lose their declared typmod, but the two physical
    // bytes remain authoritative and must stay usable without relabeling.
    REQUIRE(query_scalar(
        ctx,
        "SELECT (('AC'::bioseq.seq('dna4',32))[1])::text;") == "C");

#if BIOSEQ_OPTTIMIZATION_LEVEL > 0
    const auto mismatched_typmod = sql_error(
        ctx,
        "SELECT (('A'::bioseq.letter('dna4'))"
        "        ::bioseq.letter('dna5'))::text;");
    REQUIRE(mismatched_typmod.sqlstate == "42804");
    REQUIRE(mismatched_typmod.contains(
        "cannot relabel bioseq.letter to a different alphabet"));
#else
    // DOC0377 section 17.2 requires the dna4/dna5 matrix.  The repository's
    // mandated opt0 iteration build intentionally registers only dna4, so it
    // must report that exact configuration boundary rather than silently skip
    // the query.  The cross-alphabet 42804 branch remains a required opt>0
    // acceptance run before final handoff.
    const auto unavailable_dna5 = sql_error(
        ctx, "SELECT 'A'::bioseq.letter('dna5');");
    REQUIRE(unavailable_dna5.sqlstate == "22023");
    REQUIRE(unavailable_dna5.contains("is not supported"));
#endif

    const auto typmodless_literal = sql_error(
        ctx, "SELECT 'A'::bioseq.letter;");
    REQUIRE(typmodless_literal.sqlstate == "22023");
    REQUIRE(typmodless_literal.contains(
        "bioseq.letter input requires an alphabet typmod"));
    const auto typmodless_values = sql_error(
        ctx, "VALUES ('A'::bioseq.letter);");
    REQUIRE(typmodless_values.sqlstate == "22023");
    REQUIRE(typmodless_values.contains(
        "bioseq.letter input requires an alphabet typmod"));

    // Unknown literals enter the base input function before PostgreSQL applies
    // the same-type typmod cast.  The analyzed-tree guard consumes that token
    // only under a positive typmod cast and rejects it before a typmodless heap
    // target can persist it.  An already-bound physical letter remains valid.
    exec_sql(ctx,
        "CREATE TABLE doc0377_typmodless_letter "
        " (value bioseq.letter);");
    const auto stored_typmodless_literal = sql_error(
        ctx, "INSERT INTO doc0377_typmodless_letter VALUES ('A');");
    REQUIRE(stored_typmodless_literal.sqlstate == "22023");
    REQUIRE(stored_typmodless_literal.contains(
        "bioseq.letter input requires an alphabet typmod"));
    exec_sql(ctx,
        "INSERT INTO doc0377_typmodless_letter VALUES "
        " ('A'::bioseq.letter('dna4'));");
    REQUIRE(query_scalar(
        ctx, "SELECT value::text FROM doc0377_typmodless_letter;") == "A");

    exec_sql(ctx,
        "CREATE TABLE doc0377_typed_letter "
        " (value bioseq.letter('dna4')); "
        "INSERT INTO doc0377_typed_letter VALUES ('C');");
    REQUIRE(query_scalar(
        ctx, "SELECT value::text FROM doc0377_typed_letter;") == "C");
}

TEST_CASE("DOC0377 nested slice concat and transforms preserve logical value",
          "[seq][slice][concat][reverse]") {
    auto ctx = fresh_context();
    exec_sql(ctx, "SET search_path TO pg_catalog, public;");
    const auto unqualified_concat = sql_error(
        ctx,
        "SELECT ('AC'::bioseq.seq('dna4',32)) "
        "       || ('GT'::bioseq.seq('dna4',32));");
    REQUIRE(unqualified_concat.sqlstate == "42883");
    REQUIRE(query_scalar(
        ctx,
        "SELECT (('AC'::bioseq.seq('dna4',32)) "
        "        OPERATOR(bioseq.||) "
        "        ('GT'::bioseq.seq('dna4',32)))::text;") == "ACGT");

    REQUIRE(query_scalar(
        ctx,
        "SELECT (((('ACGTACGTACGT'::bioseq.seq('dna4',32))[:10])[5:] "
        "       OPERATOR(bioseq.||) "
        "       ('TTTTCCCCAAAA'::bioseq.seq('dna4',32))[5:10])"
        "       [1:-1])::text;") == "GTACCCCA");
    REQUIRE(query_scalar(
        ctx,
        "SELECT bioseq.reverse("
        " 'AAGC'::bioseq.seq('dna4',32))::text;") == "CGAA");
    REQUIRE(query_scalar(
        ctx,
        "SELECT bioseq.reverse_complement("
        " 'AAGC'::bioseq.seq('dna4',32))::text;") == "GCTT");

    REQUIRE(query_scalar(
        ctx,
        "WITH a AS MATERIALIZED ("
        "  SELECT 'ACGTACGT'::bioseq.seq('dna4',32) AS s), "
        "b AS MATERIALIZED ("
        "  SELECT s[1:7] OPERATOR(bioseq.||) "
        "         bioseq.reverse(s[0:2]) AS s FROM a) "
        "SELECT (SELECT s::text FROM b) || (SELECT s::text FROM b);")
        == "CGTACGCACGTACGCA");
}

#if BIOSEQ_OPTTIMIZATION_LEVEL >= 1
TEST_CASE("DOC0377 concat mismatch diagnostics name both alphabets",
          "[seq][concat][diagnostic]") {
    auto ctx = fresh_context();
    const auto mismatch = sql_error(
        ctx,
        "SELECT 'AC'::bioseq.seq('dna4',64) "
        "       OPERATOR(bioseq.||) "
        "       'GT'::bioseq.seq('dna5',64);");
    REQUIRE(mismatch.sqlstate == "42804");
    REQUIRE(mismatch.contains("dna4"));
    REQUIRE(mismatch.contains("dna5"));
}
#endif

TEST_CASE("DOC0377 TOAST metadata casts and element access are bounded",
          "[seq][slice][toast][cast][letter]") {
    auto ctx = fresh_context({"doc0377_toast_seq"});
    exec_sql(ctx,
        "CREATE TABLE doc0377_toast_seq ("
        " id int, s bioseq.seq('dna4',64));"
        "ALTER TABLE doc0377_toast_seq ALTER COLUMN s SET STORAGE EXTERNAL;"
        "INSERT INTO doc0377_toast_seq VALUES "
        " (1, repeat('ACGT',300000)::bioseq.seq('dna4',64));");

    REQUIRE(query_scalar(
        ctx,
        "SELECT reltoastrelid <> 0 "
        "       AND pg_total_relation_size(reltoastrelid) > 8192 "
        "FROM pg_catalog.pg_class "
        "WHERE oid='doc0377_toast_seq'::regclass;") == "t");
    const auto row = query_one(
        ctx,
        "SELECT bioseq.len(s), s[1199999]::text, "
        "       bioseq.len(bioseq.seq_cast("
        "           s, (SELECT atttypmod FROM pg_catalog.pg_attribute "
        "               WHERE attrelid='doc0377_toast_seq'::regclass "
        "                 AND attname='s'), false)), "
        "       bioseq.len(bioseq.seq_cast(s, -1, false)) "
        "FROM doc0377_toast_seq;");
    REQUIRE(row[0] == "1200000");
    REQUIRE(row[1] == "T");
    REQUIRE(row[2] == "1200000");
    REQUIRE(row[3] == "1200000");
}

TEST_CASE("DOC0377 explicit text and array bridges validate and round-trip",
          "[seq][concat][array]") {
    auto ctx = fresh_context();
    REQUIRE(query_scalar(
        ctx,
        "SELECT (('AC'::bioseq.seq('dna4',32)) "
        " OPERATOR(bioseq.||) ARRAY['G','TAC']::text[])::text;")
        == "ACGTAC");
    REQUIRE(query_scalar(
        ctx,
        "SELECT (ARRAY['A'::bioseq.letter('dna4'),"
        "              'C'::bioseq.letter('dna4')] "
        " OPERATOR(bioseq.||) "
        " 'GT'::bioseq.seq('dna4',32))::text;") == "ACGT");
    REQUIRE(query_scalar(
        ctx,
        "SELECT array_to_string(bioseq.to_array("
        " 'ACGT'::bioseq.seq('dna4',32)), '');") == "ACGT");
    REQUIRE(query_scalar(
        ctx,
        "SELECT bioseq.from_array(ARRAY["
        " 'A'::bioseq.letter('dna4'), 'C'::bioseq.letter('dna4')],"
        " 32)::text;") == "AC");
    REQUIRE(query_scalar(
        ctx,
        "SELECT bioseq.from_array(ARRAY[]::bioseq.letter[],"
        " 'dna4', 32)::text;") == "");
    const auto null_element = sql_error(
        ctx,
        "SELECT ('A'::bioseq.seq('dna4',32)) "
        " OPERATOR(bioseq.||) ARRAY['C'::text,NULL::text];");
    REQUIRE(null_element.sqlstate == "22004");
}

TEST_CASE("DOC0377 sliced source concatenations survive INSERT boundaries",
          "[seq][slice][storage][materialization]") {
    auto ctx = fresh_context({"doc0377_slice_src", "doc0377_slice_dst"});
    exec_sql(ctx,
        "CREATE TABLE doc0377_slice_src "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;");
    exec_sql(ctx,
        "INSERT INTO doc0377_slice_src VALUES "
        " (1, repeat('ACGT',262144)::bioseq.seq('dna4',32));");
    exec_sql(ctx,
        "CREATE TABLE doc0377_slice_dst "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;");
    exec_sql(ctx,
        "INSERT INTO doc0377_slice_dst "
        "SELECT id, s[500000:500064] OPERATOR(bioseq.||) s[-64:] "
        "FROM doc0377_slice_src;");
    REQUIRE(query_scalar_int(
        ctx, "SELECT bioseq.len(s) FROM doc0377_slice_dst;") == 128);
    REQUIRE(query_scalar(
        ctx,
        "SELECT s::text = repeat('ACGT',32) "
        "FROM doc0377_slice_dst;") == "t");
    REQUIRE(query_scalar(
        ctx,
        "SELECT bioseq.seq_send(s[0:64]) = "
        "       bioseq.seq_send(repeat('ACGT',16)::"
        "                          bioseq.seq('dna4',32)) "
        "FROM doc0377_slice_dst;") == "t");
    REQUIRE(query_scalar(
        ctx,
        "SELECT array_to_string(bioseq.to_array(s[0:8]),'') "
        "FROM doc0377_slice_dst;") == "ACGTACGT");
    constexpr auto literal_left_sql =
        "SELECT octet_length(bioseq.to_text("
        " text 'TG' OPERATOR(bioseq.||) s[0:4])) "
        "FROM doc0377_slice_dst;";
    constexpr auto array_left_sql =
        "SELECT octet_length(bioseq.to_text("
        " ARRAY['T','G']::text[] OPERATOR(bioseq.||) s[0:4])) "
        "FROM doc0377_slice_dst;";
    REQUIRE(query_scalar(
        ctx,
        "SELECT (text 'TG' OPERATOR(bioseq.||) s[0:4])::text "
        "FROM doc0377_slice_dst;") == "TGACGT");
    REQUIRE(query_scalar(
        ctx,
        "SELECT (ARRAY['T','G']::text[] "
        "        OPERATOR(bioseq.||) s[0:4])::text "
        "FROM doc0377_slice_dst;") == "TGACGT");
    REQUIRE(query_scalar_int(ctx, literal_left_sql) == 6);
    REQUIRE(query_scalar_int(ctx, array_left_sql) == 6);
    const std::array<const char*, 2> provenance_sql{
        literal_left_sql, array_left_sql};
    for (const char* sql : provenance_sql) {
        const auto provenance_plan = explain_text(ctx, sql);
        REQUIRE(plan_contains(provenance_plan, "Mechanism: text"));
        REQUIRE(plan_contains(provenance_plan, "Reason: SQL conversion"));
        REQUIRE(boundary_explain_property(
            provenance_plan, "text", "SQL conversion", "Actual Calls")
            == "1");
    }

    const auto plan = explain_text(
        ctx,
        "SELECT s[500000:500064] FROM doc0377_slice_src;");
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_contains(plan, "Slice Materialization:"));
    REQUIRE(plan_contains(plan, "s[500000:500064]"));
    REQUIRE(plan_contains(plan, "Mechanism: text"));
    REQUIRE(plan_contains(plan, "Reason: client output"));
    REQUIRE(plan_contains(plan, "Planned Materialized Symbols"));
    REQUIRE(plan_contains(plan, "Planned Materialized Bytes"));
    REQUIRE(plan_contains(plan, "Actual Calls"));
}

TEST_CASE("DOC0377 long source slice is faster than whole-sequence materialization",
          "[seq][slice][storage][performance]") {
    auto ctx = fresh_context({"doc0377_slice_perf"});
    exec_sql(ctx,
        "SET jit = off; "
        "SET bioseq.enable_column_cache = off; "
        "SET bioseq.enable_vectorization = off;");
    exec_sql(ctx,
        "CREATE TABLE doc0377_slice_perf "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;");
    exec_sql(ctx,
        "INSERT INTO doc0377_slice_perf VALUES "
        " (1, repeat('ACGT',2097152)::bioseq.seq('dna4',32));");

    constexpr auto slice_sql =
        "SELECT octet_length(bioseq.to_text(s[4194304:4194368])) "
        "FROM doc0377_slice_perf;";
    constexpr auto whole_sql =
        "SELECT octet_length(bioseq.to_text(s)) "
        "FROM doc0377_slice_perf;";
    REQUIRE(query_scalar_int(ctx, slice_sql) == 64);
    REQUIRE(query_scalar_int(ctx, whole_sql) == 8388608);

    const auto timed_ns = [&ctx](const char* sql, std::int64_t expected) {
        const auto started = std::chrono::steady_clock::now();
        const auto value = query_scalar_int(ctx, sql);
        const auto stopped = std::chrono::steady_clock::now();
        REQUIRE(value == expected);
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            stopped - started).count();
    };

    std::array<std::int64_t, 7> slice_ns{};
    std::array<std::int64_t, 7> whole_ns{};
    for (std::size_t round = 0; round < slice_ns.size(); ++round) {
        if ((round & 1u) == 0u) {
            whole_ns[round] = timed_ns(whole_sql, 8388608);
            slice_ns[round] = timed_ns(slice_sql, 64);
        } else {
            slice_ns[round] = timed_ns(slice_sql, 64);
            whole_ns[round] = timed_ns(whole_sql, 8388608);
        }
    }
    std::sort(slice_ns.begin(), slice_ns.end());
    std::sort(whole_ns.begin(), whole_ns.end());
    const auto slice_median_ns = slice_ns[slice_ns.size() / 2u];
    const auto whole_median_ns = whole_ns[whole_ns.size() / 2u];
    INFO("slice median ns=" << slice_median_ns
         << ", whole median ns=" << whole_median_ns);
    REQUIRE(slice_median_ns > 0);
    REQUIRE(whole_median_ns > 0);
    REQUIRE(slice_median_ns <= whole_median_ns / 2);

    const auto trace_plan = explain_text(ctx, slice_sql);
    REQUIRE(plan_contains(trace_plan, "Slice Materialization:"));
    REQUIRE(plan_contains(trace_plan, "Mechanism: text"));
    REQUIRE(plan_contains(trace_plan, "Reason: SQL conversion"));
    REQUIRE(boundary_explain_property(
        trace_plan, "text", "SQL conversion", "Actual Calls") == "1");
    const auto traced_input = boundary_explain_property(
        trace_plan, "text", "SQL conversion", "Actual Input Symbols");
    const auto traced_output = boundary_explain_property(
        trace_plan, "text", "SQL conversion", "Actual Output Symbols");
    const auto traced_bytes = boundary_explain_property(
        trace_plan, "text", "SQL conversion", "Actual Bytes");
    REQUIRE(traced_input.has_value());
    REQUIRE(traced_output.has_value());
    REQUIRE(traced_bytes.has_value());
    REQUIRE(std::stoull(*traced_input) <= 64u);
    REQUIRE(std::stoull(*traced_output) <= 64u);
    REQUIRE(std::stoull(traced_bytes->substr(
        0, traced_bytes->find(' '))) <= 128u);
    REQUIRE_FALSE(plan_contains(trace_plan, "Mechanism: packed full"));
    REQUIRE_FALSE(plan_contains(
        trace_plan, "Actual Input Symbols: 8388608"));
}

TEST_CASE("DOC0377 SliceProject selection is exact and fail-closed",
          "[seq][slice][planner][explain]") {
    auto ctx = fresh_context({"doc0377_slice_plan"});
    exec_sql(ctx,
        "CREATE TABLE doc0377_slice_plan "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;");
    exec_sql(ctx,
        "INSERT INTO doc0377_slice_plan VALUES "
        " (1, 'ACGTACGT'::bioseq.seq('dna4',32));");

    const auto array_plan = explain_text<false>(
        ctx,
        "SELECT (ARRAY[1,2,3])[1] FROM doc0377_slice_plan;");
    REQUIRE_FALSE(plan_contains(array_plan, "BioseqSliceProject"));

    const auto volatile_plan = explain_text<false>(
        ctx,
        "SELECT s[0:(random() * 4)::bigint] "
        "FROM doc0377_slice_plan;");
    REQUIRE_FALSE(plan_contains(volatile_plan, "BioseqSliceProject"));

    exec_sql(ctx, "SET bioseq.enable_custom_scan = off;");
    const auto disabled_plan = explain_text<false>(
        ctx,
        "SELECT s[1:3] FROM doc0377_slice_plan;");
    REQUIRE_FALSE(plan_contains(disabled_plan, "BioseqSliceProject"));
}

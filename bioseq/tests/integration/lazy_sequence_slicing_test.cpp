/**
 * @file lazy_sequence_slicing_test.cpp
 * @brief Tests SQL slicing, letter types, and storing sliced sequences.
 */

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {

/** Return one property from the matching lazy-boundary EXPLAIN section. */
[[nodiscard]] std::optional<std::string> boundary_explain_property(
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

/** Locate the first plan line containing the requested marker. */
[[nodiscard]] std::optional<std::size_t> plan_line_index(
        std::span<const std::string> plan,
        std::string_view needle) {
    for (std::size_t index = 0; index < plan.size(); ++index) {
        if (plan[index].find(needle) != std::string::npos) {
            return index;
        }
    }
    return std::nullopt;
}

/** Count plan lines containing the requested marker. */
[[nodiscard]] std::size_t plan_line_count(
        std::span<const std::string> plan,
        std::string_view needle) {
    return static_cast<std::size_t>(std::count_if(
        plan.begin(), plan.end(), [needle](const std::string& line) {
            return line.find(needle) != std::string::npos;
        }));
}

}  // namespace

TEST_CASE("trace counters scopes refs and reusable slots are exact",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context();
    REQUIRE(query_scalar(
        ctx, "SELECT bioseq_internal.slice_trace_self_test();") == "t");
}

TEST_CASE("Python coordinates and letter access are exact",
          "[component:bioseq][layer:integration][feature:alphabet][resource:postgres]") {
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

TEST_CASE("letter validation hook walks nested queries without reentry",
          "[component:bioseq][layer:integration][feature:alphabet][resource:postgres]") {
    auto ctx = fresh_context({"sequence_slicing_letter_hook_ctas"});
    exec_sql(ctx, "SET statement_timeout='2s';");

    REQUIRE(query_scalar(
        ctx,
        "SELECT EXISTS (SELECT 1 FROM unnest(string_to_array("
        "       current_setting('shared_preload_libraries'), ',')) AS entry "
        "       WHERE btrim(entry) = 'bioseq');")
        == "t");

    const auto simple_before = query_scalar_int(
        ctx, "SELECT bioseq_internal.letter_hook_chain_calls();");
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
    const auto simple_after = query_scalar_int(
        ctx, "SELECT bioseq_internal.letter_hook_chain_calls();");
    // The  probe SELECT and the second counter query each call the hook once.
    // A delta of two checks that the installed hook runs once per statement.
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
             "PREPARE sequence_slicing_letter_hook_prepare AS "
             "SELECT 'A'::bioseq.letter;",
             "CREATE TABLE public.sequence_slicing_letter_hook_ctas AS "
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
        "DECLARE sequence_slicing_letter_hook_cursor CURSOR FOR "
        "SELECT 'A'::bioseq.letter;");
    REQUIRE(cursor_error.sqlstate == "22023");
    exec_sql(ctx, "ROLLBACK;");
    exec_sql(ctx, "RESET statement_timeout;");
}

TEST_CASE("letter typmods resolve literals and validate physical alphabets",
          "[component:bioseq][layer:integration][feature:alphabet][resource:postgres]") {
    auto ctx = fresh_context({"sequence_slicing_typmodless_letter",
                              "sequence_slicing_typed_letter"});

    REQUIRE(query_scalar(
        ctx, "SELECT ('A'::bioseq.letter('dna4'))::text;") == "A");
    REQUIRE(query_scalar(
        ctx,
        "SELECT ((ARRAY['A'::bioseq.letter('dna4'), "
        "               'C'::bioseq.letter('dna4')])[2])::text;") == "C");

    // A sequence element keeps its alphabet in the value even when the
    // expression has no declared typmod.
    REQUIRE(query_scalar(
        ctx,
        "SELECT (('AC'::bioseq.seq('dna4',32))[1])::text;") == "C");

#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    const auto mismatched_typmod = sql_error(
        ctx,
        "SELECT (('A'::bioseq.letter('dna4'))"
        "        ::bioseq.letter('dna5'))::text;");
    REQUIRE(mismatched_typmod.sqlstate == "42804");
    REQUIRE(mismatched_typmod.contains(
        "cannot relabel bioseq.letter to a different alphabet"));
#else
    // The opt0 build supports only dna4. Asking for dna5 must, report that
    // the alphabet is unavailable.
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

    // A letter literal needs an alphabet before it can be stored. A value
    // already created with dna4 remains valid in a typmodless column
    exec_sql(ctx,
        "CREATE TABLE sequence_slicing_typmodless_letter "
        " (value bioseq.letter);");
    const auto stored_typmodless_literal = sql_error(
        ctx, "INSERT INTO sequence_slicing_typmodless_letter VALUES ('A');");
    REQUIRE(stored_typmodless_literal.sqlstate == "22023");
    REQUIRE(stored_typmodless_literal.contains(
        "bioseq.letter input requires an alphabet typmod"));
    exec_sql(ctx,
        "INSERT INTO sequence_slicing_typmodless_letter VALUES "
        " ('A'::bioseq.letter('dna4'));");
    REQUIRE(query_scalar(
        ctx, "SELECT value::text FROM sequence_slicing_typmodless_letter;") == "A");

    exec_sql(ctx,
        "CREATE TABLE sequence_slicing_typed_letter "
        " (value bioseq.letter('dna4')); "
        "INSERT INTO sequence_slicing_typed_letter VALUES ('C');");
    REQUIRE(query_scalar(
        ctx, "SELECT value::text FROM sequence_slicing_typed_letter;") == "C");
}

TEST_CASE("nested slice concat and transforms preserve logical value",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
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

#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
TEST_CASE("concat mismatch diagnostics name both alphabets",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
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

TEST_CASE("TOAST metadata casts and element access are bounded",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context({"sequence_slicing_toast_seq"});
    exec_sql(ctx,
        "CREATE TABLE sequence_slicing_toast_seq ("
        " id int, s bioseq.seq('dna4',64));"
        "ALTER TABLE sequence_slicing_toast_seq ALTER COLUMN s SET STORAGE EXTERNAL;"
        "INSERT INTO sequence_slicing_toast_seq VALUES "
        " (1, repeat('ACGT',300000)::bioseq.seq('dna4',64));");

    REQUIRE(query_scalar(
        ctx,
        "SELECT reltoastrelid <> 0 "
        "       AND pg_total_relation_size(reltoastrelid) > 8192 "
        "FROM pg_catalog.pg_class "
        "WHERE oid='sequence_slicing_toast_seq'::regclass;") == "t");
    const auto row = query_one(
        ctx,
        "SELECT bioseq.len(s), s[1199999]::text, "
        "       bioseq.len(bioseq.seq_cast("
        "           s, (SELECT atttypmod FROM pg_catalog.pg_attribute "
        "               WHERE attrelid='sequence_slicing_toast_seq'::regclass "
        "                 AND attname='s'), false)), "
        "       bioseq.len(bioseq.seq_cast(s, -1, false)) "
        "FROM sequence_slicing_toast_seq;");
    REQUIRE(row[0] == "1200000");
    REQUIRE(row[1] == "T");
    REQUIRE(row[2] == "1200000");
    REQUIRE(row[3] == "1200000");
}

TEST_CASE("explicit text and array bridges validate and round-trip",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
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

TEST_CASE("sliced source concatenations survive INSERT boundaries",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context({
        "sequence_slicing_slice_src", "sequence_slicing_slice_dst",
        "sequence_slicing_slice_plan_dst"});
    exec_sql(ctx,
        "CREATE TABLE sequence_slicing_slice_src "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;");
    exec_sql(ctx,
        "INSERT INTO sequence_slicing_slice_src VALUES "
        " (1, repeat('ACGT',262144)::bioseq.seq('dna4',32));");
    exec_sql(ctx,
        "CREATE TABLE sequence_slicing_slice_dst "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;");
    exec_sql(ctx,
        "CREATE TABLE sequence_slicing_slice_plan_dst "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;");
    exec_sql(ctx,
        "CREATE TEMP TABLE sequence_slicing_cast_namespace_probe(marker int);"
        "CREATE FUNCTION pg_temp.sequence_slicing_unrelated_seq_cast("
        " input_value bioseq.seq, target_typmod integer, is_explicit boolean) "
        "RETURNS bioseq.seq LANGUAGE plpgsql IMMUTABLE STRICT PARALLEL SAFE AS "
        "$function$ BEGIN RETURN input_value; END $function$;");
    exec_sql(ctx,
        "INSERT INTO sequence_slicing_slice_dst "
        "SELECT id, s[500000:500064] OPERATOR(bioseq.||) s[-64:] "
        "FROM sequence_slicing_slice_src;");
    REQUIRE(query_scalar_int(
        ctx, "SELECT bioseq.len(s) FROM sequence_slicing_slice_dst;") == 128);
    REQUIRE(query_scalar(
        ctx,
        "SELECT s::text = repeat('ACGT',32) "
        "FROM sequence_slicing_slice_dst;") == "t");
    REQUIRE(query_scalar(
        ctx,
        "SELECT bioseq.seq_send(s[0:64]) = "
        "       bioseq.seq_send(repeat('ACGT',16)::"
        "                          bioseq.seq('dna4',32)) "
        "FROM sequence_slicing_slice_dst;") == "t");
    REQUIRE(query_scalar(
        ctx,
        "SELECT array_to_string(bioseq.to_array(s[0:8]),'') "
        "FROM sequence_slicing_slice_dst;") == "ACGTACGT");
    constexpr auto literal_left_sql =
        "SELECT octet_length(bioseq.to_text("
        " text 'TG' OPERATOR(bioseq.||) s[0:4])) "
        "FROM sequence_slicing_slice_dst;";
    constexpr auto array_left_sql =
        "SELECT octet_length(bioseq.to_text("
        " ARRAY['T','G']::text[] OPERATOR(bioseq.||) s[0:4])) "
        "FROM sequence_slicing_slice_dst;";
    REQUIRE(query_scalar(
        ctx,
        "SELECT (text 'TG' OPERATOR(bioseq.||) s[0:4])::text "
        "FROM sequence_slicing_slice_dst;") == "TGACGT");
    REQUIRE(query_scalar(
        ctx,
        "SELECT (ARRAY['T','G']::text[] "
        "        OPERATOR(bioseq.||) s[0:4])::text "
        "FROM sequence_slicing_slice_dst;") == "TGACGT");
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

    /* Use another  table to check both EXPLAIN ANALYZE for the INSERT and the
     * row it stores. */
    const auto concat_insert_plan = explain_text(
        ctx,
        "INSERT INTO sequence_slicing_slice_plan_dst "
        "SELECT id, s[500000:500064] OPERATOR(bioseq.||) s[-64:] "
        "FROM sequence_slicing_slice_src;");
    REQUIRE(plan_line_count(
        concat_insert_plan, "Custom Scan (BioseqSliceProject)") == 1u);
    REQUIRE(plan_line_count(
        concat_insert_plan, "value_1 [source] =") == 1u);
    REQUIRE(plan_line_count(
        concat_insert_plan, "value_2 [slice] =") == 1u);
    REQUIRE(plan_line_count(
        concat_insert_plan, "value_3 [slice] =") == 1u);
    REQUIRE(plan_line_count(
        concat_insert_plan, "value_4 [concat] =") == 1u);
    REQUIRE(plan_line_count(concat_insert_plan, "value_5 [") == 0u);
    const auto first_slice = plan_line_index(
        concat_insert_plan, "value_2 [slice] =");
    const auto tail_slice = plan_line_index(
        concat_insert_plan, "value_3 [slice] =");
    const auto concat_node = plan_line_index(
        concat_insert_plan, "value_4 [concat] =");
    REQUIRE(first_slice.has_value());
    REQUIRE(tail_slice.has_value());
    REQUIRE(concat_node.has_value());
    REQUIRE(*first_slice + 2u < concat_insert_plan.size());
    REQUIRE(*tail_slice + 2u < concat_insert_plan.size());
    REQUIRE(*concat_node + 1u < concat_insert_plan.size());
    REQUIRE(find_explain_property(
        std::span<const std::string>{concat_insert_plan}.subspan(
            *first_slice + 1u, 1u), "Depends On") == "value_1");
    REQUIRE(find_explain_property(
        std::span<const std::string>{concat_insert_plan}.subspan(
            *first_slice + 2u, 1u), "Bounds")
        == "lower='500000'::bigint upper='500064'::bigint");
    REQUIRE(find_explain_property(
        std::span<const std::string>{concat_insert_plan}.subspan(
            *tail_slice + 1u, 1u), "Depends On") == "value_1");
    REQUIRE(find_explain_property(
        std::span<const std::string>{concat_insert_plan}.subspan(
            *tail_slice + 2u, 1u), "Bounds")
        == "lower='-64'::bigint upper=omitted");
    REQUIRE(find_explain_property(
        std::span<const std::string>{concat_insert_plan}.subspan(
            *concat_node + 1u, 1u), "Depends On")
        == "value_2, value_3");
    REQUIRE(boundary_explain_property(
        concat_insert_plan, "packed full", "heap tuple", "Producer Value")
        == "4");
    REQUIRE(boundary_explain_property(
        concat_insert_plan, "packed full", "heap tuple", "Estimated Symbols")
        == "128");
    REQUIRE(boundary_explain_property(
        concat_insert_plan, "packed full", "heap tuple", "Estimated Bytes")
        == "128 bytes");
    REQUIRE(boundary_explain_property(
        concat_insert_plan, "packed full", "heap tuple", "Actual Calls")
        == "1");
    REQUIRE(boundary_explain_property(
        concat_insert_plan, "packed full", "heap tuple",
        "Actual Input Symbols") == "128");
    REQUIRE(boundary_explain_property(
        concat_insert_plan, "packed full", "heap tuple",
        "Actual Output Symbols") == "128");
    REQUIRE(find_explain_property(
        concat_insert_plan, "Planned Materialized Symbols") == "128");
    REQUIRE(find_explain_property(
        concat_insert_plan, "Planned Materialized Bytes") == "128 bytes");
    const auto durable_plan_rows = query_rows(
        ctx,
        "SELECT id, bioseq.len(s), s::text "
        "FROM sequence_slicing_slice_plan_dst ORDER BY id;");
    REQUIRE(durable_plan_rows.size() == 1u);
    REQUIRE(durable_plan_rows.front().size() == 3u);
    REQUIRE(durable_plan_rows.front()[0]
        == std::optional<std::string>{"1"});
    REQUIRE(durable_plan_rows.front()[1]
        == std::optional<std::string>{"128"});
    std::string expected_concat;
    expected_concat.reserve(128u);
    for (int repetition = 0; repetition < 32; ++repetition) {
        expected_concat += "ACGT";
    }
    REQUIRE(durable_plan_rows.front()[2]
        == std::optional<std::string>{expected_concat});

    /* A function with the same arguments as the cast is still an ordinary
     * function call. The plan must keep its result as a separate source. */
    const auto unrelated_cast_plan = explain_text<false>(
        ctx,
        "SELECT pg_temp.sequence_slicing_unrelated_seq_cast("
        " s[500000:500064], 3, false) FROM sequence_slicing_slice_src;");
    REQUIRE(plan_line_count(
        unrelated_cast_plan, "value_1 [source] =") == 1u);
    REQUIRE(plan_line_count(
        unrelated_cast_plan, "value_2 [slice] =") == 1u);
    REQUIRE(plan_line_count(
        unrelated_cast_plan, "value_3 [source] =") == 1u);
    const auto opaque_cast = plan_line_index(
        unrelated_cast_plan, "value_3 [source] =");
    REQUIRE(opaque_cast.has_value());
    REQUIRE(*opaque_cast + 1u < unrelated_cast_plan.size());
    REQUIRE(find_explain_property(
        std::span<const std::string>{unrelated_cast_plan}.subspan(
            *opaque_cast + 1u, 1u), "Depends On") == "none");
    for (const std::string_view mechanism : {"text", "binary"}) {
        CAPTURE(mechanism);
        REQUIRE(boundary_explain_property(
            unrelated_cast_plan, mechanism, "client output",
            "Producer Value") == "3");
    }

    /* Direct SQL calls to seq_cast rmain separate sources. Only the cast
     * expression PostgreSQL inserts for assignment can be folded into a slice. */
    const std::array<std::string_view, 3> explicit_cast_sql{
        "SELECT bioseq.seq_cast(s[500000:500064], 3, false) "
        "FROM sequence_slicing_slice_src;",
        "SELECT bioseq.seq_cast(s[500000:500064], 3, true) "
        "FROM sequence_slicing_slice_src;",
        "SELECT bioseq.seq_cast(s[500000:500064], -1, false) "
        "FROM sequence_slicing_slice_src;",
    };
    for (const std::string_view sql : explicit_cast_sql) {
        CAPTURE(sql);
        const auto explicit_cast_plan = explain_text<false>(ctx, sql);
        REQUIRE(plan_line_count(
            explicit_cast_plan, "value_1 [source] =") == 1u);
        REQUIRE(plan_line_count(
            explicit_cast_plan, "value_2 [slice] =") == 1u);
        REQUIRE(plan_line_count(
            explicit_cast_plan, "value_3 [source] =") == 1u);
        REQUIRE(plan_line_count(explicit_cast_plan, "value_4 [") == 0u);
        const auto opaque_exact_cast = plan_line_index(
            explicit_cast_plan, "value_3 [source] =");
        REQUIRE(opaque_exact_cast.has_value());
        REQUIRE(*opaque_exact_cast + 1u < explicit_cast_plan.size());
        REQUIRE(find_explain_property(
            std::span<const std::string>{explicit_cast_plan}.subspan(
                *opaque_exact_cast + 1u, 1u), "Depends On") == "none");
        for (const std::string_view mechanism : {"text", "binary"}) {
            CAPTURE(mechanism);
            REQUIRE(boundary_explain_property(
                explicit_cast_plan, mechanism, "client output",
                "Producer Value") == "3");
        }
    }

    const auto plan = explain_text(
        ctx,
        "SELECT s[500000:500064] FROM sequence_slicing_slice_src;");
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_line_count(
        plan, "Custom Scan (BioseqSliceProject)") == 1u);
    REQUIRE(plan_contains(plan, "Slice Materialization:"));
    REQUIRE(plan_line_count(plan, "value_1 [source] =") == 1u);
    REQUIRE(plan_line_count(plan, "value_2 [slice] =") == 1u);
    const auto slice_node = plan_line_index(plan, "value_2 [slice] =");
    REQUIRE(slice_node.has_value());
    REQUIRE(*slice_node + 2u < plan.size());
    REQUIRE(find_explain_property(
        std::span<const std::string>{plan}.subspan(*slice_node + 1u, 1u),
        "Depends On") == "value_1");
    REQUIRE(find_explain_property(
        std::span<const std::string>{plan}.subspan(*slice_node + 2u, 1u),
        "Bounds")
        == "lower='500000'::bigint upper='500064'::bigint");
    REQUIRE(plan_contains(plan, "Mechanism: text"));
    REQUIRE(plan_contains(plan, "Reason: client output"));
    REQUIRE(plan_line_count(plan, "Mechanism: text") == 1u);
    REQUIRE(plan_line_count(plan, "Mechanism: binary") == 1u);
    REQUIRE(plan_line_count(plan, "Reason: client output") == 2u);
    for (const std::string_view mechanism : {"text", "binary"}) {
        CAPTURE(mechanism);
        REQUIRE(boundary_explain_property(
            plan, mechanism, "client output", "Producer Value") == "2");
        REQUIRE(boundary_explain_property(
            plan, mechanism, "client output", "Estimated Symbols") == "64");
        REQUIRE(boundary_explain_property(
            plan, mechanism, "client output", "Estimated Bytes")
            == "64 bytes");
    }
    REQUIRE(plan_contains(plan, "Planned Materialized Symbols"));
    REQUIRE(plan_contains(plan, "Planned Materialized Bytes"));
    REQUIRE(find_explain_property(plan, "Planned Materialized Symbols")
        == "128");
    REQUIRE(find_explain_property(plan, "Planned Materialized Bytes")
        == "128 bytes");
    REQUIRE(plan_contains(plan, "Actual Calls"));
}

TEST_CASE("long source slice is faster than whole-sequence materialization",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context({"sequence_slicing_slice_perf"});
    exec_sql(ctx,
        "SET jit = off; "
        "SET bioseq.column_cache.enabled = off; "
        "SET bioseq.vectorization.enabled = off;");
    exec_sql(ctx,
        "CREATE TABLE sequence_slicing_slice_perf "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;");
    exec_sql(ctx,
        "INSERT INTO sequence_slicing_slice_perf VALUES "
        " (1, repeat('ACGT',2097152)::bioseq.seq('dna4',32));");

    constexpr auto slice_sql =
        "SELECT octet_length(bioseq.to_text(s[4194304:4194368])) "
        "FROM sequence_slicing_slice_perf;";
    constexpr auto whole_sql =
        "SELECT octet_length(bioseq.to_text(s)) "
        "FROM sequence_slicing_slice_perf;";
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

TEST_CASE("SliceProject selection is exact and fail-closed",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    auto ctx = fresh_context({"sequence_slicing_slice_plan"});
    exec_sql(ctx,
        "CREATE TABLE sequence_slicing_slice_plan "
        " (id int, s bioseq.seq('dna4',32)) USING bioseq;");
    exec_sql(ctx,
        "INSERT INTO sequence_slicing_slice_plan VALUES "
        " (1, 'ACGTACGT'::bioseq.seq('dna4',32));");

    const auto array_plan = explain_text<false>(
        ctx,
        "SELECT (ARRAY[1,2,3])[1] FROM sequence_slicing_slice_plan;");
    REQUIRE_FALSE(plan_contains(array_plan, "BioseqSliceProject"));

    const auto volatile_plan = explain_text<false>(
        ctx,
        "SELECT s[0:(random() * 4)::bigint] "
        "FROM sequence_slicing_slice_plan;");
    REQUIRE_FALSE(plan_contains(volatile_plan, "BioseqSliceProject"));

    exec_sql(ctx, "SET bioseq.planner.custom_scan.enabled = off;");
    const auto disabled_plan = explain_text<false>(
        ctx,
        "SELECT s[1:3] FROM sequence_slicing_slice_plan;");
    REQUIRE_FALSE(plan_contains(disabled_plan, "BioseqSliceProject"));
}

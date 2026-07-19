// DOC0377: SQL-level slicing/letter/algebra/bridge and durable-boundary tests.

#include "common_tests_integration.h"
import bioseq.integration_tests.common;

using namespace bioseq_int_tests;

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

TEST_CASE("DOC0377 letter typmods resolve literals and validate physical alphabets",
          "[seq][letter][typmod][array]") {
    auto ctx = fresh_context({"doc0377_typmodless_letter"});

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

    const auto mismatched_typmod = sql_error(
        ctx,
        "SELECT (('A'::bioseq.letter('dna4'))"
        "        ::bioseq.letter('dna5'))::text;");
    REQUIRE(mismatched_typmod.sqlstate == "42804");
    REQUIRE(mismatched_typmod.contains(
        "cannot relabel bioseq.letter to a different alphabet"));

    const auto typmodless_literal = sql_error(
        ctx, "SELECT 'A'::bioseq.letter;");
    REQUIRE(typmodless_literal.sqlstate == "22023");
    REQUIRE(typmodless_literal.contains(
        "bioseq.letter input requires an alphabet typmod"));

    // PG17 performs unknown-literal input before typmod coercion and has no
    // base-type heap-write callback.  A parser token can therefore enter a
    // typmodless column, but its first observable read must fail closed.
    exec_sql(ctx,
        "CREATE TABLE doc0377_typmodless_letter "
        " (value bioseq.letter);");
    exec_sql(ctx,
        "INSERT INTO doc0377_typmodless_letter VALUES ('A');");
    const auto stored_typmodless_literal = sql_error(
        ctx, "SELECT value FROM doc0377_typmodless_letter;");
    REQUIRE(stored_typmodless_literal.sqlstate == "22023");
    REQUIRE(stored_typmodless_literal.contains(
        "bioseq.letter input requires an alphabet typmod"));
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

    const auto plan = explain_text(
        ctx,
        "SELECT s[500000:500064] FROM doc0377_slice_src;");
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_contains(plan, "Projection Mode: source-backed lazy"));
    REQUIRE(plan_contains(plan, "Normalized Interval: [500000, 500064)"));
    REQUIRE(plan_contains(plan, "Planned Materialized Symbols"));
    REQUIRE(plan_contains(plan, "Planned Materialized Bytes"));
    REQUIRE(plan_contains(plan, "Actual Rows Published: 1"));
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

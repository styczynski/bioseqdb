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

TEST_CASE("DOC0377 nested slice concat and transforms preserve logical value",
          "[seq][slice][concat][reverse]") {
    auto ctx = fresh_context();
    REQUIRE(query_scalar(
        ctx,
        "SELECT (((('ACGTACGTACGT'::bioseq.seq('dna4',32))[:10])[5:] "
        "       || ('TTTTCCCCAAAA'::bioseq.seq('dna4',32))[5:10])"
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
        "  SELECT s[1:7] || bioseq.reverse(s[0:2]) AS s FROM a) "
        "SELECT (SELECT s::text FROM b) || (SELECT s::text FROM b);")
        == "CGTACGCACGTACGCA");
}

TEST_CASE("DOC0377 explicit text and array bridges validate and round-trip",
          "[seq][concat][array]") {
    auto ctx = fresh_context();
    REQUIRE(query_scalar(
        ctx,
        "SELECT (('AC'::bioseq.seq('dna4',32)) "
        " || ARRAY['G','TAC']::text[])::text;") == "ACGTAC");
    REQUIRE(query_scalar(
        ctx,
        "SELECT (ARRAY['A'::bioseq.letter('dna4'),"
        "              'C'::bioseq.letter('dna4')] "
        " || 'GT'::bioseq.seq('dna4',32))::text;") == "ACGT");
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
        " || ARRAY['C'::text,NULL::text];");
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
        "SELECT id, s[500000:500064] || s[-64:] "
        "FROM doc0377_slice_src;");
    REQUIRE(query_scalar_int(
        ctx, "SELECT bioseq.len(s) FROM doc0377_slice_dst;") == 128);
    REQUIRE(query_scalar(
        ctx,
        "SELECT s::text = repeat('ACGT',32) "
        "FROM doc0377_slice_dst;") == "t");

    const auto plan = query_rows(
        ctx,
        "EXPLAIN (ANALYZE, COSTS OFF) "
        "SELECT s[500000:500064] FROM doc0377_slice_src;");
    bool saw_slice_project = false;
    for (const auto& line : plan) {
        if (line[0].has_value()
            && line[0]->find("BioseqSliceProject") != std::string::npos) {
            saw_slice_project = true;
        }
    }
    REQUIRE(saw_slice_project);
}

// Integration tests for `bioseq.create_sample_table`,
// `bioseq.inspect_locator`, and the per-fragment row layout.
//
// USING bioseq tables emit zero rows for SELECT-without-mmsearch by design
// (the columnar custom scan only fires when mmsearch is in the qual list —
// see test_mmsearch.cpp:50). The same applies to the sub-tables (also
// USING bioseq). So verification happens via:
//   1. `bioseq_internal.seq_sub_table` catalog row count + sub_table_relid.
//   2. `pg_relation_size(sub_table_relid)` to confirm fragment bytes were
//      written (positive size proves data exists on disk).
//   3. `bioseq.inspect_locator` on inline literals to prove the locator layout
//      is wired. The insertion fixture exercises the stub path by writing
//      locator stubs into the parent relation.

#include "common_tests_integration.h"
import bioseq.integration_tests.common;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view kSampleTable = "sample_table_fixture_t";

}  // namespace

TEST_CASE("bioseq.create_sample_table writes one catalog row and a "
          "non-empty sub-table",
          "[integration][sample-table]") {
    auto ctx = fresh_context({kSampleTable});
    exec_sqlf(ctx,
              "SELECT bioseq.create_sample_table('{}', 100, 1024, 'dna4', 42);",
              kSampleTable);
    // 100 sequences × 1024 bp each = 100 sequences × 1 fragment each.
    // All sequences should land in one (bucket, shard) — bucket determined
    // by len_log2(1024) → bucket_idx ≥ 0; shard 0 (first write of this bucket).
    const long long catalog_rows = query_scalar_int(ctx,
        fmt::format("SELECT COUNT(*)::bigint FROM bioseq_internal.seq_sub_table s "
                    "  WHERE s.main_relfilenode = (SELECT relfilenode FROM pg_class "
                    "                              WHERE relname = '{}');",
                    kSampleTable));
    REQUIRE(catalog_rows == 1);
    // The sub_table_relid must point at an existing pg_class entry.
    const long long sub_relid = query_scalar_int(ctx,
        fmt::format("SELECT sub_table_relid FROM bioseq_internal.seq_sub_table s "
                    "  WHERE s.main_relfilenode = (SELECT relfilenode FROM pg_class "
                    "                              WHERE relname = '{}') LIMIT 1;",
                    kSampleTable));
    REQUIRE(sub_relid > 0);
    REQUIRE(query_scalar_int(ctx,
                fmt::format("SELECT COUNT(*)::bigint FROM pg_class WHERE oid = {};",
                            sub_relid)) == 1);
    // pg_relation_size must be > 0 (bytes were written for 100 fragment rows).
    const long long sub_size = query_scalar_int(ctx,
        fmt::format("SELECT pg_relation_size({})::bigint;", sub_relid));
    REQUIRE(sub_size > 0);
}

TEST_CASE("bioseq.create_sample_table rejects alphabets other than dna4",
          "[integration][sample-table]") {
    auto ctx = fresh_context({kSampleTable});
    REQUIRE(sql_fails(ctx,
        fmt::format("SELECT bioseq.create_sample_table('{}', 1, 1024, 'dna5', 42);",
                    kSampleTable)));
}

TEST_CASE("sample table generators resolve the active creation namespace",
          "[integration][sample-table][doc0388][relocatable]") {
    constexpr std::string_view schema = "doc0388_sample_namespace";
    constexpr std::string_view sequence_table = "doc0388_sequence_sample";
    constexpr std::string_view pair_table = "doc0388_pair_sample";

    auto ctx = fresh_context({sequence_table, pair_table});
    exec_sqlf(ctx, "DROP SCHEMA IF EXISTS {} CASCADE;", schema);
    exec_sql(ctx, "BEGIN;");
    exec_sqlf(ctx, "CREATE SCHEMA {};", schema);
    exec_sqlf(ctx, "SET LOCAL search_path = {}, public;", schema);

    const long long sequence_oid = query_scalar_int(
        ctx,
        fmt::format(
            "SELECT bioseq.create_sample_table("
            "'{}', 1, 32, 'dna4', 42);",
            sequence_table));
    const long long pair_oid = query_scalar_int(
        ctx,
        fmt::format(
            "SELECT bioseq.create_query_target_sample_table("
            "'{}', 1, 32, 0.0, 128, "
            "0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 42);",
            pair_table));

    REQUIRE(query_scalar_int(
                ctx,
                fmt::format(
                    "SELECT count(*)::bigint "
                    "FROM pg_catalog.pg_class AS c "
                    "JOIN pg_catalog.pg_namespace AS n "
                    "  ON n.oid = c.relnamespace "
                    "WHERE n.nspname = '{}' "
                    "  AND c.relname IN ('{}', '{}') "
                    "  AND c.oid IN ({}, {});",
                    schema,
                    sequence_table,
                    pair_table,
                    sequence_oid,
                    pair_oid)) == 2);
    REQUIRE(query_scalar_int(
                ctx,
                fmt::format(
                    "SELECT count(*)::bigint "
                    "FROM pg_catalog.pg_class AS c "
                    "JOIN pg_catalog.pg_namespace AS n "
                    "  ON n.oid = c.relnamespace "
                    "WHERE n.nspname = 'public' "
                    "  AND c.relname IN ('{}', '{}');",
                    sequence_table,
                    pair_table)) == 0);

    exec_sql(ctx, "ROLLBACK;");
}

TEST_CASE("an empty sequence creates a catalog row "
          "without allocating an empty fragment relation",
          "[integration][sample-table]") {
    auto ctx = fresh_context({kSampleTable});
    exec_sqlf(ctx,
              "SELECT bioseq.create_sample_table('{}', 1, 0, 'dna4', 42);",
              kSampleTable);
    // Empty sequence: no physical fragment relation is needed.
    REQUIRE(query_scalar_int(ctx,
                fmt::format("SELECT COUNT(*)::bigint FROM bioseq_internal.seq_sub_table s "
                            "  WHERE s.main_relfilenode = (SELECT relfilenode FROM pg_class "
                            "                              WHERE relname = '{}');",
                            kSampleTable)) == 0);
    REQUIRE(query_scalar_int(ctx,
                fmt::format("SELECT COUNT(*)::bigint "
                    "FROM bioseq_internal.sequence_source_version v "
                    "WHERE v.user_relid='{}'::regclass AND v.state=1 "
                    "AND v.num_fragments=0 AND v.fragment_relid IS NULL "
                    "AND v.fragment_storage_id=0;", kSampleTable)) == 1);
}

TEST_CASE("inspect_locator on an inline value returns inline fields",
          "[integration][phase-c][locator-inspect]") {
    auto ctx = fresh_context();
    // Inline values return zeros for locator-only fields and
    // the inline bp_size + padding for non-stub inputs.
    const long long version = query_scalar_int(ctx,
        "SELECT version FROM bioseq.inspect_locator("
        "  'ACGTACGTACGTACGTACGTACGTACGTACGT'::bioseq.seq('dna4', 32));");
    REQUIRE(version == 0);
    const long long bp_size = query_scalar_int(ctx,
        "SELECT total_bp_size FROM bioseq.inspect_locator("
        "  'ACGTACGTACGTACGTACGTACGTACGTACGT'::bioseq.seq('dna4', 32));");
    REQUIRE(bp_size == 32);
}

TEST_CASE("DOC0264 — INSERT INTO USING bioseq table writes per-fragment "
          "sub-table rows + stub locator",
          "[integration][phase-c][locator-inspect][stub]") {
    auto ctx = fresh_context({kSampleTable});
    exec_sqlf(ctx,
              "CREATE TABLE {} (pk INT PRIMARY KEY, "
              "seq bioseq.seq('dna4', 32)) USING bioseq;",
              kSampleTable);
    exec_sqlf(ctx,
              "INSERT INTO {} VALUES (1, "
              "'ACGTACGTACGTACGTACGTACGTACGTACGT'::bioseq.seq('dna4', 32));",
              kSampleTable);
    // The standard INSERT path (bioseq_tuple_insert → rewrite-to-stub →
    // insert_payload_as_fragments) creates one catalog row for the bucket/shard.
    REQUIRE(query_scalar_int(ctx,
                fmt::format("SELECT COUNT(*)::bigint FROM bioseq_internal.seq_sub_table s "
                            "  WHERE s.main_relfilenode = (SELECT relfilenode FROM pg_class "
                            "                              WHERE relname = '{}');",
                            kSampleTable)) == 1);
    // The sub-table relation has positive bytes (one fragment row × ~8 KiB).
    const long long sub_size = query_scalar_int(ctx,
        fmt::format("SELECT pg_relation_size(s.sub_table_relid)::bigint "
                    "  FROM bioseq_internal.seq_sub_table s "
                    " WHERE s.main_relfilenode = (SELECT relfilenode FROM pg_class "
                    "                              WHERE relname = '{}') LIMIT 1;",
                    kSampleTable));
    REQUIRE(sub_size > 0);
}

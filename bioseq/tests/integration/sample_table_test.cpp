/**
 * @file sample_table_test.cpp
 * @brief Tests sample table creation and sequence fragment storage.
 *
 * Checks the fragment catalog and relation sizes after generating or inserting
 * sequences. Also tests schema selection, empty sequences, and the fields
 * reported by inspect_locator for an inline value.
 */

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view k_sample_table = "sample_table_storage";

}  // namespace

TEST_CASE("bioseq.create_sample_table writes one catalog row and a "
          "non-empty sub-table",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context({k_sample_table});
    exec_sqlf(ctx,
              "SELECT bioseq.create_sample_table('{}', 100, 1024, 'dna4', 42);",
              k_sample_table);
    // These 100 short sequences should share one fragment storage shard.
    const long long catalog_rows = query_scalar_int(ctx,
        fmt::format("SELECT COUNT(*)::bigint FROM bioseq_internal.seq_sub_table s "
                    "  WHERE s.main_relfilenode = (SELECT relfilenode FROM pg_class "
                    "                              WHERE relname = '{}');",
                    k_sample_table));
    REQUIRE(catalog_rows == 1);
    // The sub_table_relid must point at an existing pg_class entry.
    const long long sub_relid = query_scalar_int(ctx,
        fmt::format("SELECT sub_table_relid FROM bioseq_internal.seq_sub_table s "
                    "  WHERE s.main_relfilenode = (SELECT relfilenode FROM pg_class "
                    "                              WHERE relname = '{}') LIMIT 1;",
                    k_sample_table));
    REQUIRE(sub_relid > 0);
    REQUIRE(query_scalar_int(ctx,
                fmt::format("SELECT COUNT(*)::bigint FROM pg_class WHERE oid = {};",
                            sub_relid)) == 1);
    // The fragment relation must occupy space on disk.
    const long long sub_size = query_scalar_int(ctx,
        fmt::format("SELECT pg_relation_size({})::bigint;", sub_relid));
    REQUIRE(sub_size > 0);
}

TEST_CASE("bioseq.create_sample_table rejects alphabets other than dna4",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context({k_sample_table});
    REQUIRE(sql_fails(ctx,
        fmt::format("SELECT bioseq.create_sample_table('{}', 1, 1024, 'dna5', 42);",
                    k_sample_table)));
}

TEST_CASE("sample table generators resolve the active creation namespace",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    constexpr std::string_view schema = "sample_creation_namespace";
    constexpr std::string_view sequence_table = "relocatable_sequence_sample";
    constexpr std::string_view pair_table = "relocatable_pair_sample";

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
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context({k_sample_table});
    exec_sqlf(ctx,
              "SELECT bioseq.create_sample_table('{}', 1, 0, 'dna4', 42);",
              k_sample_table);
    // Empty sequence: no physical fragment relation is needed.
    REQUIRE(query_scalar_int(ctx,
                fmt::format("SELECT COUNT(*)::bigint FROM bioseq_internal.seq_sub_table s "
                            "  WHERE s.main_relfilenode = (SELECT relfilenode FROM pg_class "
                            "                              WHERE relname = '{}');",
                            k_sample_table)) == 0);
    REQUIRE(query_scalar_int(ctx,
                fmt::format("SELECT COUNT(*)::bigint "
                    "FROM bioseq_internal.sequence_source_version v "
                    "WHERE v.user_relid='{}'::regclass AND v.state=1 "
                    "AND v.num_fragments=0 AND v.fragment_relid IS NULL "
                    "AND v.fragment_storage_id=0;", k_sample_table)) == 1);
}

TEST_CASE("inspect_locator on an inline value returns inline fields",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context();
    // An inline value reports version zero and its own sequence length.
    const long long version = query_scalar_int(ctx,
        "SELECT version FROM bioseq.inspect_locator("
        "  'ACGTACGTACGTACGTACGTACGTACGTACGT'::bioseq.seq('dna4', 32));");
    REQUIRE(version == 0);
    const long long bp_size = query_scalar_int(ctx,
        "SELECT total_bp_size FROM bioseq.inspect_locator("
        "  'ACGTACGTACGTACGTACGTACGTACGTACGT'::bioseq.seq('dna4', 32));");
    REQUIRE(bp_size == 32);
}

TEST_CASE("INSERT INTO USING bioseq table writes per-fragment "
          "sub-table rows + stub locator",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context({k_sample_table});
    exec_sqlf(ctx,
              "CREATE TABLE {} (pk INT PRIMARY KEY, "
              "seq bioseq.seq('dna4', 32)) USING bioseq;",
              k_sample_table);
    exec_sqlf(ctx,
              "INSERT INTO {} VALUES (1, "
              "'ACGTACGTACGTACGTACGTACGTACGTACGT'::bioseq.seq('dna4', 32));",
              k_sample_table);
    // INSERT must  create a fragment storage entry for the sequence.
    REQUIRE(query_scalar_int(ctx,
                fmt::format("SELECT COUNT(*)::bigint FROM bioseq_internal.seq_sub_table s "
                            "  WHERE s.main_relfilenode = (SELECT relfilenode FROM pg_class "
                            "                              WHERE relname = '{}');",
                            k_sample_table)) == 1);
    // The new fragment relation must occupy space on disk.
    const long long sub_size = query_scalar_int(ctx,
        fmt::format("SELECT pg_relation_size(s.sub_table_relid)::bigint "
                    "  FROM bioseq_internal.seq_sub_table s "
                    " WHERE s.main_relfilenode = (SELECT relfilenode FROM pg_class "
                    "                              WHERE relname = '{}') LIMIT 1;",
                    k_sample_table));
    REQUIRE(sub_size > 0);
}

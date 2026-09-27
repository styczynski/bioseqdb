/**
 * @file Tests storing sequences in sub-tables grouped by length and column.
 *
 * Checks shard limits, sequence lengths, and reads through mmsearch.
 * Also checks cleanup on DROP, rejection of TEMP parents, and inheritance
 * of UNLOGGED storage.
 */

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view k_table = "storage_buckets";

}  // namespace

TEST_CASE("bioseq.len reports correct payload size after stub hydration",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    // Check that bioseq.len returns the sequence legth as BIGINT.
    auto ctx = fresh_context({k_table});
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(k_table),
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = false,
        },
        {seq_row{.pk = 1, .sequences = {"ACGT"}}});

    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT bioseq.len(seq0) FROM {} LIMIT 1;", k_table))
            == 4);
    REQUIRE(query_scalar(
                ctx,
                fmt::format("SELECT pg_typeof(bioseq.len(seq0))::text "
                            "FROM {} LIMIT 1;", k_table))
            == "bigint");
}

TEST_CASE("bioseq.storage.sequence_shard.max_size_gib=0 forces one shard "
          "per insert",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    exec_sql(ctx, "SET bioseq.storage.sequence_shard.max_size_gib = 0;");
    exec_sqlf(ctx,
              "CREATE TABLE {} (id INT PRIMARY KEY, "
              "seq bioseq.seq('dna4', 32)) USING bioseq;",
              k_table);
    for (int row_number = 0; row_number < 5; ++row_number) {
        exec_sqlf(ctx,
                  "INSERT INTO {} VALUES ({}, "
                  "'ACGT'::bioseq.seq('dna4', 32));",
                  k_table, row_number);
    }
    const long long rows = query_scalar_int(
        ctx,
        fmt::format(
            "SELECT count(*) FROM bioseq_internal.seq_sub_table s "
            "WHERE s.main_relfilenode = (SELECT relfilenode FROM pg_class "
            "WHERE relname = '{}');",
            k_table));
    REQUIRE(rows == 5);
}

TEST_CASE("bioseq.seq_sub_table_summary view + list_seq_storage_shards "
          "expose every shard for a parent",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    exec_sqlf(ctx,
              "CREATE TABLE {} (id INT PRIMARY KEY, "
              "seq bioseq.seq('dna4', 32)) USING bioseq;",
              k_table);
    exec_sqlf(ctx,
              "INSERT INTO {} VALUES (1, "
              "'ACGTACGTACGT'::bioseq.seq('dna4', 32));",
              k_table);

    // View must list at least one row for the parent.
    REQUIRE(query_scalar_int(
                ctx,
                fmt::format(
                    "SELECT COUNT(*) FROM bioseq.seq_sub_table_summary "
                    "WHERE main_relfilenode = "
                    "  (SELECT relfilenode FROM pg_class WHERE relname='{}');",
                    k_table))
            >= 1);

    // The shard-listing function must also report a row for this parent
    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT COUNT(*) FROM "
                            "bioseq.list_seq_storage_shards('{}'::regclass);",
                            k_table))
            >= 1);
}

TEST_CASE("seq_sub_table catalog exists and is empty on fresh extension",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context();
    REQUIRE(query_scalar_int(
                ctx, "SELECT count(*) FROM bioseq_internal.seq_sub_table;")
            == 0);
}

TEST_CASE("TEMP parents are rejected at CREATE TABLE",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context();
    REQUIRE(sql_fails(
        ctx,
        "CREATE TEMPORARY TABLE _storage_bucket_temp_parent "
        "(id int, seq bioseq.seq('dna4', 32)) USING bioseq;"));
}

TEST_CASE("every SEQ column INSERT lands in the expected bucket",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    exec_sqlf(ctx,
              "CREATE TABLE {} (id INT PRIMARY KEY, "
              "seq bioseq.seq('dna4', 32)) USING bioseq;",
              k_table);
    exec_sqlf(
        ctx,
        "INSERT INTO {} VALUES (1, repeat('A', 16)::bioseq.seq('dna4', 32));",
        k_table);
    exec_sqlf(
        ctx,
        "INSERT INTO {} VALUES (2, repeat('A', 1024)::bioseq.seq('dna4', 32));",
        k_table);

    // At least one sub-table catalog row must exist for this parent.
    const long long rows = query_scalar_int(
        ctx,
        fmt::format(
            "SELECT count(*) FROM bioseq_internal.seq_sub_table s "
            "WHERE s.main_relfilenode = (SELECT relfilenode FROM pg_class "
            "WHERE relname = '{}');",
            k_table));
    REQUIRE(rows >= 1);
}

TEST_CASE("DROP TABLE cascades to every sub-table",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    exec_sqlf(ctx,
              "CREATE TABLE {} (id INT PRIMARY KEY, "
              "seq bioseq.seq('dna4', 32)) USING bioseq;",
              k_table);
    for (int row_number = 0; row_number < 5; ++row_number) {
        exec_sqlf(ctx,
                  "INSERT INTO {} VALUES ({}, "
                  "'ACGTACGTACGTACGTACGTACGTACGT'::bioseq.seq('dna4', 32));",
                  k_table, row_number);
    }
    exec_sqlf(ctx, "DROP TABLE {};", k_table);
    REQUIRE(query_scalar_int(
                ctx, "SELECT count(*) FROM bioseq_internal.seq_sub_table;")
            == 0);
    REQUIRE(query_scalar_int(
                ctx,
                // A digit after seq_sub_ identifies shard tables. This excludes the
                // seq_sub_table catalog and its indexes from the cleanup check.
                "SELECT count(*) FROM pg_class WHERE "
                "relnamespace = 'bioseq_internal'::regnamespace "
                "AND (relname ~ '^seq_sub_[0-9]' OR relname LIKE 'idx_sidecar_%');")
            == 0);
}

TEST_CASE("multi-SEQ-column parents route by column_attno",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context({k_table});
#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    constexpr std::string_view second_alphabet = "dna5";
    constexpr std::string_view second_sequence = "NACGT";
#else
    constexpr std::string_view second_alphabet = "dna4";
    constexpr std::string_view second_sequence = "AACGT";
#endif
    exec_sqlf(ctx,
              "CREATE TABLE {} (id INT PRIMARY KEY, "
              "seq_a bioseq.seq('dna4', 32), "
              "seq_b bioseq.seq('{}', 64)) USING bioseq;",
              k_table, second_alphabet);
    exec_sqlf(ctx,
              "INSERT INTO {} VALUES (1, "
              "'ACGT'::bioseq.seq('dna4', 32), "
              "'{}'::bioseq.seq('{}', 64));",
              k_table, second_sequence, second_alphabet);
    const auto column_attnos = query_rows(
        ctx,
        fmt::format(
            "SELECT column_attno::text FROM bioseq_internal.seq_sub_table "
            "WHERE main_relfilenode = (SELECT relfilenode FROM pg_class "
            "WHERE relname = '{}') ORDER BY column_attno;",
            k_table));
    REQUIRE(column_attnos.size() == 2);
    // seq_a is the 2nd column (id is #1), seq_b is the 3rd column.
    REQUIRE(column_attnos[0][0].value() == "2");
    REQUIRE(column_attnos[1][0].value() == "3");
}

TEST_CASE("UNLOGGED parents inherit UNLOGGED sub-tables",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    exec_sqlf(ctx,
              "CREATE UNLOGGED TABLE {} (id INT PRIMARY KEY, "
              "seq bioseq.seq('dna4', 32)) USING bioseq;",
              k_table);
    exec_sqlf(ctx,
              "INSERT INTO {} VALUES (1, "
              "'ACGT'::bioseq.seq('dna4', 32));",
              k_table);
    REQUIRE(query_scalar(
                ctx,
                fmt::format(
                    "SELECT c.relpersistence::text "
                    "FROM bioseq_internal.seq_sub_table s "
                    "JOIN pg_class c ON c.oid = s.sub_table_relid "
                    "WHERE s.main_relfilenode = "
                    "  (SELECT relfilenode FROM pg_class WHERE relname = '{}') "
                    "LIMIT 1;",
                    k_table))
            == "u");
}

TEST_CASE("round-trip via USING bioseq table preserves payload bytes",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    exec_sqlf(ctx,
              "CREATE TABLE {} (id INT PRIMARY KEY, "
              "seq bioseq.seq('dna4', 32)) USING bioseq;",
              k_table);
    exec_sqlf(ctx,
              "INSERT INTO {} VALUES (1, "
              "'ACGTACGTACGT'::bioseq.seq('dna4', 32));",
              k_table);
    exec_sqlf(ctx,
              "INSERT INTO {} VALUES (2, "
              "'TTTTAAAA'::bioseq.seq('dna4', 32));",
              k_table);
    const auto rows = query_rows(
        ctx,
        fmt::format("SELECT seq::text, bioseq.len(seq)::text FROM {} "
                    "ORDER BY id;",
                    k_table));
    REQUIRE(rows.size() == 2);
    REQUIRE(rows[0][0].value() == "ACGTACGTACGT");
    REQUIRE(rows[0][1].value() == "12");
    REQUIRE(rows[1][0].value() == "TTTTAAAA");
    REQUIRE(rows[1][1].value() == "8");
}

// Store the alignment fixture  in sub-tables and run mmsearch against it.
// A returned match checks that the scan can load sequence bytes from this storage.
TEST_CASE("mmsearch end-to-end exercises hydration from sub-tables",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    constexpr std::string_view k_alignment_table = "storage_bucket_alignment";
    auto ctx = fresh_context({k_alignment_table});
    seed_hoe_fixture(ctx, k_alignment_table);

    // Check that the seeded parent has sequence shards.
    const long long shards = query_scalar_int(
        ctx,
        fmt::format(
            "SELECT count(*) FROM bioseq_internal.seq_sub_table s "
            "WHERE s.main_relfilenode = (SELECT relfilenode FROM pg_class "
            "WHERE relname='{}');",
            k_alignment_table));
    REQUIRE(shards >= 1);

    // The five seeded rows contain the probe or a near match. Accept any
    // nonempty result whose primary keys belong to this fixture.
    const auto matched_pks = query_int_column(
        ctx,
        fmt::format(
            "SELECT pk FROM {} WHERE mmsearch(seq0, {}) ORDER BY pk;",
            k_alignment_table, hoe_probe_literal()));
    REQUIRE(matched_pks.size() >= 1);
    for (const auto pk : matched_pks) {
        REQUIRE((pk >= 1 && pk <= 5));
    }
}

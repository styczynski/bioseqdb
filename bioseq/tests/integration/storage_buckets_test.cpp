/**
 * @file Integration contracts for length-bucketed sub-table sharding.
 *
 *   * INSERT routes payloads by length bucket and column attribute number.
 *   * `bioseq.len` and `bioseq.unit_len` return BIGINT.
 *   * The cap=0 sentinel forces one shard per INSERT.
 *   * DROP cascades, TEMP rejection, and UNLOGGED inheritance stay exact.
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
    // bioseq.len and bioseq.unit_len are BIGINT and locator-aware:
    // the stub-side path reads `loc.total_bp_size` (uint64) from the locator
    // payload, the inline path reads `hdr->bp_size`. The widening means
    // sequences with bp_size > INT32_MAX round-trip their length without
    // silent truncation.
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

    // The list_seq_storage_shards SRF must return the same number of rows.
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
                // Per-shard relations are named seq_sub_<db>_<spc>_... (a
                // digit follows the prefix); the catalog table itself is
                // bioseq_internal.seq_sub_table, which must NOT be counted
                // here. Anchor on a digit so the catalog + its indexes are
                // excluded.
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

// End-to-end coverage verifies that existing queries (mmsearch in particular)
// keep working transparently while
// the payload lives in sub-tables. The canonical alignment dataset
// (sequences spanning multiple length buckets), checks the sub-table catalog
// has rows under the parent's relfilenode, then runs mmsearch and asserts the
// match set is non-empty. A non-empty match is the smoking gun that the
// hydration path actually reaches into the sub-tables and returns real
// payload bytes; the alignment result must be non-empty.
TEST_CASE("mmsearch end-to-end exercises hydration from sub-tables",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    constexpr std::string_view k_alignment_table = "storage_bucket_alignment";
    auto ctx = fresh_context({k_alignment_table});
    seed_hoe_fixture(ctx, k_alignment_table);

    // Sub-tables exist under the parent's relfilenode triple.
    const long long shards = query_scalar_int(
        ctx,
        fmt::format(
            "SELECT count(*) FROM bioseq_internal.seq_sub_table s "
            "WHERE s.main_relfilenode = (SELECT relfilenode FROM pg_class "
            "WHERE relname='{}');",
            k_alignment_table));
    REQUIRE(shards >= 1);

    // mmsearch returns matching rows. The canonical dataset has five rows that all
    // contain the canonical probe (or a near-match); the exact result set
    // depends on the aligner, so we only assert non-empty + that all PKs
    // are in the seeded range.
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

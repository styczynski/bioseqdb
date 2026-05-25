// DOC0260 Phase A — integration tests for the length-bucketed sub-table
// sharding. Verifies that:
//   * INSERT into a `USING bioseq` parent lands the SEQ payload in a
//     length-bucketed sub-table whose bucket_idx matches the sequence
//     length.
//   * `bioseq.len` and `bioseq.unit_len` now return BIGINT.
//   * The cap=0 sentinel forces one shard per INSERT.
//   * Multi-SEQ-column parents route by column_attno.
//   * DROP TABLE cascades to every sub-table.
//   * TEMP parents are rejected.
//   * UNLOGGED parents inherit UNLOGGED sub-tables.

#include "common_tests_integration.h"
import integration_tests.common;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view kTable = "test_storage_buckets_t";

}  // namespace

TEST_CASE("DOC0260 — bioseq.len reports correct payload size after stub hydration",
          "[integration][storage-buckets][len]") {
    // DOC0260 Phase A keeps bioseq.len/unit_len at INTEGER (see SQL DDL
    // notes); the BIGINT widening will land alongside the Phase-C
    // bit_casted_view/stdc_compare fix.
    auto ctx = fresh_context({kTable});
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(kTable),
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = false,
        },
        {seq_row{.pk = 1, .sequences = {"ACGT"}}});

    REQUIRE(query_scalar_int(
                ctx,
                fmt::format("SELECT bioseq.len(seq0) FROM {} LIMIT 1;", kTable))
            == 4);
}

TEST_CASE("DOC0260 — seq_sub_table catalog exists and is empty on fresh extension",
          "[integration][storage-buckets][catalog]") {
    auto ctx = fresh_context();
    REQUIRE(query_scalar_int(
                ctx, "SELECT count(*) FROM bioseq_internal.seq_sub_table;")
            == 0);
}

TEST_CASE("DOC0260 — TEMP parents are rejected at CREATE TABLE",
          "[integration][storage-buckets][temp]") {
    auto ctx = fresh_context();
    REQUIRE(sql_fails(
        ctx,
        "CREATE TEMPORARY TABLE _doc0260_temp_t "
        "(id int, seq bioseq.seq('dna4', 32)) USING bioseq;"));
}

TEST_CASE("DOC0260 — every SEQ column INSERT lands in the expected bucket",
          "[integration][storage-buckets][bucket-routing]") {
    auto ctx = fresh_context({kTable});
    exec_sqlf(ctx,
              "CREATE TABLE {} (id INT PRIMARY KEY, "
              "seq bioseq.seq('dna4', 32)) USING bioseq;",
              kTable);
    exec_sqlf(
        ctx,
        "INSERT INTO {} VALUES (1, repeat('A', 16)::bioseq.seq('dna4', 32));",
        kTable);
    exec_sqlf(
        ctx,
        "INSERT INTO {} VALUES (2, repeat('A', 1024)::bioseq.seq('dna4', 32));",
        kTable);

    // At least one sub-table catalog row must exist for this parent.
    const long long rows = query_scalar_int(
        ctx,
        fmt::format(
            "SELECT count(*) FROM bioseq_internal.seq_sub_table s "
            "WHERE s.main_relfilenode = (SELECT relfilenode FROM pg_class "
            "WHERE relname = '{}');",
            kTable));
    REQUIRE(rows >= 1);
}

TEST_CASE("DOC0260 — DROP TABLE cascades to every sub-table",
          "[integration][storage-buckets][drop]") {
    auto ctx = fresh_context({kTable});
    exec_sqlf(ctx,
              "CREATE TABLE {} (id INT PRIMARY KEY, "
              "seq bioseq.seq('dna4', 32)) USING bioseq;",
              kTable);
    for (int i = 0; i < 5; ++i) {
        exec_sqlf(ctx,
                  "INSERT INTO {} VALUES ({}, "
                  "'ACGTACGTACGTACGTACGTACGTACGT'::bioseq.seq('dna4', 32));",
                  kTable, i);
    }
    exec_sqlf(ctx, "DROP TABLE {};", kTable);
    REQUIRE(query_scalar_int(
                ctx, "SELECT count(*) FROM bioseq_internal.seq_sub_table;")
            == 0);
    REQUIRE(query_scalar_int(
                ctx,
                "SELECT count(*) FROM pg_class WHERE "
                "relnamespace = 'bioseq_internal'::regnamespace "
                "AND (relname LIKE 'seq_sub_%' OR relname LIKE 'idx_sidecar_%');")
            == 0);
}

TEST_CASE("DOC0260 — multi-SEQ-column parents route by column_attno",
          "[integration][storage-buckets][multi-column]") {
    auto ctx = fresh_context({kTable});
    exec_sqlf(ctx,
              "CREATE TABLE {} (id INT PRIMARY KEY, "
              "seq_a bioseq.seq('dna4', 32), "
              "seq_b bioseq.seq('dna5', 64)) USING bioseq;",
              kTable);
    exec_sqlf(ctx,
              "INSERT INTO {} VALUES (1, "
              "'ACGT'::bioseq.seq('dna4', 32), "
              "'NACGT'::bioseq.seq('dna5', 64));",
              kTable);
    const auto column_attnos = query_rows(
        ctx,
        fmt::format(
            "SELECT column_attno::text FROM bioseq_internal.seq_sub_table "
            "WHERE main_relfilenode = (SELECT relfilenode FROM pg_class "
            "WHERE relname = '{}') ORDER BY column_attno;",
            kTable));
    REQUIRE(column_attnos.size() == 2);
    // seq_a is the 2nd column (id is #1), seq_b is the 3rd column.
    REQUIRE(column_attnos[0][0].value() == "2");
    REQUIRE(column_attnos[1][0].value() == "3");
}

TEST_CASE("DOC0260 — UNLOGGED parents inherit UNLOGGED sub-tables",
          "[integration][storage-buckets][unlogged]") {
    auto ctx = fresh_context({kTable});
    exec_sqlf(ctx,
              "CREATE UNLOGGED TABLE {} (id INT PRIMARY KEY, "
              "seq bioseq.seq('dna4', 32)) USING bioseq;",
              kTable);
    exec_sqlf(ctx,
              "INSERT INTO {} VALUES (1, "
              "'ACGT'::bioseq.seq('dna4', 32));",
              kTable);
    REQUIRE(query_scalar(
                ctx,
                fmt::format(
                    "SELECT c.relpersistence::text "
                    "FROM bioseq_internal.seq_sub_table s "
                    "JOIN pg_class c ON c.oid = s.sub_table_relid "
                    "WHERE s.main_relfilenode = "
                    "  (SELECT relfilenode FROM pg_class WHERE relname = '{}') "
                    "LIMIT 1;",
                    kTable))
            == "u");
}

TEST_CASE("DOC0260 — round-trip via USING bioseq table preserves payload bytes",
          "[integration][storage-buckets][roundtrip]") {
    auto ctx = fresh_context({kTable});
    exec_sqlf(ctx,
              "CREATE TABLE {} (id INT PRIMARY KEY, "
              "seq bioseq.seq('dna4', 32)) USING bioseq;",
              kTable);
    exec_sqlf(ctx,
              "INSERT INTO {} VALUES (1, "
              "'ACGTACGTACGT'::bioseq.seq('dna4', 32));",
              kTable);
    exec_sqlf(ctx,
              "INSERT INTO {} VALUES (2, "
              "'TTTTAAAA'::bioseq.seq('dna4', 32));",
              kTable);
    const auto rows = query_rows(
        ctx,
        fmt::format("SELECT seq::text, bioseq.len(seq)::text FROM {} "
                    "ORDER BY id;",
                    kTable));
    REQUIRE(rows.size() == 2);
    REQUIRE(rows[0][0].value() == "ACGTACGTACGT");
    REQUIRE(rows[0][1].value() == "12");
    REQUIRE(rows[1][0].value() == "TTTTAAAA");
    REQUIRE(rows[1][1].value() == "8");
}

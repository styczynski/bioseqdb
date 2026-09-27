/**
 * @file sequence_basic_test.cpp
 * @brief Tests the bioseq.seq SQL type.
 *
 * Checks text round trips, lengths, ordering, comparisons, and hashes. Heap
 * tables keep the type tests separate from Bioseq table scans. Other cases
 * check expanded-value ownership, custom sorting, and invalid tgpe modifiers.
 */

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

#include <catch2/catch_template_test_macros.hpp>

import std;
import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view k_table = "sequence_basic";

}  // namespace

TEST_CASE("bioseq.seq round-trips ASCII payloads on heap tables",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
  auto ctx = fresh_context({k_table});
  create_seq_table(
      ctx,
      seq_table_spec{
          .name = std::string(k_table),
          .typmod = {.alphabet = "dna4", .width = 64},
          .seq_columns = 1,
          .using_bioseq = false,
      },
      {
          seq_row{.pk = 23, .sequences = {"ACTAGGCACAT"}},
          seq_row{.pk = 101,
                  .sequences = {"GGGGTTTTACTAGGCACATACTAGGCACATACT"}},
          seq_row{
              .pk = 69,
              .sequences = {"ACGTACGTACGTACGTACGTACGTACGTACGTACGTACGTACGT"}},
          seq_row{.pk = 42, .sequences = {"TTTTTT"}},
      });

  const auto rows = query_rows(
      ctx, fmt::format(
               "SELECT pk, seq0::text, bioseq.len(seq0), bioseq.unit_len(seq0) "
               "FROM {} ORDER BY pk;",
               k_table));

  REQUIRE(rows.size() == 4);
  REQUIRE(rows[0][1].value() == "ACTAGGCACAT");
  REQUIRE(rows[0][2].value() == "11");
  REQUIRE(rows[1][1].value() == "TTTTTT");
  REQUIRE(rows[1][2].value() == "6");
  REQUIRE(rows[2][1].value() == "ACGTACGTACGTACGTACGTACGTACGTACGTACGTACGTACGT");
  REQUIRE(rows[2][2].value() == "44");
  REQUIRE(rows[3][1].value() == "GGGGTTTTACTAGGCACATACTAGGCACATACT");
  REQUIRE(rows[3][2].value() == "33");
}

// Sort sequence values through the heap-table ORDER BY path and, compare
// the row order with the expected lexicographic order.
TEST_CASE("ORDER BY bioseq.seq follows the documented lexicographic order",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
  auto ctx = fresh_context({k_table});
  create_seq_table(
      ctx,
      seq_table_spec{
          .name = std::string(k_table),
          .typmod = {.alphabet = "dna4", .width = 64},
          .seq_columns = 1,
      },
      {
          seq_row{.pk = 1, .sequences = {"ACGT"}},
          seq_row{.pk = 2, .sequences = {"ACTA"}},
          seq_row{.pk = 3, .sequences = {"AC"}}, // shorter prefix sorts first
          seq_row{.pk = 4, .sequences = {"GGGG"}},
          seq_row{.pk = 5, .sequences = {"AAAA"}},
      });

  const auto pks = query_int_column(
      ctx, fmt::format("SELECT pk FROM {} ORDER BY seq0;", k_table));
  // Expected lexicographic order: AAAA, AC, ACGT, ACTA, GGGG -> pk 5,3,1,2,4.
  REQUIRE(pks == std::vector<long long>{5, 3, 1, 2, 4});
}

TEST_CASE("expanded sequence transfer and copy survive input-context reset",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
  auto ctx = fresh_context();
  REQUIRE(query_scalar(
              ctx, "SELECT bioseq_internal.expanded_ownership_self_test();") ==
          "t");
}

TEST_CASE(
    "BioseqBatchSortScan uses stdc_compare when Vulkan sorting is disabled",
    "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
  auto ctx = fresh_context({k_table});
  create_seq_table(ctx,
                   seq_table_spec{
                       .name = std::string(k_table),
                       .typmod = {.alphabet = "dna4", .width = 64},
                       .seq_columns = 1,
                   },
                   {
                       seq_row{.pk = 1, .sequences = {"ACGT"}},
                       seq_row{.pk = 2, .sequences = {"ACTA"}},
                       seq_row{.pk = 3, .sequences = {"AC"}},
                       seq_row{.pk = 4, .sequences = {"GGGG"}},
                       seq_row{.pk = 5, .sequences = {"AAAA"}},
                   });

  exec_sql(ctx, "SET bioseq.planner.sequence_sort_scan.enabled = on;");
  exec_sql(ctx, "SET bioseq.aligner.vulkan.enabled = off;");

  const std::string sql =
      fmt::format("SELECT pk FROM {} ORDER BY seq0;", k_table);
  const auto pks = query_int_column(ctx, sql);
  REQUIRE(pks == std::vector<long long>{5, 3, 1, 2, 4});

  const auto plan = explain_text<true>(ctx, sql);
  REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchSortScan)"));
  REQUIRE(plan_contains(plan, "Bioseq Sort Accelerator: stdc_compare"));
  REQUIRE(plan_contains(plan, "Bioseq GPU Sort Dispatches: 0"));
  REQUIRE(plan_contains(plan, "Bioseq GPU Global Compare Dispatches: 0"));
  REQUIRE(plan_contains(plan, "Bioseq GPU Dense Scatter Dispatches: 0"));
}

TEMPLATE_TEST_CASE_SIG(
    "bioseq.seq accepts the empty sequence for every supported typmod",
    "[component:bioseq][layer:integration][feature:sequence][resource:postgres]",
    ((int p_width), p_width),
    8, 16, 32, 64) {
    auto ctx = fresh_context();
#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    for (const auto alphabet : {std::string_view("dna4")}) {
#else
    for (const auto alphabet : {std::string_view("dna4"), std::string_view("dna5")}) {
#endif
        const seq_typmod typmod{.alphabet = alphabet, .width = p_width};
        const auto text = query_scalar(
            ctx, fmt::format("SELECT {}::text;", format_literal("", typmod)));
        REQUIRE(text == "");
        REQUIRE(query_scalar_int(
                    ctx, fmt::format("SELECT bioseq.len({});",
                                     format_literal("", typmod))) == 0);
        REQUIRE(query_scalar_int(
                    ctx, fmt::format("SELECT bioseq.unit_len({});",
                                     format_literal("", typmod))) == 0);
    }
#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    const auto unsupported_alphabet = sql_error(
        ctx,
        fmt::format("SELECT ''::bioseq.seq('dna5', {})::text;", p_width));
    REQUIRE(unsupported_alphabet.sqlstate == "22023");
    REQUIRE(unsupported_alphabet.contains(
        "bioseq.seq alphabet \"dna5\" is not supported"));
    REQUIRE(unsupported_alphabet.contains("Valid alphabets: 'dna4'"));
#endif
}

// Store  empty and nonempty sequences together in a heap table. Reading
// them back must preserve their text and lengths.
TEST_CASE("Empty literal survives INSERT-then-SELECT on heap table",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(k_table),
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
        },
        {
            seq_row{.pk = 1, .sequences = {""}},
            seq_row{.pk = 2, .sequences = {"AAAA"}},
        });

    const auto rows = query_rows(
        ctx,
        fmt::format("SELECT pk, seq0::text, bioseq.len(seq0) FROM {} "
                    "ORDER BY pk;",
                    k_table));
    REQUIRE(rows.size() == 2);
    REQUIRE(rows[0][1].value() == "");
    REQUIRE(rows[0][2].value() == "0");
    REQUIRE(rows[1][1].value() == "AAAA");
    REQUIRE(rows[1][2].value() == "4");
}

TEST_CASE("bioseq.seq comparison operators yield lexicographic ordering",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context();
    const auto row =
        query_one(ctx,
                  "SELECT (a OPERATOR(bioseq.<) b) AS lt, "
                  "       (a OPERATOR(bioseq.=) b) AS eq, "
                  "       (a OPERATOR(bioseq.>) b) AS gt, "
                  "       bioseq.seq_cmp(a, b) AS cmp "
                  "FROM (SELECT 'ACGT'::bioseq.seq('dna4', 8) AS a, "
                  "             'ACTA'::bioseq.seq('dna4', 8) AS b) t;");
    REQUIRE(row[0].value() == "t");
    REQUIRE(row[1].value() == "f");
    REQUIRE(row[2].value() == "f");
    const int cmp = std::stoi(row[3].value());
    REQUIRE(cmp < 0);
}

TEST_CASE("bioseq.seq operators do not fall back through assignment-only text casts",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context();
    exec_sql(ctx, "SET search_path TO pg_catalog, public;");
    const auto absent_operator = sql_error(
        ctx,
        "SELECT 'AC'::bioseq.seq('dna4',64) "
        "       = 'AC'::bioseq.seq('dna4',64);");
    REQUIRE(absent_operator.sqlstate == "42883");
    REQUIRE(query_scalar(
        ctx,
        "SELECT 'AC'::bioseq.seq('dna4',64) "
        "       OPERATOR(bioseq.=) "
        "       'AC'::bioseq.seq('dna4',64);") == "t");
}

TEST_CASE("bioseq.seq base hash folds the mandated seed-zero extended hash",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context();
    const auto row = query_one(
        ctx,
        "WITH v(s) AS (VALUES "
        " ('ACGT'::bioseq.seq('dna4',64)),"
        " (''::bioseq.seq('dna4',64)),"
        " ('ACGT'::bioseq.seq)) "
        ", hashes AS (SELECT s, bioseq.seq_hash_extended(s,0) AS h "
        "             FROM v) "
        "SELECT bool_and("
        " (bioseq.seq_hash(s)::bigint & 4294967295) = "
        " ((h # (h >> 32)) & 4294967295)) "
        "FROM hashes;");
    REQUIRE(row[0] == "t");
}

TEST_CASE("bioseq.seq prefix ordering: shorter prefix sorts before its extension",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context();
    const auto row = query_one(
        ctx,
        "SELECT ('A'::bioseq.seq('dna4', 8) "
        "        OPERATOR(bioseq.<) 'AC'::bioseq.seq('dna4', 8)),"
        "       ('AC'::bioseq.seq('dna4', 8) "
        "        OPERATOR(bioseq.=) 'AC'::bioseq.seq('dna4', 8)),"
        "       ('ACGT'::bioseq.seq('dna4', 8) "
        "        OPERATOR(bioseq.>) 'ACG'::bioseq.seq('dna4', 8));");
    REQUIRE(row[0].value() == "t");
    REQUIRE(row[1].value() == "t");
    REQUIRE(row[2].value() == "t");
}

TEST_CASE("bioseq.seq_cmp accepts unequal-typmod operands and orders consistently",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context();
    // If comparison succeeds across storage widths, reversing the operands
    // must reverse its sign
    constexpr std::string_view sql_pair =
        "SELECT bioseq.seq_cmp('ACGT'::bioseq.seq('dna4', 16), "
        "                      'ACGT'::bioseq.seq('dna4', 64));";
    const bool succeeds = !sql_fails(ctx, sql_pair);
    if (succeeds) {
        const int forward = std::stoi(query_scalar(ctx, sql_pair));
        const int reverse = std::stoi(query_scalar(
            ctx,
            "SELECT bioseq.seq_cmp('ACGT'::bioseq.seq('dna4', 64), "
            "                      'ACGT'::bioseq.seq('dna4', 16));"));
        REQUIRE(forward == -reverse);
    }
}

TEST_CASE("bioseq.seq supports equality and range filters on heap tables",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context({k_table});
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(k_table),
            .typmod = {.alphabet = "dna4", .width = 64},
            .seq_columns = 1,
        },
        {
            seq_row{.pk = 23,  .sequences = {"ACTAGGCACAT"}},
            seq_row{.pk = 101, .sequences = {"GGGGTTTTACT"}},
            seq_row{.pk = 69,  .sequences = {"ACGTACGTACGT"}},
            seq_row{.pk = 42,  .sequences = {"TTTTTT"}},
        });

    SECTION("Exact equality") {
        REQUIRE(query_int_column(
                    ctx, fmt::format(
                             "SELECT pk FROM {} "
                             "WHERE seq0 OPERATOR(bioseq.=) "
                             "      'TTTTTT'::bioseq.seq('dna4', 64) "
                             "ORDER BY pk;",
                             k_table)) ==
                std::vector<long long>{42});
    }
    SECTION("Strict less-than") {
        REQUIRE(query_int_column(
                    ctx, fmt::format(
                             "SELECT pk FROM {} "
                             "WHERE seq0 OPERATOR(bioseq.<) "
                             "      'C'::bioseq.seq('dna4', 64) "
                             "ORDER BY pk;",
                             k_table)) ==
                std::vector<long long>{23, 69});
    }
    SECTION("BETWEEN") {
        REQUIRE(query_int_column(
                    ctx, fmt::format(
                             "SELECT pk FROM {} "
                             "WHERE seq0 OPERATOR(bioseq.>=) "
                             "      'A'::bioseq.seq('dna4', 64) "
                             "  AND seq0 OPERATOR(bioseq.<=) "
                             "      'G'::bioseq.seq('dna4', 64) "
                             "ORDER BY pk;",
                             k_table)) ==
                std::vector<long long>{23, 69});
    }
}

TEST_CASE("non-canonical typmod casts follow the active alphabet profile",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context();
    const auto invalid_dna4 = sql_error(
        ctx, "SELECT 'ACGTN'::bioseq.seq('dna4', 16)::text;");
    REQUIRE(invalid_dna4.sqlstate == "22P02");
#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    /* The DNA4-only build rejects N while parsing the unbound input. */
    REQUIRE(invalid_dna4.contains(
        "invalid symbol at byte 4 in unbound bioseq.seq"));
    REQUIRE(invalid_dna4.contains("DETAIL: byte value=78"));
#else
    /* With DNA5 available, input parsing accepts N. Casting that value to
     * DNA4 must then reject it. */
    REQUIRE(invalid_dna4.contains(
        "invalid symbol 'N' for bioseq alphabet dna4"));
    REQUIRE(invalid_dna4.contains("DETAIL: symbol offset=4"));
#endif
#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    const auto unsupported_alphabet = sql_error(
        ctx, "SELECT 'ACGTN'::bioseq.seq('dna5', 16)::text;");
    REQUIRE(unsupported_alphabet.sqlstate == "22023");
    REQUIRE(unsupported_alphabet.contains(
        "bioseq.seq alphabet \"dna5\" is not supported"));
    REQUIRE(unsupported_alphabet.contains("Valid alphabets: 'dna4'"));
#else
    REQUIRE(query_scalar(
        ctx,
        "SELECT 'ACGTN'::bioseq.seq('dna5', 16)::text;") == "ACGTN");
#endif
}

// Each listed invalid type modifier must raise a SQL error.
TEMPLATE_TEST_CASE_SIG(
    "Invalid bioseq.seq typmods raise a SQL error",
    "[component:bioseq][layer:integration][feature:sequence][resource:postgres]",
    ((std::size_t p_index), p_index),
    0, 1, 2, 3, 4, 5, 6) {
    static constexpr std::array<std::string_view, 7> k_invalid_typmods{
        "SELECT 'ACGT'::bioseq.seq('protein', 32);",
        "SELECT 'ACGT'::bioseq.seq('dna4', 128);",
        "SELECT 'ACGT'::bioseq.seq('dna4', 0);",
        "SELECT 'ACGT'::bioseq.seq('dna4', -8);",
        "SELECT 'ACGT'::bioseq.seq('', 32);",
        "SELECT 'ACGT'::bioseq.seq('dna4');",
        "SELECT 'ACGT'::bioseq.seq('dna4', 64, 10);",
    };
    auto ctx = fresh_context();
    REQUIRE(sql_fails(ctx, k_invalid_typmods[p_index]));
}

/**
 * @file sql_boundary_bpchar_test.cpp
 * @brief Exercises BPCHAR physical forms, comparisons, and collation behavior.
 */

#include <tests/support/postgres.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <libpq-fe.h>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

import bioseq.tests.integration.support;
import bioseq.tests.integration.sql_boundary_test_support;

using namespace bioseq_int_tests;
using namespace bioseq_int_tests::sql_boundary_test_support;

TEST_CASE(
    "BPCHAR owns every PostgreSQL physical varlena form",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();
  exec_sqlf(ctx, "DROP TABLE IF EXISTS {};", k_bpchar_physical_table);
  exec_sql(ctx, "SET default_toast_compression = 'pglz';");
  exec_sqlf(
      ctx,
      "CREATE TEMP TABLE {} ("
      "    id integer PRIMARY KEY,"
      "    value bpchar"
      ");",
      k_bpchar_physical_table);
  exec_sqlf(
      ctx,
      "ALTER TABLE {} SET (toast_tuple_target = 128);",
      k_bpchar_physical_table);

  // EXTENDED allows heap formation to select the one-byte short header.
  exec_sqlf(
      ctx,
      "INSERT INTO {} VALUES "
      "    (1, ('a' || repeat(' ', 3))::bpchar);",
      k_bpchar_physical_table);
  // PLAIN forbids compression/toasting and the payload is too large for a
  // one-byte header, forcing a 4-byte uncompressed datum.
  exec_sqlf(
      ctx,
      "ALTER TABLE {} ALTER COLUMN value SET STORAGE PLAIN;",
      k_bpchar_physical_table);
  exec_sqlf(
      ctx,
      "INSERT INTO {} VALUES "
      "    (2, ('b' || repeat(' ', 199))::bpchar);",
      k_bpchar_physical_table);
  // MAIN permits inline compression but not ordinary external storage.
  exec_sqlf(
      ctx,
      "ALTER TABLE {} ALTER COLUMN value SET STORAGE MAIN;",
      k_bpchar_physical_table);
  exec_sqlf(
      ctx,
      "INSERT INTO {} VALUES "
      "    (3, ('c' || repeat(' ', 3999))::bpchar);",
      k_bpchar_physical_table);
  // EXTERNAL disables compression, so this oversized value must be stored in
  // the table's TOAST relation and presented as an on-disk external pointer.
  exec_sqlf(
      ctx,
      "ALTER TABLE {} ALTER COLUMN value SET STORAGE EXTERNAL;",
      k_bpchar_physical_table);
  exec_sqlf(
      ctx,
      "INSERT INTO {} VALUES "
      "    (4, ('b' || repeat(' ', 15999))::bpchar),"
      "    (5, NULL);",
      k_bpchar_physical_table);

  const std::string relation_oid = query_scalar(
      ctx,
      fmt::format(
          "SELECT '{}'::regclass::oid::text;",
          k_bpchar_physical_table));
  exec_sql(
      ctx,
      fmt::format(
          "SELECT bioseq_internal.arm_sequence_source_failure("
          "    'bpchar_physical_probe:{}');",
          relation_oid));

  // The probe resets and reuses a dedicated memory context before returning.
  // A normal catalog read and scalar BPCHAR operation verify the backend and
  // the source table remain usable after every vector payload was released.
  REQUIRE(query_scalar(
              ctx,
              fmt::format(
                  "SELECT (count(*) = 5 AND count(value) = 4)::text "
                  "  FROM {};",
                  k_bpchar_physical_table))
          == "true");
  REQUIRE(query_scalar(
              ctx,
              "SELECT ("
              "  'a'::bpchar = 'a   '::bpchar "
              "  AND 'a'::bpchar < 'b'::bpchar "
              "  AND 'b'::bpchar >= 'b   '::bpchar"
              ")::text;")
          == "true");
  exec_sql(
      ctx,
      "SELECT bioseq_internal.arm_sequence_source_failure("
      "    'bpchar_vector_resolver');");
  exec_sqlf(ctx, "DROP TABLE {};", k_bpchar_physical_table);
}

TEST_CASE("BPCHAR comparisons preserve PostgreSQL parity in both modes",
          "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context({k_bpchar_table});
  exec_sql(
      ctx,
      "SELECT bioseq_internal.arm_sequence_source_failure("
      "    'bpchar_vector_resolver');");
  exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
  exec_sqlf(ctx,
            "CREATE TABLE {} ("
            "    id integer NOT NULL,"
            "    lhs character(4),"
            "    rhs character(8),"
            "    seq bioseq.seq('dna4', 32)"
            ") USING bioseq;",
            k_bpchar_table);
  exec_sqlf(ctx,
            "INSERT INTO {} VALUES "
            "    (1, 'a',    'a   ', 'ACGT'::bioseq.seq('dna4', 32)),"
            "    (2, 'a',    'b',    'ACGT'::bioseq.seq('dna4', 32)),"
            "    (3, NULL,   'a',    'ACGT'::bioseq.seq('dna4', 32)),"
            "    (4, 'z   ', 'z',    'ACGT'::bioseq.seq('dna4', 32));",
            k_bpchar_table);

  const std::string comparison_query =
      fmt::format("SELECT id, "
                  "       lhs = rhs, rhs = lhs, "
                  "       lhs <> rhs, rhs <> lhs, "
                  "       lhs < rhs, rhs > lhs, "
                  "       lhs <= rhs, rhs >= lhs, "
                  "       lhs > rhs, rhs < lhs, "
                  "       lhs >= rhs, rhs <= lhs "
                  "  FROM {} "
                  " WHERE lhs IS NULL OR lhs <= rhs OR lhs > rhs "
                  " ORDER BY id;",
                  k_bpchar_table);

  {
    guc_scope vectorization(ctx, "bioseq.vectorization.enabled", "on");
    const auto plan = explain_text<false>(ctx, comparison_query);
    REQUIRE(plan_contains(plan, "BioseqScan"));
  }

  {
    guc_scope vectorization(ctx, "bioseq.vectorization.enabled", "on");
    REQUIRE(query_rows(ctx, comparison_query).size() == 4);
  }

  const rowset_t rows = expect_invariant_under_guc_toggle(
      ctx, comparison_query, "bioseq.vectorization.enabled");
  const rowset_t expected{
      {
          std::string{"1"},
          std::string{"t"},
          std::string{"t"},
          std::string{"f"},
          std::string{"f"},
          std::string{"f"},
          std::string{"f"},
          std::string{"t"},
          std::string{"t"},
          std::string{"f"},
          std::string{"f"},
          std::string{"t"},
          std::string{"t"},
      },
      {
          std::string{"2"},
          std::string{"f"},
          std::string{"f"},
          std::string{"t"},
          std::string{"t"},
          std::string{"t"},
          std::string{"t"},
          std::string{"t"},
          std::string{"t"},
          std::string{"f"},
          std::string{"f"},
          std::string{"f"},
          std::string{"f"},
      },
      {
          std::string{"3"},
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
      },
      {
          std::string{"4"},
          std::string{"t"},
          std::string{"t"},
          std::string{"f"},
          std::string{"f"},
          std::string{"f"},
          std::string{"f"},
          std::string{"t"},
          std::string{"t"},
          std::string{"f"},
          std::string{"f"},
          std::string{"t"},
          std::string{"t"},
      },
  };
  REQUIRE(rows == expected);

  const std::string constant_directions =
      fmt::format("SELECT id FROM {} "
                  "WHERE 'a'::character(4) <= lhs "
                  "  AND lhs >= 'a'::character(4) "
                  "ORDER BY id;",
                  k_bpchar_table);
  {
    guc_scope vectorization(ctx, "bioseq.vectorization.enabled", "on");
    REQUIRE(plan_contains(
        explain_text<false>(ctx, constant_directions), "BioseqScan"));
  }
  REQUIRE(expect_invariant_under_guc_toggle(
              ctx,
              constant_directions,
              "bioseq.vectorization.enabled")
          == rowset_t{{std::string{"1"}},
                      {std::string{"2"}},
                      {std::string{"4"}}});

  const std::string non_folded_null =
      fmt::format("SELECT id FROM {} "
                  "WHERE lhs = (SELECT NULL::character(4) OFFSET 0) "
                  "ORDER BY id;",
                  k_bpchar_table);
  REQUIRE(expect_invariant_under_guc_toggle(
              ctx,
              non_folded_null,
              "bioseq.vectorization.enabled")
          .empty());
}

TEST_CASE(
    "BPCHAR comparisons honor nondeterministic ICU collation",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context({k_bpchar_icu_table});
  if (query_scalar(
          ctx,
          "SELECT EXISTS ("
          "    SELECT 1 FROM pg_catalog.pg_collation "
          "     WHERE collprovider = 'i'::\"char\")::text;") != "true") {
    SKIP("PostgreSQL was built without an ICU collation provider");
  }

  exec_sqlf(
      ctx, "DROP SCHEMA IF EXISTS {} CASCADE;", k_bpchar_icu_schema);
  exec_sqlf(ctx, "CREATE SCHEMA {};", k_bpchar_icu_schema);
  exec_sqlf(
      ctx,
      "CREATE COLLATION {}.nondeterministic_ci "
      "(provider = icu, locale = 'und-u-ks-level1', "
      " deterministic = false);",
      k_bpchar_icu_schema);
  exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
  exec_sqlf(
      ctx,
      "CREATE TABLE {} ("
      "    id integer NOT NULL,"
      "    probe_ci character(8) COLLATE "
      "        {}.nondeterministic_ci,"
      "    seq bioseq.seq('dna4', 32)"
      ") USING bioseq;",
      k_bpchar_icu_table, k_bpchar_icu_schema);
  exec_sqlf(
      ctx,
      "INSERT INTO {} VALUES "
      "    (1, 'A', 'ACGT'::bioseq.seq('dna4', 32)),"
      "    (2, 'a', 'ACGT'::bioseq.seq('dna4', 32)),"
      "    (3, 'B', 'ACGT'::bioseq.seq('dna4', 32)),"
      "    (4, 'á', 'ACGT'::bioseq.seq('dna4', 32)),"
      "    (5, NULL, 'ACGT'::bioseq.seq('dna4', 32));",
      k_bpchar_icu_table);

  REQUIRE(query_scalar(
              ctx,
              fmt::format(
                  "SELECT ("
                  "  'A'::bpchar COLLATE {}.nondeterministic_ci = "
                  "  'a'::bpchar COLLATE {}.nondeterministic_ci"
                  ")::text;",
                  k_bpchar_icu_schema, k_bpchar_icu_schema)) == "true");

  const std::string comparison_query = fmt::format(
      "SELECT id FROM {} "
      " WHERE probe_ci = ("
      "   'a'::bpchar COLLATE {}.nondeterministic_ci) "
      " ORDER BY id;",
      k_bpchar_icu_table, k_bpchar_icu_schema);
  {
    guc_scope vectorization(ctx, "bioseq.vectorization.enabled", "on");
    REQUIRE(plan_contains(
        explain_text<false>(ctx, comparison_query), "BioseqScan"));
  }
  REQUIRE(expect_invariant_under_guc_toggle(
              ctx,
              comparison_query,
              "bioseq.vectorization.enabled")
          == rowset_t{{std::string{"1"}},
                      {std::string{"2"}},
                      {std::string{"4"}}});

  exec_sqlf(
      ctx, "DROP SCHEMA {} CASCADE;", k_bpchar_icu_schema);
}

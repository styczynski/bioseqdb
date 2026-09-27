/**
 * @file sql_boundary_generated_registry_test.cpp
 * @brief Tests generated SQL comparisons, aggregates, argument handling, and catalog entries.
 */

#include <libpq-fe.h>

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;
import bioseq.tests.integration.sql_boundary_test_support;

using namespace bioseq_int_tests;
using namespace bioseq_int_tests::sql_boundary_test_support;

TEST_CASE("generated vector comparison registry preserves scalar parity",
          "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context({k_vector_comparison_table});

  const auto catalog_associations = query_one(ctx, R"sql(
WITH families(stem, left_type, right_type) AS (
    VALUES
      ('vbpchar',       'pg_catalog.bpchar'::regtype::oid,
                         'pg_catalog.bpchar'::regtype::oid),
      ('vint2',         'pg_catalog.int2'::regtype::oid,
                         'pg_catalog.int2'::regtype::oid),
      ('vint24',        'pg_catalog.int2'::regtype::oid,
                         'pg_catalog.int4'::regtype::oid),
      ('vint28',        'pg_catalog.int2'::regtype::oid,
                         'pg_catalog.int8'::regtype::oid),
      ('vint4',         'pg_catalog.int4'::regtype::oid,
                         'pg_catalog.int4'::regtype::oid),
      ('vint42',        'pg_catalog.int4'::regtype::oid,
                         'pg_catalog.int2'::regtype::oid),
      ('vint48',        'pg_catalog.int4'::regtype::oid,
                         'pg_catalog.int8'::regtype::oid),
      ('vint8',         'pg_catalog.int8'::regtype::oid,
                         'pg_catalog.int8'::regtype::oid),
      ('vint82',        'pg_catalog.int8'::regtype::oid,
                         'pg_catalog.int2'::regtype::oid),
      ('vint84',        'pg_catalog.int8'::regtype::oid,
                         'pg_catalog.int4'::regtype::oid),
      ('vdate_',        'pg_catalog.date'::regtype::oid,
                         'pg_catalog.date'::regtype::oid),
      ('vtime_',        'pg_catalog.time'::regtype::oid,
                         'pg_catalog.time'::regtype::oid),
      ('vtimestamp_',   'pg_catalog.timestamp'::regtype::oid,
                         'pg_catalog.timestamp'::regtype::oid),
      ('vtimestamptz_', 'pg_catalog.timestamptz'::regtype::oid,
                         'pg_catalog.timestamptz'::regtype::oid)
), operations(op) AS (
    VALUES ('eq'), ('ne'), ('gt'), ('lt'), ('le'), ('ge')
), expected AS (
    SELECT stem || op AS vector_name,
           substring(stem || op FROM 2) AS scalar_name,
           left_type,
           right_type,
           'pg_catalog.bool'::regtype::oid AS result_type
      FROM families CROSS JOIN operations
), actual AS (
    SELECT p.oid, p.proname::text AS vector_name, p.pronargs,
           p.proargtypes[0] AS left_type,
           p.proargtypes[1] AS right_type,
           p.prorettype AS result_type
      FROM pg_catalog.pg_proc AS p
      JOIN pg_catalog.pg_depend AS dep
        ON dep.classid = 'pg_catalog.pg_proc'::regclass
       AND dep.objid = p.oid
       AND dep.deptype = 'e'
      JOIN pg_catalog.pg_extension AS ext
        ON ext.oid = dep.refobjid
       AND ext.extname = 'bioseq'
     WHERE p.prokind = 'f'
       AND p.proname ~
           '^(vbpchar(eq|ne|gt|lt|le|ge)|vint(2|4|8|24|28|42|48|82|84)(eq|ne|gt|lt|le|ge)|v(date|time|timestamp|timestamptz)_(eq|ne|gt|lt|le|ge))$'
), scalar_catalog AS (
    SELECT e.vector_name, count(p.oid) AS matches
      FROM expected AS e
      LEFT JOIN pg_catalog.pg_proc AS p
        ON p.pronamespace = 'pg_catalog'::regnamespace
       AND p.prokind = 'f'
       AND p.proname::text = e.scalar_name
       AND p.pronargs = 2
       AND p.proargtypes[0] = e.left_type
       AND p.proargtypes[1] = e.right_type
       AND p.prorettype = e.result_type
     GROUP BY e.vector_name
)
SELECT (SELECT count(*) FROM expected)::text,
       (SELECT count(*) FROM actual)::text,
       (SELECT count(*)
          FROM expected AS e
          LEFT JOIN actual AS a USING (vector_name)
         WHERE a.oid IS NULL)::text,
       (SELECT count(*)
          FROM actual AS a
          LEFT JOIN expected AS e USING (vector_name)
         WHERE e.vector_name IS NULL)::text,
       (SELECT count(*)
          FROM expected AS e
          JOIN actual AS a USING (vector_name)
          JOIN scalar_catalog AS s USING (vector_name)
         WHERE a.pronargs <> 2
            OR a.left_type <> e.left_type
            OR a.right_type <> e.right_type
            OR a.result_type <> e.result_type
            OR s.matches <> 1)::text;
)sql");
  REQUIRE(catalog_associations.size() == 5);
  REQUIRE(catalog_associations[0].value() == "84");
  REQUIRE(catalog_associations[1].value() == "84");
  REQUIRE(catalog_associations[2].value() == "0");
  REQUIRE(catalog_associations[3].value() == "0");
  REQUIRE(catalog_associations[4].value() == "0");

  exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
  exec_sqlf(ctx,
            "CREATE TABLE {} ("
            "    id integer NOT NULL,"
            "    i2 smallint,"
            "    i4 integer,"
            "    i8 bigint,"
            "    d date,"
            "    t time,"
            "    ts timestamp,"
            "    tz timestamptz,"
            "    c character(4),"
            "    seq bioseq.seq('dna4', 32)"
            ") USING bioseq;",
            k_vector_comparison_table);
  exec_sqlf(ctx,
            "INSERT INTO {} VALUES "
            "    (1, 3, 40, 20, DATE '2024-01-01', TIME '11:00',"
            "        TIMESTAMP '2024-01-01 13:00',"
            "        TIMESTAMPTZ '2024-01-02 00:00+00',"
            "        'm',"
            "        'ACGT'::bioseq.seq('dna4', 32)),"
            "    (2, 5, 41, 20, DATE '2024-01-01', TIME '11:00',"
            "        TIMESTAMP '2024-01-01 13:00',"
            "        TIMESTAMPTZ '2024-01-02 00:00+00',"
            "        'n',"
            "        'ACGT'::bioseq.seq('dna4', 32)),"
            "    (3, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,"
            "        'ACGT'::bioseq.seq('dna4', 32)),"
            "    (4, 32767, -1, 11, 'infinity'::date, TIME '00:00',"
            "        'infinity'::timestamp,"
            "        'infinity'::timestamptz,"
            "        'z',"
            "        'ACGT'::bioseq.seq('dna4', 32));",
            k_vector_comparison_table);

  const std::string comparison_query =
      fmt::format("SELECT id "
                  "  FROM {} "
                  " WHERE 40000::integer > i2 "
                  "   AND i4 <= 40::bigint "
                  "   AND 10::smallint < i8 "
                  "   AND d >= DATE '2024-01-01' "
                  "   AND TIME '12:00' > t "
                  "   AND ts >= TIMESTAMP '2024-01-01 12:00' "
                  "   AND TIMESTAMPTZ '2024-01-01 00:00+00' < tz "
                  " ORDER BY id;",
                  k_vector_comparison_table);

  {
    guc_scope vectorization(ctx, "bioseq.vectorization.enabled", "on");
    const auto plan = explain_text<false>(ctx, comparison_query);
    REQUIRE(plan_contains(plan, "BioseqScan"));
  }

  const rowset_t rows = expect_invariant_under_guc_toggle(
      ctx, comparison_query, "bioseq.vectorization.enabled");
  REQUIRE(rows == rowset_t{{std::string{"1"}}, {std::string{"4"}}});

  const std::string null_constant_query =
      fmt::format("SELECT id FROM {} "
                  "WHERE i4 = NULL::integer ORDER BY id;",
                  k_vector_comparison_table);
  REQUIRE(expect_invariant_under_guc_toggle(
              ctx,
              null_constant_query,
              "bioseq.vectorization.enabled")
          .empty());

  const std::string non_folded_null_query =
      fmt::format("SELECT id FROM {} "
                  "WHERE i4 = (SELECT NULL::integer OFFSET 0) "
                  "ORDER BY id;",
                  k_vector_comparison_table);
  REQUIRE(expect_invariant_under_guc_toggle(
              ctx,
              non_folded_null_query,
              "bioseq.vectorization.enabled")
          .empty());

  struct resolver_family {
    std::string_view left_literal;
    std::string_view right_column;
  };
  static constexpr std::array resolver_families{
      resolver_family{"'m'::character(4)", "c"},
      resolver_family{"3::smallint", "i2"},
      resolver_family{"3::smallint", "i4"},
      resolver_family{"3::smallint", "i8"},
      resolver_family{"40::integer", "i4"},
      resolver_family{"40::integer", "i2"},
      resolver_family{"40::integer", "i8"},
      resolver_family{"20::bigint", "i8"},
      resolver_family{"20::bigint", "i2"},
      resolver_family{"20::bigint", "i4"},
      resolver_family{"DATE '2024-01-01'", "d"},
      resolver_family{"TIME '12:00'", "t"},
      resolver_family{
          "TIMESTAMP '2024-01-01 12:00'", "ts"},
      resolver_family{
          "TIMESTAMPTZ '2024-01-01 00:00+00'", "tz"},
  };
  static constexpr std::array<std::string_view, 6> operators{
      "=", "<>", ">", "<", "<=", ">="};
  for (const resolver_family &family : resolver_families) {
    for (const std::string_view operation : operators) {
      const std::string query = fmt::format(
          "SELECT count(*)::text FROM {} "
          "WHERE ({} {} {}) IS NOT FALSE;",
          k_vector_comparison_table,
          family.left_literal,
          operation,
          family.right_column);
      {
        guc_scope vectorization(
            ctx, "bioseq.vectorization.enabled", "on");
        REQUIRE(plan_contains(
            explain_text<false>(ctx, query), "BioseqScan"));
      }
      static_cast<void>(expect_invariant_under_guc_toggle(
          ctx, query, "bioseq.vectorization.enabled"));
    }
  }
}

TEST_CASE("generated vector aggregate registry is exact and executable",
          "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context({k_vector_aggregate_table});

  const auto catalog_associations = query_one(ctx, R"sql(
WITH expected(
    scalar_name, vector_name, input_type, state_type, result_type,
    transition_name, final_name, serial_name, deserial_name,
    initial_condition) AS (
  VALUES
    ('count', 'vcount', 0::oid,
     'pg_catalog.int8'::regtype::oid, 'pg_catalog.int8'::regtype::oid,
     'vemptycount', '', '', '', '0'),
    ('count', 'vcount', 2276::oid,
     'pg_catalog.int8'::regtype::oid, 'pg_catalog.int8'::regtype::oid,
     'vanycount', '', '', '', '0'),
    ('sum', 'vsum', 'pg_catalog.int2'::regtype::oid,
     'pg_catalog.int8'::regtype::oid, 'pg_catalog.int8'::regtype::oid,
     'vint2sum', '', '', '', '0'),
    ('avg', 'vavg', 'pg_catalog.int2'::regtype::oid,
     'pg_catalog.internal'::regtype::oid,
     'pg_catalog.numeric'::regtype::oid,
     'vint2acc', 'vint2int4avg', '', '', '{0,0}'),
    ('max', 'vmax', 'pg_catalog.int2'::regtype::oid,
     'pg_catalog.int2'::regtype::oid, 'pg_catalog.int2'::regtype::oid,
     'vint2larger', '', '', '', '-32768'),
    ('min', 'vmin', 'pg_catalog.int2'::regtype::oid,
     'pg_catalog.int2'::regtype::oid, 'pg_catalog.int2'::regtype::oid,
     'vint2smaller', '', '', '', '32767'),
    ('sum', 'vsum', 'pg_catalog.int4'::regtype::oid,
     'pg_catalog.int8'::regtype::oid, 'pg_catalog.int8'::regtype::oid,
     'vint4sum', '', '', '', '0'),
    ('avg', 'vavg', 'pg_catalog.int4'::regtype::oid,
     'pg_catalog.internal'::regtype::oid,
     'pg_catalog.numeric'::regtype::oid,
     'vint4acc', 'vint2int4avg', '', '', '{0,0}'),
    ('max', 'vmax', 'pg_catalog.int4'::regtype::oid,
     'pg_catalog.int4'::regtype::oid, 'pg_catalog.int4'::regtype::oid,
     'vint4larger', '', '', '', '-2147483648'),
    ('min', 'vmin', 'pg_catalog.int4'::regtype::oid,
     'pg_catalog.int4'::regtype::oid, 'pg_catalog.int4'::regtype::oid,
     'vint4smaller', '', '', '', '2147483647'),
    ('sum', 'vsum', 'pg_catalog.int8'::regtype::oid,
     'pg_catalog.internal'::regtype::oid,
     'pg_catalog.numeric'::regtype::oid,
     'vint8acc', 'vint8sum',
     'int8_avg_serialize', 'int8_avg_deserialize', '<null>'),
    ('avg', 'vavg', 'pg_catalog.int8'::regtype::oid,
     'pg_catalog.internal'::regtype::oid,
     'pg_catalog.numeric'::regtype::oid,
     'vint8acc', 'vint8avg',
     'int8_avg_serialize', 'int8_avg_deserialize', '<null>'),
    ('max', 'vmax', 'pg_catalog.int8'::regtype::oid,
     'pg_catalog.int8'::regtype::oid, 'pg_catalog.int8'::regtype::oid,
     'vint8larger', '', '', '', '-9223372036854775808'),
    ('min', 'vmin', 'pg_catalog.int8'::regtype::oid,
     'pg_catalog.int8'::regtype::oid, 'pg_catalog.int8'::regtype::oid,
     'vint8smaller', '', '', '', '9223372036854775807'),
    ('max', 'vmax', 'pg_catalog.date'::regtype::oid,
     'pg_catalog.date'::regtype::oid, 'pg_catalog.date'::regtype::oid,
     'vdatelarger', '', '', '', '-infinity'),
    ('min', 'vmin', 'pg_catalog.date'::regtype::oid,
     'pg_catalog.date'::regtype::oid, 'pg_catalog.date'::regtype::oid,
     'vdatesmaller', '', '', '', 'infinity')
), actual AS (
  SELECT p.oid, p.proname::text AS vector_name,
         CASE WHEN p.pronargs = 0 THEN 0::oid
              ELSE p.proargtypes[0] END AS input_type,
         a.aggtranstype AS state_type,
         p.prorettype AS result_type,
         trans.proname::text AS transition_name,
         COALESCE(final.proname::text, '') AS final_name,
         COALESCE(serial.proname::text, '') AS serial_name,
         COALESCE(deserial.proname::text, '') AS deserial_name,
         COALESCE(a.agginitval, '<null>') AS initial_condition
    FROM pg_catalog.pg_proc AS p
    JOIN pg_catalog.pg_aggregate AS a ON a.aggfnoid = p.oid
    JOIN pg_catalog.pg_proc AS trans ON trans.oid = a.aggtransfn
    LEFT JOIN pg_catalog.pg_proc AS final ON final.oid = a.aggfinalfn
    LEFT JOIN pg_catalog.pg_proc AS serial ON serial.oid = a.aggserialfn
    LEFT JOIN pg_catalog.pg_proc AS deserial
      ON deserial.oid = a.aggdeserialfn
    JOIN pg_catalog.pg_depend AS dep
      ON dep.classid = 'pg_catalog.pg_proc'::regclass
     AND dep.objid = p.oid
     AND dep.deptype = 'e'
    JOIN pg_catalog.pg_extension AS ext
      ON ext.oid = dep.refobjid
     AND ext.extname = 'bioseq'
   WHERE p.prokind = 'a'
), expected_with_scalar AS (
  SELECT e.*, count(s.oid) AS scalar_matches
    FROM expected AS e
    LEFT JOIN pg_catalog.pg_proc AS s
      ON s.pronamespace = 'pg_catalog'::regnamespace
     AND s.prokind = 'a'
     AND s.proname::text = e.scalar_name
     AND ((e.input_type = 0::oid AND s.pronargs = 0)
       OR (e.input_type <> 0::oid AND s.pronargs = 1
           AND s.proargtypes[0] = e.input_type))
     AND s.prorettype = e.result_type
   GROUP BY e.scalar_name, e.vector_name, e.input_type, e.state_type,
            e.result_type, e.transition_name, e.final_name,
            e.serial_name, e.deserial_name, e.initial_condition
)
SELECT (SELECT count(*) FROM expected)::text,
       (SELECT count(*) FROM actual)::text,
       (SELECT count(*)
          FROM expected AS e
          LEFT JOIN actual AS a
            ON a.vector_name = e.vector_name
           AND a.input_type = e.input_type
         WHERE a.oid IS NULL)::text,
       (SELECT count(*)
          FROM actual AS a
          LEFT JOIN expected AS e
            ON e.vector_name = a.vector_name
           AND e.input_type = a.input_type
         WHERE e.vector_name IS NULL)::text,
       (SELECT count(*)
          FROM expected_with_scalar AS e
          JOIN actual AS a
            ON a.vector_name = e.vector_name
           AND a.input_type = e.input_type
         WHERE e.scalar_matches <> 1
            OR a.state_type <> e.state_type
            OR a.result_type <> e.result_type
            OR a.transition_name <> e.transition_name
            OR a.final_name <> e.final_name
            OR a.serial_name <> e.serial_name
            OR a.deserial_name <> e.deserial_name
            OR a.initial_condition <> e.initial_condition)::text;
)sql");
  REQUIRE(catalog_associations.size() == 5);
  REQUIRE(catalog_associations[0].value() == "16");
  REQUIRE(catalog_associations[1].value() == "16");
  REQUIRE(catalog_associations[2].value() == "0");
  REQUIRE(catalog_associations[3].value() == "0");
  REQUIRE(catalog_associations[4].value() == "0");

  exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
  exec_sqlf(ctx,
            "CREATE TABLE {} ("
            "  id integer NOT NULL,"
            "  i2 smallint,"
            "  i4 integer,"
            "  i8 bigint,"
            "  d date,"
            "  seq bioseq.seq('dna4', 32)"
            ") USING bioseq;",
            k_vector_aggregate_table);
  exec_sqlf(ctx,
            "INSERT INTO {} VALUES "
            " (1, 2, 20, 200, DATE '2024-01-01',"
            "  'ACGT'::bioseq.seq('dna4', 32)),"
            " (2, -3, -30, -300, DATE '2023-01-01',"
            "  'ACGT'::bioseq.seq('dna4', 32)),"
            " (3, 7, 70, 700, DATE '2025-01-01',"
            "  'ACGT'::bioseq.seq('dna4', 32)),"
            " (4, NULL, NULL, NULL, NULL,"
            "  'ACGT'::bioseq.seq('dna4', 32));",
            k_vector_aggregate_table);
  exec_sql(
      ctx,
      "SELECT bioseq_internal.arm_sequence_source_failure("
      "  'aggregate_owner_visits_reset');");

  static constexpr std::array<std::string_view, 16> aggregate_calls{
      "count(*)",
      "count(i2)",
      "sum(i2)",
      "avg(i2)",
      "max(i2)",
      "min(i2)",
      "sum(i4)",
      "avg(i4)",
      "max(i4)",
      "min(i4)",
      "sum(i8)",
      "avg(i8)",
      "max(i8)",
      "min(i8)",
      "max(d)",
      "min(d)",
  };
  for (const std::string_view aggregate_call : aggregate_calls) {
    const std::string query = fmt::format(
        "SELECT {} FROM {};",
        aggregate_call,
        k_vector_aggregate_table);
    {
      guc_scope vectorization(
          ctx, "bioseq.vectorization.enabled", "on");
      REQUIRE(plan_contains(
          explain_text<false>(ctx, query), "VectorAggNode"));
    }
    static_cast<void>(expect_invariant_under_guc_toggle(
        ctx, query, "bioseq.vectorization.enabled"));
  }
  exec_sql(
      ctx,
      "SELECT bioseq_internal.arm_sequence_source_failure("
      "  'aggregate_owner_visits_validate');");
  exec_sql(
      ctx,
      "SELECT bioseq_internal.arm_sequence_source_failure("
      "  'aggregate_int8_serial_roundtrip');");
  REQUIRE(query_scalar(ctx, "SELECT 711::text;") == "711");

  const auto aggregate_matrix_query =
      [](std::string_view predicate) {
        return fmt::format(
            "SELECT count(*)::text, count(i2)::text, "
            "       sum(i2)::text, avg(i2)::text, "
            "       max(i2)::text, min(i2)::text, "
            "       sum(i4)::text, avg(i4)::text, "
            "       max(i4)::text, min(i4)::text, "
            "       sum(i8)::text, avg(i8)::text, "
            "       max(i8)::text, min(i8)::text, "
            "       max(d)::text, min(d)::text "
            "  FROM {} WHERE {};",
            k_vector_aggregate_table,
            predicate);
      };
  const std::string empty_query =
      aggregate_matrix_query("id > 1000");
  const std::string all_null_query =
      aggregate_matrix_query("id = 4");
  {
    guc_scope vectorization(
        ctx, "bioseq.vectorization.enabled", "on");
    REQUIRE(plan_contains(
        explain_text<false>(ctx, empty_query), "VectorAggNode"));
    REQUIRE(plan_contains(
        explain_text<false>(ctx, all_null_query), "VectorAggNode"));
  }
  const row_t vector_empty_expected{
      std::string{"0"},
      std::string{"0"},
      std::string{"0"},
      std::nullopt,
      std::string{"-32768"},
      std::string{"32767"},
      std::string{"0"},
      std::nullopt,
      std::string{"-2147483648"},
      std::string{"2147483647"},
      std::nullopt,
      std::nullopt,
      std::string{"-9223372036854775808"},
      std::string{"9223372036854775807"},
      std::string{"-infinity"},
      std::string{"infinity"},
  };
  const row_t scalar_empty_expected{
      std::string{"0"},
      std::string{"0"},
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
      std::nullopt,
      std::nullopt,
  };
  {
    guc_scope vectorization(
        ctx, "bioseq.vectorization.enabled", "on");
    REQUIRE(query_rows(ctx, empty_query)
            == rowset_t{vector_empty_expected});
    row_t all_null_expected = vector_empty_expected;
    all_null_expected[0] = std::string{"1"};
    REQUIRE(query_rows(ctx, all_null_query)
            == rowset_t{std::move(all_null_expected)});
  }
  {
    guc_scope vectorization(
        ctx, "bioseq.vectorization.enabled", "off");
    REQUIRE(query_rows(ctx, empty_query)
            == rowset_t{scalar_empty_expected});
    row_t all_null_expected = scalar_empty_expected;
    all_null_expected[0] = std::string{"1"};
    REQUIRE(query_rows(ctx, all_null_query)
            == rowset_t{std::move(all_null_expected)});
  }
}

TEST_CASE(
    "aligner_options does not depend on an ambient extension schema",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();
  const std::string extension_schema = query_scalar(
      ctx,
      "SELECT pg_catalog.quote_ident(n.nspname) "
      "FROM pg_catalog.pg_extension AS e "
      "JOIN pg_catalog.pg_namespace AS n ON n.oid = e.extnamespace "
      "WHERE e.extname = 'bioseq';");

  exec_sql(ctx, "BEGIN;");
  exec_sql(ctx, "SET LOCAL search_path = pg_catalog;");
  REQUIRE(query_scalar(
              ctx,
              "SELECT (n.nspname = ANY("
              "  pg_catalog.current_schemas(false)))::int::text "
              "FROM pg_catalog.pg_extension AS e "
              "JOIN pg_catalog.pg_namespace AS n "
              "  ON n.oid = e.extnamespace "
              "WHERE e.extname = 'bioseq';") == "0");

  const row_t options = query_one(
      ctx,
      fmt::format(
          "SELECT ({}.aligner_options(kmer_size => 17)).*;",
          extension_schema));
  REQUIRE(options.size() == 36);
  REQUIRE(options[0].value() == "17");
  REQUIRE(std::ranges::all_of(
      options.begin() + 1,
      options.end(),
      [](const std::optional<std::string> &value) {
        return !value.has_value();
      }));
  exec_sql(ctx, "ROLLBACK;");
}

TEST_CASE(
    "generated function calls preserve positional named and mixed identities",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();

  exec_sql(
      ctx,
      "CREATE TEMP VIEW sql_boundary_from_array_two_positional AS "
      "SELECT bioseq.from_array("
      "  ARRAY['A'::bioseq.letter('dna4'),"
      "        'C'::bioseq.letter('dna4')], 32)::text AS value;");
  exec_sql(
      ctx,
      "CREATE TEMP VIEW sql_boundary_from_array_two_named AS "
      "SELECT bioseq.from_array("
      "  \"array\" => ARRAY['A'::bioseq.letter('dna4'),"
      "                 'C'::bioseq.letter('dna4')],"
      "  width_bits => 32)::text AS value;");
  exec_sql(
      ctx,
      "CREATE TEMP VIEW sql_boundary_from_array_two_mixed AS "
      "SELECT bioseq.from_array("
      "  ARRAY['A'::bioseq.letter('dna4'),"
      "        'C'::bioseq.letter('dna4')],"
      "  width_bits => 32)::text AS value;");

  exec_sql(
      ctx,
      "CREATE TEMP VIEW sql_boundary_from_array_three_positional AS "
      "SELECT bioseq.from_array("
      "  ARRAY['A'::bioseq.letter('dna4'),"
      "        'C'::bioseq.letter('dna4')],"
      "  'dna4', 32)::text AS value;");
  exec_sql(
      ctx,
      "CREATE TEMP VIEW sql_boundary_from_array_three_named AS "
      "SELECT bioseq.from_array("
      "  \"array\" => ARRAY['A'::bioseq.letter('dna4'),"
      "                 'C'::bioseq.letter('dna4')],"
      "  alphabet_name => 'dna4',"
      "  width_bits => 32)::text AS value;");
  exec_sql(
      ctx,
      "CREATE TEMP VIEW sql_boundary_from_array_three_mixed AS "
      "SELECT bioseq.from_array("
      "  ARRAY['A'::bioseq.letter('dna4'),"
      "        'C'::bioseq.letter('dna4')],"
      "  alphabet_name => 'dna4',"
      "  width_bits => 32)::text AS value;");

  exec_sql(
      ctx,
      "CREATE TEMP VIEW sql_boundary_aligner_options_positional AS "
      "SELECT (public.aligner_options(15, 10)).*;");
  exec_sql(
      ctx,
      "CREATE TEMP VIEW sql_boundary_aligner_options_named AS "
      "SELECT (public.aligner_options("
      "  kmer_size => 15, window_size => 10)).*;");
  exec_sql(
      ctx,
      "CREATE TEMP VIEW sql_boundary_aligner_options_mixed AS "
      "SELECT (public.aligner_options("
      "  15, window_size => 10)).*;");

  const std::string from_array_two_oid = query_scalar(
      ctx,
      "SELECT 'bioseq.from_array("
      "bioseq.letter[],integer)'::regprocedure::oid::text;");
  const std::string from_array_three_oid = query_scalar(
      ctx,
      "SELECT 'bioseq.from_array("
      "bioseq.letter[],text,integer)'::regprocedure::oid::text;");
  const std::string aligner_options_oid = query_scalar(
      ctx,
      fmt::format(
          "SELECT 'public.aligner_options({})'"
          "::regprocedure::oid::text;",
          k_aligner_option_types));
  REQUIRE(from_array_two_oid != from_array_three_oid);

  const auto require_dependency =
      [&ctx](std::string_view view_name,
             std::string_view routine_name,
             const std::string &expected_oid) {
        const row_t dependency =
            view_routine_dependency(ctx, view_name, routine_name);
        REQUIRE(dependency.size() == 2);
        REQUIRE(dependency[0].value() == "1");
        REQUIRE(dependency[1].value() == expected_oid);
      };
  static constexpr std::array<std::string_view, 3>
      from_array_two_views{
          "sql_boundary_from_array_two_positional",
          "sql_boundary_from_array_two_named",
          "sql_boundary_from_array_two_mixed",
      };
  static constexpr std::array<std::string_view, 3>
      from_array_three_views{
          "sql_boundary_from_array_three_positional",
          "sql_boundary_from_array_three_named",
          "sql_boundary_from_array_three_mixed",
      };
  static constexpr std::array<std::string_view, 3>
      aligner_options_views{
          "sql_boundary_aligner_options_positional",
          "sql_boundary_aligner_options_named",
          "sql_boundary_aligner_options_mixed",
      };
  for (const std::string_view view : from_array_two_views) {
    require_dependency(view, "from_array", from_array_two_oid);
    REQUIRE(query_scalar(
                ctx,
                fmt::format(
                    "SELECT value FROM pg_temp.{};", view)) == "AC");
  }
  for (const std::string_view view : from_array_three_views) {
    require_dependency(view, "from_array", from_array_three_oid);
    REQUIRE(query_scalar(
                ctx,
                fmt::format(
                    "SELECT value FROM pg_temp.{};", view)) == "AC");
  }
  for (const std::string_view view : aligner_options_views) {
    require_dependency(
        view, "aligner_options", aligner_options_oid);
  }

  const row_t positional_options = query_one(
      ctx, "SELECT * FROM pg_temp.sql_boundary_aligner_options_positional;");
  const row_t named_options = query_one(
      ctx, "SELECT * FROM pg_temp.sql_boundary_aligner_options_named;");
  const row_t mixed_options = query_one(
      ctx, "SELECT * FROM pg_temp.sql_boundary_aligner_options_mixed;");
  REQUIRE(positional_options == named_options);
  REQUIRE(positional_options == mixed_options);
  REQUIRE(positional_options.size() == 36);
  REQUIRE(positional_options[0].value() == "15");
  REQUIRE(positional_options[1].value() == "10");
  REQUIRE(std::ranges::all_of(
      positional_options.begin() + 2,
      positional_options.end(),
      [](const std::optional<std::string> &value) {
        return !value.has_value();
      }));

  const row_t default_options =
      query_one(ctx, "SELECT (public.aligner_options()).*;");
  REQUIRE(default_options.size() == 36);
  REQUIRE(std::ranges::all_of(
      default_options,
      [](const std::optional<std::string> &value) {
        return !value.has_value();
      }));
}

TEST_CASE("generated SQL preserves the extension catalog surface",
          "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();

  const auto extension_member_count =
      [&ctx](std::string_view catalog, std::string_view alias,
             std::string_view predicate) {
        return query_scalar(
            ctx,
            fmt::format(
                "SELECT count(*)::text "
                "FROM pg_catalog.{0} AS {1} "
                "JOIN pg_catalog.pg_depend AS dep "
                "  ON dep.classid = 'pg_catalog.{0}'::regclass "
                " AND dep.objid = {1}.oid "
                " AND dep.deptype = 'e' "
                "JOIN pg_catalog.pg_extension AS ext "
                "  ON ext.oid = dep.refobjid "
                "WHERE ext.extname = 'bioseq' AND ({2});",
                catalog, alias, predicate));
      };

  REQUIRE(extension_member_count("pg_operator", "obj", "true") == "21");
  REQUIRE(extension_member_count("pg_opclass", "obj", "true") == "4");
  REQUIRE(extension_member_count("pg_proc", "obj", "obj.prokind = 'a'") ==
          "16");

  REQUIRE(extension_member_count(
              "pg_proc", "obj",
              "obj.proname ~ "
              "'^(vbpchar(eq|ne|gt|lt|le|ge)|"
              "vint(2|4|8|24|28|42|48|82|84)(eq|ne|gt|lt|le|ge)|"
              "v(date|time|timestamp|timestamptz)_"
              "(eq|ne|gt|lt|le|ge))$'") == "84");

  REQUIRE(extension_member_count(
              "pg_proc", "obj",
              "obj.proname = ANY (ARRAY["
              "'vemptycount','vanycount','vint2int4avg','vint2sum',"
              "'vint2acc','vint2larger','vint2smaller','vint4sum',"
              "'vint4acc','vint4larger','vint4smaller','vint8acc',"
              "'vint8sum','vint8avg','vint8larger','vint8smaller',"
              "'vdatelarger','vdatesmaller'])") == "18");

  REQUIRE(query_scalar(
              ctx,
              "SELECT count(*)::text "
              "FROM pg_catalog.pg_attribute "
              "WHERE attrelid = 'bioseq_aligner_options'::regclass "
              "  AND attnum > 0 AND NOT attisdropped;") == "36");
  const auto aligner_signature = query_one(
      ctx,
      "SELECT p.pronargs::text, p.pronargdefaults::text, "
      "       array_to_string(p.proargnames, ','), "
      "       (SELECT string_agg("
      "                   pg_catalog.format_type(arg_type, NULL), ',' "
      "                   ORDER BY ordinal) "
      "          FROM unnest(p.proargtypes::oid[]) WITH ORDINALITY "
      "               AS args(arg_type, ordinal)), "
      "       regexp_count("
      "           pg_catalog.pg_get_function_arguments(p.oid), "
      "           ' DEFAULT ')::text, "
      "       regexp_count("
      "           pg_catalog.pg_get_function_arguments(p.oid), "
      "           ' DEFAULT NULL')::text "
      "  FROM pg_catalog.pg_proc AS p "
      " WHERE p.pronamespace = 'public'::regnamespace "
      "   AND p.proname = 'aligner_options' "
      "   AND p.prorettype = 'bioseq_aligner_options'::regtype;");
  REQUIRE(aligner_signature.size() == 6);
  REQUIRE(aligner_signature[0].value() == "36");
  REQUIRE(aligner_signature[1].value() == "36");
  REQUIRE(aligner_signature[2].value() == k_aligner_option_names);
  REQUIRE(aligner_signature[3].value() == k_aligner_option_types);
  REQUIRE(aligner_signature[4].value() == "36");
  REQUIRE(aligner_signature[5].value() == "36");

  const auto guc_profile = query_one(
      ctx,
      "SELECT count(*)::text "
      "  FROM pg_catalog.pg_settings "
      " WHERE name LIKE 'bioseq.%';");
  REQUIRE(guc_profile.size() == 1);
  // Both build profiles expose 62 bioseq settings in pg_settings.
  // GUC_NO_SHOW_ALL hides the internal controls, including those added for tests.
  REQUIRE(guc_profile[0].value() == "62");

  const auto profile_guc = query_one(
      ctx,
      "SELECT (current_setting("
      "          'bioseq.testing.persisted_index.failure_mode', true)"
      "        IS NOT NULL)::text, "
      "       current_setting("
      "          'bioseq.testing.persisted_index.failure_mode', true);");
  REQUIRE(profile_guc.size() == 2);
#ifdef BIOSEQ_TEST_INDEXES
  REQUIRE(profile_guc[0].value() == "true");
  REQUIRE(profile_guc[1].value() == "none");
#else
  REQUIRE(profile_guc[0].value() == "false");
  REQUIRE_FALSE(profile_guc[1].has_value());
#endif
}

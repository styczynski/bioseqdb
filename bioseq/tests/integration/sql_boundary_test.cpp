/**
 * @file sql_boundary_test.cpp
 * @brief Tests installed SQL objects, sequence lookup races, and  table options.
 */

#include <libpq-fe.h>

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;
import bioseq.tests.integration.sql_boundary_test_support;

using namespace bioseq_int_tests;
using namespace bioseq_int_tests::sql_boundary_test_support;

TEST_CASE(
    "Installed routines use supported languages and owned symbols",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();

  const auto languages = query_one(
      ctx, "WITH extension_routines AS ("
           "    SELECT p.prokind, l.lanname "
           "      FROM pg_catalog.pg_proc AS p "
           "      JOIN pg_catalog.pg_language AS l ON l.oid = p.prolang "
           "      JOIN pg_catalog.pg_depend AS d "
           "        ON d.classid = 'pg_catalog.pg_proc'::regclass "
           "       AND d.objid = p.oid "
           "       AND d.objsubid = 0 "
           "      JOIN pg_catalog.pg_extension AS e "
           "        ON d.refclassid = 'pg_catalog.pg_extension'::regclass "
           "       AND d.refobjid = e.oid "
           "       AND d.deptype = 'e' "
           "     WHERE e.extname = 'bioseq'"
           ") "
           "SELECT (count(*) FILTER "
           "           (WHERE prokind = 'f' AND lanname = 'c'))::text, "
           "       (count(*) FILTER "
           "           (WHERE prokind = 'f' AND lanname = 'sql'))::text, "
           "       (count(*) FILTER "
           "           (WHERE prokind = 'p' AND lanname = 'c'))::text, "
           "       (count(*) FILTER (WHERE lanname = 'plpgsql'))::text, "
           "       (count(*) FILTER ("
           "           WHERE prokind IN ('f', 'p') "
           "             AND NOT ("
           "                 (prokind = 'f' AND lanname IN ('c', 'sql')) "
           "                 OR (prokind = 'p' AND lanname = 'c'))))::text "
           "  FROM extension_routines;");
  REQUIRE(languages.size() == 5);
  REQUIRE(std::stoll(languages[0].value()) > 0);
  REQUIRE(languages[1].value() == "2");
  REQUIRE(languages[2].value() == "1");
  REQUIRE(languages[3].value() == "0");
  REQUIRE(languages[4].value() == "0");

  const auto symbols = query_one(
      ctx, "WITH c_routines AS ("
           "    SELECT p.proname, p.prosrc "
           "      FROM pg_catalog.pg_proc AS p "
           "      JOIN pg_catalog.pg_language AS l ON l.oid = p.prolang "
           "      JOIN pg_catalog.pg_depend AS d "
           "        ON d.classid = 'pg_catalog.pg_proc'::regclass "
           "       AND d.objid = p.oid "
           "       AND d.objsubid = 0 "
           "      JOIN pg_catalog.pg_extension AS e "
           "        ON d.refclassid = 'pg_catalog.pg_extension'::regclass "
           "       AND d.refobjid = e.oid "
           "       AND d.deptype = 'e' "
           "     WHERE e.extname = 'bioseq' "
           "       AND l.lanname = 'c' "
           "       AND p.prokind IN ('f', 'p')"
           "), shared AS ("
           "    SELECT prosrc, count(*) AS identities "
           "      FROM c_routines "
           "     GROUP BY prosrc "
           "    HAVING count(*) > 1"
           ") "
           "SELECT (SELECT count(*) FROM c_routines)::text, "
           "       (SELECT count(DISTINCT prosrc) FROM c_routines)::text, "
           "       (SELECT count(*) FROM c_routines "
           "         WHERE prosrc <> proname)::text, "
           "       (SELECT string_agg(prosrc || ':' || identities::text, ',' "
           "                          ORDER BY prosrc) "
           "          FROM shared);");
  REQUIRE(symbols.size() == 4);
  REQUIRE(std::stoll(symbols[0].value()) ==
          std::stoll(languages[0].value()) + 1);
  REQUIRE(std::stoll(symbols[1].value()) > 0);
  REQUIRE(std::stoll(symbols[1].value()) <=
          std::stoll(symbols[0].value()));
  REQUIRE(symbols[2].value() == "0");
  REQUIRE(symbols[3].value() == "from_array:2,mmsearch:2");

  const auto bpchar = query_one(
      ctx, "WITH extension_routines AS ("
           "    SELECT p.*, l.lanname "
           "      FROM pg_catalog.pg_proc AS p "
           "      JOIN pg_catalog.pg_language AS l ON l.oid = p.prolang "
           "      JOIN pg_catalog.pg_depend AS d "
           "        ON d.classid = 'pg_catalog.pg_proc'::regclass "
           "       AND d.objid = p.oid "
           "       AND d.objsubid = 0 "
           "      JOIN pg_catalog.pg_extension AS e "
           "        ON d.refclassid = 'pg_catalog.pg_extension'::regclass "
           "       AND d.refobjid = e.oid "
           "       AND d.deptype = 'e' "
           "     WHERE e.extname = 'bioseq'"
           "), target AS ("
           "    SELECT * FROM extension_routines "
           "     WHERE proname = ANY (ARRAY["
           "         'vbpchareq', 'vbpcharne', 'vbpchargt', "
           "         'vbpcharlt', 'vbpcharle', 'vbpcharge']::name[])"
           ") "
           "SELECT (SELECT count(*) FROM target)::text, "
           "       (SELECT count(*) FROM target "
           "         WHERE pronargs = 2 "
           "           AND proargtypes[0] = 'pg_catalog.bpchar'::regtype "
           "           AND proargtypes[1] = 'pg_catalog.bpchar'::regtype "
           "           AND prorettype = 'pg_catalog.bool'::regtype "
           "           AND proisstrict "
           "           AND provolatile = 'i' "
           "           AND lanname = 'c' "
           "           AND prosrc = proname)::text, "
           "       (SELECT count(*) FROM extension_routines "
           "         WHERE proname = ANY (ARRAY["
           "             'vchareq', 'vcharne', 'vchargt', "
           "             'vcharlt', 'vcharle', 'vcharge']::name[]))::text, "
           "       (SELECT string_agg(proname::text, ',' ORDER BY proname) "
           "          FROM target);");
  REQUIRE(bpchar.size() == 4);
  REQUIRE(bpchar[0].value() == "6");
  REQUIRE(bpchar[1].value() == "6");
  REQUIRE(bpchar[2].value() == "0");
  REQUIRE(bpchar[3].value() ==
          "vbpchareq,vbpcharge,vbpchargt,vbpcharle,vbpcharlt,vbpcharne");
}

TEST_CASE("Table-AM catalog dependencies are exact and idempotent",
          "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();

  require_exact_am_dependencies(am_dependency_summary(ctx));
  exec_sql(ctx, k_reapply_am_dependencies);
  exec_sql(ctx, k_reapply_am_dependencies);
  require_exact_am_dependencies(am_dependency_summary(ctx));

  exec_sql(ctx, "BEGIN;");
  exec_sql(ctx, "ALTER EXTENSION bioseq DROP ACCESS METHOD bioseq;");
  exec_sql(
      ctx,
      "ALTER EXTENSION bioseq "
      "DROP SEQUENCE bioseq_internal.storageid_seq;");
  const auto restricted_drop = sql_error(
      ctx, "DROP SEQUENCE bioseq_internal.storageid_seq;");
  REQUIRE(restricted_drop.sqlstate == "2BP01");
  exec_sql(ctx, "ROLLBACK;");
  require_exact_am_dependencies(am_dependency_summary(ctx));

  exec_sql(ctx, "BEGIN;");
  exec_sql(ctx, "ALTER EXTENSION bioseq DROP ACCESS METHOD bioseq;");
  exec_sql(
      ctx,
      "ALTER EXTENSION bioseq "
      "DROP SEQUENCE bioseq_internal.storageid_seq;");
  exec_sql(
      ctx,
      "DROP SEQUENCE bioseq_internal.storageid_seq CASCADE;");
  REQUIRE(query_scalar(
              ctx,
              "SELECT count(*)::text "
              "  FROM pg_catalog.pg_am "
              " WHERE amname = 'bioseq';") == "0");
  REQUIRE(query_scalar(
              ctx,
              "SELECT (to_regclass("
              "    'bioseq_internal.storageid_seq') IS NULL)::text;") ==
          "true");
  REQUIRE(query_scalar(
              ctx,
              "SELECT (to_regclass("
              "    'bioseq_internal.chunk') IS NOT NULL)::text;") ==
          "true");
  exec_sql(ctx, "ROLLBACK;");
  require_exact_am_dependencies(am_dependency_summary(ctx));
}

TEST_CASE("Generated sequence handles bind only to fixed internal objects",
          "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();

  const auto handles = query_one(
      ctx,
      "WITH expected(leaf) AS (VALUES "
      "  ('minimizer_generation_id_seq'::name),"
      "  ('row_mask_seq'::name),"
      "  ('sequence_source_epoch_seq'::name),"
      "  ('storageid_seq'::name)"
      "), resolved AS ("
      "  SELECT e.leaf, n.nspname, c.oid, c.relname, "
      "         c.relkind, c.relpersistence "
      "    FROM expected AS e "
      "    LEFT JOIN pg_catalog.pg_namespace AS n "
      "      ON n.nspname = 'bioseq_internal' "
      "    LEFT JOIN pg_catalog.pg_class AS c "
      "      ON c.relnamespace = n.oid "
      "     AND c.relname = e.leaf"
      ") "
      "SELECT (count(*) FILTER (WHERE oid IS NOT NULL))::text, "
      "       string_agg("
      "         CASE WHEN oid IS NULL THEN '<missing:' || leaf::text || '>' "
      "              ELSE nspname || '.' || relname END,"
      "         ',' ORDER BY leaf), "
      "       (count(*) FILTER ("
      "          WHERE oid IS NULL OR relkind <> 'S' "
      "             OR relpersistence <> 'p'))::text, "
      "       (SELECT count(*)::text "
      "          FROM pg_catalog.pg_class AS misplaced "
      "          JOIN pg_catalog.pg_namespace AS misplaced_ns "
      "            ON misplaced_ns.oid = misplaced.relnamespace "
      "          JOIN expected AS e ON e.leaf = misplaced.relname "
      "         WHERE misplaced_ns.nspname = 'bioseq') "
      "  FROM resolved;");

  REQUIRE(handles.size() == 4);
  REQUIRE(handles[0].value() == "4");
  REQUIRE(handles[1].value() ==
          "bioseq_internal.minimizer_generation_id_seq,"
          "bioseq_internal.row_mask_seq,"
          "bioseq_internal.sequence_source_epoch_seq,"
          "bioseq_internal.storageid_seq");
  REQUIRE(handles[2].value() == "0");
  REQUIRE(handles[3].value() == "0");

  // Call the generated C++ sequence handles through the diagnostic function
  // provided by the indexes-enabled build.
  exec_sql(
      ctx,
      "SELECT bioseq_internal.arm_sequence_source_failure("
      "    'catalog_handle_identities');");
  for (const std::string_view handle :
       {"minimizer_generation_id_seq",
        "row_mask_seq",
        "sequence_source_epoch_seq",
        "storageid_seq"}) {
    const std::string oid = query_scalar(
        ctx,
        fmt::format(
            "SELECT 'bioseq_internal.{}'::regclass::oid::text;",
            handle));
    exec_sql(
        ctx,
        fmt::format(
            "SELECT bioseq_internal.arm_sequence_source_failure("
            "  'catalog_handle_resolve:{}:error:{}');",
            handle,
            oid));
  }

  // Each lookup must see renames and reject a table placed under the sequence
  // name. Roll, back the catalog changes when these checks finish.
  exec_sql(ctx, "BEGIN;");
  const std::string original_oid = query_scalar(
      ctx,
      "SELECT 'bioseq_internal.storageid_seq'::regclass::oid::text;");
  exec_sql(
      ctx,
      "ALTER SEQUENCE bioseq_internal.storageid_seq "
      "RENAME TO storageid_seq_saved;");
  exec_sql(
      ctx,
      "SELECT bioseq_internal.arm_sequence_source_failure("
      "  'catalog_handle_resolve:storageid_seq:return_invalid:0');");

  exec_sql(ctx, "SAVEPOINT missing_handle;");
  const auto missing = sql_error(
      ctx,
      "SELECT bioseq_internal.arm_sequence_source_failure("
      "  'catalog_handle_resolve:storageid_seq:error:0');");
  REQUIRE(missing.sqlstate == "42704");
  exec_sql(ctx, "ROLLBACK TO SAVEPOINT missing_handle;");

  exec_sql(ctx, "CREATE TABLE bioseq_internal.storageid_seq (id integer);");
  for (const std::string_view policy : {"error", "return_invalid"}) {
    exec_sql(ctx, "SAVEPOINT wrong_relkind_handle;");
    const auto wrong_relkind = sql_error(
        ctx,
        fmt::format(
            "SELECT bioseq_internal.arm_sequence_source_failure("
            "  'catalog_handle_resolve:storageid_seq:{}:0');",
            policy));
    REQUIRE(wrong_relkind.sqlstate == "42809");
    exec_sql(ctx, "ROLLBACK TO SAVEPOINT wrong_relkind_handle;");
  }
  exec_sql(ctx, "DROP TABLE bioseq_internal.storageid_seq;");
  exec_sql(
      ctx,
      "ALTER SEQUENCE bioseq_internal.storageid_seq_saved "
      "RENAME TO storageid_seq;");
  exec_sql(
      ctx,
      fmt::format(
          "SELECT bioseq_internal.arm_sequence_source_failure("
          "  'catalog_handle_resolve:storageid_seq:error:{}');",
          original_oid));

  exec_sql(
      ctx,
      "ALTER EXTENSION bioseq "
      "DROP SEQUENCE bioseq_internal.storageid_seq;");
  // The access method still depends on this sequence, so DROP must fail even
  // after the sequence is removed from the extension's member list.
  exec_sql(ctx, "SAVEPOINT extension_owned_dependency;");
  const auto dependent_sequence = sql_error(
      ctx,
      "DROP SEQUENCE bioseq_internal.storageid_seq;");
  REQUIRE(dependent_sequence.sqlstate == "2BP01");
  exec_sql(ctx, "ROLLBACK TO SAVEPOINT extension_owned_dependency;");
  exec_sql(ctx, "ROLLBACK;");
}

TEST_CASE(
    "Sequence handles reject an identity changed during lookup",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();
  connection_test_context gate;
  connection_test_context worker;
  connection_test_context observer;
  constexpr long long advisory_key = 0xed3b81a1LL;

  const std::string original_oid = query_scalar(
      ctx,
      "SELECT 'bioseq_internal.storageid_seq'::regclass::oid::text;");
  const long long gate_pid =
      query_scalar_int(gate, "SELECT pg_backend_pid();");
  const long long worker_pid =
      query_scalar_int(worker, "SELECT pg_backend_pid();");
  exec_sql(worker, "SET statement_timeout = '30s';");
  exec_sql(
      worker,
      fmt::format(
          "SELECT bioseq_internal.arm_sequence_source_failure("
          "  'catalog_handle_barrier:{}');",
          advisory_key));
  exec_sql(
      gate,
      fmt::format(
          "SELECT pg_catalog.pg_advisory_lock({}::bigint);",
          advisory_key));
  const row_t lock_identity = query_one(
      observer,
      fmt::format(
          "SELECT database::text, classid::text, "
          "       objid::text, objsubid::text "
          "  FROM pg_catalog.pg_locks "
          " WHERE pid = {} "
          "   AND locktype = 'advisory' "
          "   AND mode = 'ExclusiveLock' "
          "   AND granted;",
          gate_pid));
  const std::string lock_predicate =
      advisory_lock_predicate(lock_identity);

  scoped_async_query lookup{worker.raw()};
  const bool sent = lookup.send(fmt::format(
      "SELECT bioseq_internal.arm_sequence_source_failure("
      "  'catalog_handle_resolve:storageid_seq:error:{}');",
      original_oid));
  const bool wait_observed =
      sent && wait_for_exact_advisory_lock(
          observer, worker_pid, lock_predicate, false);

  bool renamed = false;
  if (wait_observed) {
    exec_sql(
        gate,
        "ALTER SEQUENCE bioseq_internal.storageid_seq "
        "RENAME TO storageid_seq_race_saved;");
    renamed = true;
  }
  const bool unlocked = query_scalar(
      gate,
      fmt::format(
          "SELECT pg_catalog.pg_advisory_unlock({}::bigint)::text;",
          advisory_key)) == "true";
  const async_query_outcome outcome =
      sent ? lookup.drain() : async_query_outcome{};
  if (renamed) {
    exec_sql(
        gate,
        "ALTER SEQUENCE "
        "bioseq_internal.storageid_seq_race_saved "
        "RENAME TO storageid_seq;");
  }

  REQUIRE(lock_predicate != "false");
  REQUIRE(sent);
  REQUIRE(wait_observed);
  REQUIRE(unlocked);
  REQUIRE(outcome.consume_ok);
  REQUIRE(outcome.ready);
  REQUIRE(outcome.got_result);
  REQUIRE(outcome.status == PGRES_FATAL_ERROR);
  REQUIRE(outcome.sqlstate == "42704");
  REQUIRE(outcome.message.find("disappeared") != std::string::npos);
  exec_sql(
      ctx,
      fmt::format(
          "SELECT bioseq_internal.arm_sequence_source_failure("
          "  'catalog_handle_resolve:storageid_seq:error:{}');",
          original_oid));

  exec_sql(gate, "CREATE SCHEMA wrong_sequence_namespace;");
  for (const auto [policy, advisory_key] :
       std::array<std::pair<std::string_view, long long>, 2>{
           std::pair{"error", 0xed3b81a2LL},
           std::pair{"return_invalid", 0xed3b81a3LL},
       }) {
    exec_sql(
        worker,
        fmt::format(
            "SELECT bioseq_internal.arm_sequence_source_failure("
            "  'catalog_handle_barrier:{}');",
            advisory_key));
    exec_sql(
        gate,
        fmt::format(
            "SELECT pg_catalog.pg_advisory_lock({}::bigint);",
            advisory_key));
    const row_t namespace_lock_identity = query_one(
        observer,
        fmt::format(
            "SELECT database::text, classid::text, "
            "       objid::text, objsubid::text "
            "  FROM pg_catalog.pg_locks "
            " WHERE pid = {} "
            "   AND locktype = 'advisory' "
            "   AND mode = 'ExclusiveLock' "
            "   AND granted;",
            gate_pid));
    const std::string namespace_lock_predicate =
        advisory_lock_predicate(namespace_lock_identity);
    scoped_async_query namespace_lookup{worker.raw()};
    const bool namespace_sent = namespace_lookup.send(fmt::format(
        "SELECT bioseq_internal.arm_sequence_source_failure("
        "  'catalog_handle_resolve:storageid_seq:{}:{}');",
        policy,
        original_oid));
    const bool namespace_wait_observed =
        namespace_sent && wait_for_exact_advisory_lock(
            observer,
            worker_pid,
            namespace_lock_predicate,
            false);
    bool moved = false;
    if (namespace_wait_observed) {
      exec_sql(
          gate,
          "ALTER SEQUENCE bioseq_internal.storageid_seq "
          "SET SCHEMA wrong_sequence_namespace;");
      moved = true;
    }
    const bool namespace_unlocked = query_scalar(
        gate,
        fmt::format(
            "SELECT pg_catalog.pg_advisory_unlock({}::bigint)::text;",
            advisory_key)) == "true";
    const async_query_outcome namespace_outcome =
        namespace_sent
            ? namespace_lookup.drain()
            : async_query_outcome{};
    if (moved) {
      exec_sql(
          gate,
          "ALTER SEQUENCE "
          "wrong_sequence_namespace.storageid_seq "
          "SET SCHEMA bioseq_internal;");
    }

    REQUIRE(namespace_lock_predicate != "false");
    REQUIRE(namespace_sent);
    REQUIRE(namespace_wait_observed);
    REQUIRE(namespace_unlocked);
    REQUIRE(namespace_outcome.consume_ok);
    REQUIRE(namespace_outcome.ready);
    REQUIRE(namespace_outcome.got_result);
    REQUIRE(namespace_outcome.status == PGRES_FATAL_ERROR);
    REQUIRE(namespace_outcome.sqlstate == "42809");
  }
  exec_sql(gate, "DROP SCHEMA wrong_sequence_namespace;");

  // Rename the extension's sequence temporarily to preserve its state and
  // membership. Drop a disposable  sequence with the original name while
  // lookup is paused, then restore the original name.
  constexpr long long drop_advisory_key = 0xed3b81a4LL;
  exec_sql(
      gate,
      "ALTER SEQUENCE bioseq_internal.storageid_seq "
      "RENAME TO storageid_seq_drop_saved;");
  exec_sql(
      gate,
      "CREATE SEQUENCE bioseq_internal.storageid_seq;");
  const std::string disposable_oid = query_scalar(
      gate,
      "SELECT 'bioseq_internal.storageid_seq'::regclass::oid::text;");
  exec_sql(
      worker,
      fmt::format(
          "SELECT bioseq_internal.arm_sequence_source_failure("
          "  'catalog_handle_barrier:{}');",
          drop_advisory_key));
  exec_sql(
      gate,
      fmt::format(
          "SELECT pg_catalog.pg_advisory_lock({}::bigint);",
          drop_advisory_key));
  const row_t drop_lock_identity = query_one(
      observer,
      fmt::format(
          "SELECT database::text, classid::text, "
          "       objid::text, objsubid::text "
          "  FROM pg_catalog.pg_locks "
          " WHERE pid = {} "
          "   AND locktype = 'advisory' "
          "   AND mode = 'ExclusiveLock' "
          "   AND granted;",
          gate_pid));
  const std::string drop_lock_predicate =
      advisory_lock_predicate(drop_lock_identity);
  scoped_async_query drop_lookup{worker.raw()};
  const bool drop_sent = drop_lookup.send(
      "SELECT bioseq_internal.arm_sequence_source_failure("
      "  'catalog_handle_resolve:storageid_seq:return_invalid:0');");
  const bool drop_wait_observed =
      drop_sent && wait_for_exact_advisory_lock(
          observer,
          worker_pid,
          drop_lock_predicate,
          false);
  bool disposable_dropped = false;
  if (drop_wait_observed) {
    exec_sql(
        gate,
        "DROP SEQUENCE bioseq_internal.storageid_seq;");
    disposable_dropped = true;
  }
  const bool drop_unlocked = query_scalar(
      gate,
      fmt::format(
          "SELECT pg_catalog.pg_advisory_unlock({}::bigint)::text;",
          drop_advisory_key)) == "true";
  const async_query_outcome drop_outcome =
      drop_sent
          ? drop_lookup.drain()
          : async_query_outcome{};
  if (!disposable_dropped) {
    exec_sql(
        gate,
        "DROP SEQUENCE bioseq_internal.storageid_seq;");
  }
  exec_sql(
      gate,
      "ALTER SEQUENCE "
      "bioseq_internal.storageid_seq_drop_saved "
      "RENAME TO storageid_seq;");

  REQUIRE(disposable_oid != original_oid);
  REQUIRE(drop_lock_predicate != "false");
  REQUIRE(drop_sent);
  REQUIRE(drop_wait_observed);
  REQUIRE(drop_unlocked);
  REQUIRE(drop_outcome.consume_ok);
  REQUIRE(drop_outcome.ready);
  REQUIRE(drop_outcome.got_result);
  REQUIRE(drop_outcome.status == PGRES_TUPLES_OK);
  exec_sql(
      ctx,
      fmt::format(
          "SELECT bioseq_internal.arm_sequence_source_failure("
          "  'catalog_handle_resolve:storageid_seq:error:{}');",
          original_oid));
}

TEST_CASE("Native table-option functions set and reset catalog state",
          "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context({k_alter_table});
  seed_small_bioseq_column(ctx, k_alter_table);

  const row_t initial = table_options(ctx, k_alter_table);
  REQUIRE(initial.size() == 4);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_set("
            "    '{}'::regclass, 1000, 2000, 'none'::name, 1);",
            k_alter_table);
  const row_t modified = table_options(ctx, k_alter_table);
  REQUIRE(modified.size() == 4);
  REQUIRE(modified[0].value() == "1000");
  REQUIRE(modified[1].value() == "2000");
  REQUIRE(modified[2].value() == "none");
  REQUIRE(modified[3].value() == "1");
  REQUIRE(modified != initial);
  REQUIRE(query_scalar_int(
              ctx, fmt::format("SELECT count(*) FROM {};", k_alter_table)) == 3);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_reset("
            "    '{}'::regclass, true, true, true, true);",
            k_alter_table);
  REQUIRE(table_options(ctx, k_alter_table) == initial);
}

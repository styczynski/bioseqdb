/**
 * @file sql_boundary_maintenance_test.cpp
 * @brief Tests metadata ages, shard cleanup, and races while removing orphan tables.
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
    "list_metadata owns non-NULL intervals after SPI cleanup",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context({k_metadata_interval_table});
  exec_sqlf(
      ctx,
      "CREATE TABLE {} ("
      "    pk integer PRIMARY KEY,"
      "    seq0 bioseq.seq('dna4', 32)"
      ") USING bioseq;",
      k_metadata_interval_table);
  exec_sqlf(
      ctx,
      "INSERT INTO bioseq_internal.metadata_age_log ("
      "    user_relid, main_relfilenode, column_attno, "
      "    index_name, last_updated) "
      "SELECT c.oid, pg_catalog.pg_relation_filenode(c.oid), "
      "       a.attnum, 'symbol_counts', "
      "       clock_timestamp() - INTERVAL '2 days' "
      "  FROM pg_catalog.pg_class AS c "
      "  JOIN pg_catalog.pg_attribute AS a "
      "    ON a.attrelid = c.oid "
      "   AND a.attname = 'seq0' "
      " WHERE c.oid = '{}'::regclass "
      "ON CONFLICT ("
      "    user_relid, main_relfilenode, column_attno, index_name) "
      "DO UPDATE SET last_updated = EXCLUDED.last_updated;",
      k_metadata_interval_table);

  const rowset_t metadata_intervals = query_rows(
      ctx,
      fmt::format(
          "SELECT index_name, (time_since_update IS NULL)::text, "
          "       floor(extract(epoch FROM time_since_update))::bigint::text "
          "  FROM bioseq.list_metadata() "
          " WHERE table_name = '{}'::regclass "
          "   AND index_name IN ('minimizers_per_fragment', 'symbol_counts') "
          " ORDER BY index_name;",
          k_metadata_interval_table));
  REQUIRE(metadata_intervals.size() == 2);
  REQUIRE(metadata_intervals[0] ==
          row_t{std::string{"minimizers_per_fragment"},
                std::string{"true"},
                std::nullopt});
  REQUIRE(metadata_intervals[1].size() == 3);
  REQUIRE(metadata_intervals[1][0].value() == "symbol_counts");
  REQUIRE(metadata_intervals[1][1].value() == "false");
  REQUIRE(metadata_intervals[1][2].has_value());
  const long long elapsed_seconds = std::stoll(*metadata_intervals[1][2]);
  REQUIRE(elapsed_seconds >= 2 * 24 * 60 * 60);
  REQUIRE(elapsed_seconds < 2 * 24 * 60 * 60 + 60);
}

TEST_CASE(
    "Metadata interval mutations release every operation context",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();
  const auto operation_contexts = [&ctx] {
    return query_scalar_int(
        ctx,
        "SELECT count(*) "
        "  FROM pg_catalog.pg_backend_memory_contexts "
        " WHERE name IN ("
        "   'metadata_age_log upsert operation',"
        "   'metadata_age_log delete operation');");
  };
  REQUIRE(operation_contexts() == 0);
  exec_sql(
      ctx,
      "SELECT bioseq_internal.arm_sequence_source_failure("
      "  'metadata_age_log_context_stress');");
  REQUIRE(operation_contexts() == 0);
  REQUIRE(query_scalar_int(
              ctx,
              "SELECT count(*) "
              "  FROM bioseq_internal.metadata_age_log "
              " WHERE user_relid = 710079::oid "
              "   AND main_relfilenode = 710079::oid;")
          == 0);
  REQUIRE(query_scalar(ctx, "SELECT 710079::text;") == "710079");
}

TEST_CASE(
    "Generated index-table-shard codec executes an isolated lifecycle",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();
  const long long rows_before = query_scalar_int(
      ctx,
      "SELECT count(*) FROM bioseq_internal.index_table_shard;");
  exec_sql(
      ctx,
      "SELECT bioseq_internal.arm_sequence_source_failure("
      "    'row_codec_index_table_shard');");
  REQUIRE(query_scalar_int(
              ctx,
              "SELECT count(*) "
              "  FROM bioseq_internal.index_table_shard;")
          == rows_before);
}

TEST_CASE(
    "Native shard vacuum reaches both relations and preserves data",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context({k_vacuum_table});
  seed_small_bioseq_column(ctx, k_vacuum_table);

  const long long shard_count = query_scalar_int(
      ctx, fmt::format("SELECT count(*) "
                       "  FROM bioseq.list_seq_storage_shards('{}'::regclass);",
                       k_vacuum_table));
  REQUIRE(shard_count > 0);

  exec_sqlf(ctx, "CALL bioseq.vacuum_seq_storage('{}'::regclass);",
            k_vacuum_table);

  const auto vacuumed = query_one(
      ctx,
      fmt::format("SELECT count(*)::text, "
                  "       (count(*) FILTER (WHERE "
                  "           pg_stat_get_last_vacuum_time("
                  "               sub_relation::oid) IS NOT NULL))::text, "
                  "       (count(*) FILTER (WHERE "
                  "           pg_stat_get_last_vacuum_time("
                  "               sidecar_relation::oid) IS NOT NULL))::text "
                  "  FROM bioseq.list_seq_storage_shards('{}'::regclass);",
                  k_vacuum_table));
  REQUIRE(vacuumed.size() == 3);
  REQUIRE(std::stoll(vacuumed[0].value()) == shard_count);
  REQUIRE(std::stoll(vacuumed[1].value()) == shard_count);
  REQUIRE(std::stoll(vacuumed[2].value()) == shard_count);
  REQUIRE(query_scalar_int(
              ctx, fmt::format("SELECT count(*) FROM {};", k_vacuum_table)) == 3);
  REQUIRE(query_scalar(
              ctx,
              "SELECT (NOT EXISTS ("
              "    SELECT 1 "
              "      FROM pg_catalog.pg_class "
              "     WHERE relnamespace = pg_my_temp_schema() "
              "       AND relname = '_bioseq_shards_to_vacuum'))::text;") ==
          "true");

  exec_sql(ctx,
           "BEGIN; "
           "CALL bioseq.vacuum_seq_storage(NULL::regclass); "
           "COMMIT;");

  const auto atomic_call = sql_error(
      ctx,
      fmt::format("BEGIN; CALL bioseq.vacuum_seq_storage('{}'::regclass);",
                  k_vacuum_table));
  REQUIRE(atomic_call.sqlstate == "2D000");
  exec_sql(ctx, "ROLLBACK;");
  REQUIRE(query_scalar_int(
              ctx, fmt::format("SELECT count(*) FROM {};", k_vacuum_table)) == 3);

  const std::string original_sidecar = query_scalar(
      ctx,
      fmt::format(
          "SELECT sidecar_relid::text "
          "FROM bioseq_internal.seq_sub_table "
          "WHERE main_relfilenode = "
          "      pg_catalog.pg_relation_filenode('{}'::regclass) "
          "ORDER BY column_attno, bucket_idx, shard_idx LIMIT 1;",
          k_vacuum_table));
  exec_sqlf(
      ctx,
      "UPDATE bioseq_internal.seq_sub_table "
      "SET sidecar_relid = '4294967294'::oid "
      "WHERE main_relfilenode = "
      "      pg_catalog.pg_relation_filenode('{}'::regclass) "
      "  AND sidecar_relid = {}::oid;",
      k_vacuum_table, original_sidecar);
  exec_sqlf(ctx, "CALL bioseq.vacuum_seq_storage('{}'::regclass);",
            k_vacuum_table);
  REQUIRE(query_scalar(ctx, "SELECT 1::text;") == "1");
  REQUIRE(query_scalar_int(
              ctx, fmt::format("SELECT count(*) FROM {};", k_vacuum_table)) == 3);
  exec_sqlf(
      ctx,
      "UPDATE bioseq_internal.seq_sub_table "
      "SET sidecar_relid = {}::oid "
      "WHERE main_relfilenode = "
      "      pg_catalog.pg_relation_filenode('{}'::regclass) "
      "  AND sidecar_relid = '4294967294'::oid;",
      original_sidecar, k_vacuum_table);
}

TEST_CASE(
    "Empty and NULL shard maintenance are exact no-ops",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context({k_empty_vacuum_table});
  exec_sqlf(
      ctx,
      "CREATE TABLE {} ("
      "  id integer PRIMARY KEY,"
      "  seq bioseq.seq('dna4', 32)"
      ") USING bioseq;",
      k_empty_vacuum_table);
  REQUIRE(query_scalar_int(
              ctx,
              fmt::format(
                  "SELECT count(*) "
                  "  FROM bioseq.list_seq_storage_shards("
                  "    '{}'::regclass);",
                  k_empty_vacuum_table)) == 0);
  REQUIRE(query_scalar_int(
              ctx,
              "SELECT count(*) "
              "  FROM bioseq.list_seq_storage_shards(NULL::regclass);")
          == 0);
  exec_sqlf(
      ctx,
      "CALL bioseq.vacuum_seq_storage('{}'::regclass);",
      k_empty_vacuum_table);
  exec_sql(
      ctx,
      "CALL bioseq.vacuum_seq_storage(NULL::regclass);");
  REQUIRE(query_scalar_int(
              ctx,
              fmt::format("SELECT count(*) FROM {};", k_empty_vacuum_table))
          == 0);
}

TEST_CASE(
    "Native orphan reaper has a stable row shape and is idempotent",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();

  const auto shape = query_one(
      ctx, "SELECT p.pronargs::text, p.proretset::int::text, "
           "       (p.prorettype = 'pg_catalog.record'::regtype)::int::text, "
           "       array_to_string(p.proargnames, ','), "
           "       array_to_string(p.proargmodes, ','), "
           "       (SELECT string_agg("
           "                   pg_catalog.format_type(arg_oid, NULL), ',' "
           "                   ORDER BY ordinal) "
           "          FROM unnest(p.proallargtypes) WITH ORDINALITY "
           "               AS args(arg_oid, ordinal)), "
           "       l.lanname, p.prosrc "
           "  FROM pg_catalog.pg_proc AS p "
           "  JOIN pg_catalog.pg_language AS l ON l.oid = p.prolang "
           " WHERE p.oid = "
           "       'bioseq.reap_orphan_seq_sub_tables()'::regprocedure;");
  REQUIRE(shape.size() == 8);
  REQUIRE(shape[0].value() == "0");
  REQUIRE(shape[1].value() == "1");
  REQUIRE(shape[2].value() == "1");
  REQUIRE(shape[3].value() == "action,target");
  REQUIRE(shape[4].value() == "t,t");
  REQUIRE(shape[5].value() == "text,text");
  REQUIRE(shape[6].value() == "c");
  REQUIRE(shape[7].value() == "reap_orphan_seq_sub_tables");

  exec_sqlf(ctx, "CREATE TABLE {} (id integer);", k_orphan_table);
  const auto first =
      query_rows(ctx, "SELECT action, target "
                      "  FROM bioseq.reap_orphan_seq_sub_tables() "
                      " ORDER BY action, target;");
  const auto second =
      query_rows(ctx, "SELECT action, target "
                      "  FROM bioseq.reap_orphan_seq_sub_tables() "
                      " ORDER BY action, target;");
  REQUIRE(first.size() == 1);
  REQUIRE(first[0].size() == 2);
  REQUIRE(first[0][0].value() == "drop-relation");
  REQUIRE(first[0][1].value() == k_orphan_table);
  REQUIRE(second.empty());
  REQUIRE(query_scalar(
              ctx,
              "SELECT (to_regclass("
              "    'bioseq_internal.seq_sub_table') IS NOT NULL)::text;") ==
          "true");
  REQUIRE(
      query_scalar(ctx, fmt::format("SELECT (to_regclass('{}') IS NULL)::text;",
                                    k_orphan_table)) == "true");

  exec_sqlf(ctx, "CREATE VIEW {} AS SELECT 1 AS id;", k_orphan_view);
  exec_sqlf(ctx, "CREATE VIEW {} AS SELECT 1 AS id;", k_orphan_sidecar_view);
  const auto non_table =
      query_rows(ctx, "SELECT action, target "
                      "  FROM bioseq.reap_orphan_seq_sub_tables() "
                      " ORDER BY target;");
  REQUIRE(non_table.size() == 2);
  REQUIRE(non_table[0].size() == 2);
  REQUIRE(non_table[0][0].value().starts_with("drop-relation-failed:"));
  REQUIRE(non_table[0][1].value() == k_orphan_sidecar_view);
  REQUIRE(non_table[1].size() == 2);
  REQUIRE(non_table[1][0].value().starts_with("drop-relation-failed:"));
  REQUIRE(non_table[1][1].value() == k_orphan_view);
  REQUIRE(query_scalar(
              ctx,
              "SELECT (to_regclass("
              "    'bioseq_internal.seq_sub_table') IS NOT NULL)::text;") ==
          "true");
  REQUIRE(
      query_scalar(ctx, fmt::format("SELECT (to_regclass('{}') IS NOT NULL)::text;",
                                    k_orphan_view)) == "true");
  REQUIRE(query_scalar(
              ctx,
              fmt::format(
                  "SELECT (to_regclass('{}') IS NOT NULL)::text;",
                  k_orphan_sidecar_view)) == "true");
  exec_sqlf(ctx, "DROP VIEW {};", k_orphan_view);
  exec_sqlf(ctx, "DROP VIEW {};", k_orphan_sidecar_view);
}

TEST_CASE(
    "Orphan reaper covers stale catalog rows and transaction outcomes",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context({k_reaper_missing_relation_table});
  seed_small_bioseq_column(ctx, k_reaper_missing_relation_table);

  // Point a catalog row at a missing table and check that the reaper reports
  // its full key. Rolling back must  restore the row and leave its table intact.
  exec_sql(ctx, "BEGIN;");
  const row_t stale_key = query_one(
      ctx,
      fmt::format(
          "SELECT database_oid::text, main_relfilenode::text, "
          "       column_attno::text, bucket_idx::text, shard_idx::text, "
          "       sub_table_relid::text "
          "  FROM bioseq_internal.seq_sub_table "
          " WHERE main_relfilenode = "
          "       pg_catalog.pg_relation_filenode('{}'::regclass) "
          " ORDER BY column_attno, bucket_idx, shard_idx "
          " LIMIT 1;",
          k_reaper_missing_relation_table));
  REQUIRE(stale_key.size() == 6);
  const std::string expected_key = fmt::format(
      "{}/{}/{}/{}/{}",
      stale_key[0].value(),
      stale_key[1].value(),
      stale_key[2].value(),
      stale_key[3].value(),
      stale_key[4].value());
  exec_sql(
      ctx,
      fmt::format(
          "UPDATE bioseq_internal.seq_sub_table "
          "   SET sub_table_relid = '4294967294'::oid "
          " WHERE database_oid = {}::oid "
          "   AND main_relfilenode = {}::oid "
          "   AND column_attno = {}::smallint "
          "   AND bucket_idx = {}::smallint "
          "   AND shard_idx = {}::smallint;",
          stale_key[0].value(),
          stale_key[1].value(),
          stale_key[2].value(),
          stale_key[3].value(),
          stale_key[4].value()));
  const rowset_t missing_relation_events = query_rows(
      ctx,
      "SELECT action, target "
      "  FROM bioseq.reap_orphan_seq_sub_tables() "
      " ORDER BY action, target;");
  REQUIRE(std::ranges::any_of(
      missing_relation_events,
      [&expected_key](const row_t& event) {
        return event.size() == 2
            && event[0] == std::optional<std::string>{
                "delete-catalog-row"}
            && event[1] == std::optional<std::string>{expected_key};
      }));
  exec_sql(ctx, "ROLLBACK;");
  REQUIRE(query_scalar(
              ctx,
              fmt::format(
                  "SELECT EXISTS ("
                  "  SELECT 1 FROM pg_catalog.pg_class "
                  "   WHERE oid = {}::oid)::text;",
                  stale_key[5].value())) == "true");
  REQUIRE(query_scalar_int(
              ctx,
              fmt::format(
                  "SELECT count(*) "
                  "  FROM bioseq_internal.seq_sub_table "
                  " WHERE database_oid = {}::oid "
                  "   AND main_relfilenode = {}::oid "
                  "   AND column_attno = {}::smallint "
                  "   AND bucket_idx = {}::smallint "
                  "   AND shard_idx = {}::smallint;",
                  stale_key[0].value(),
                  stale_key[1].value(),
                  stale_key[2].value(),
                  stale_key[3].value(),
                  stale_key[4].value())) == 1);

  // Removing an orphan table belongs to the caller's transaction.
  // Rollback restores the table, while commit keeps it removed
  exec_sqlf(ctx, "CREATE TABLE {} (id integer);", k_transactional_orphan_table);
  exec_sql(ctx, "BEGIN;");
  REQUIRE(
      query_rows(
          ctx,
          "SELECT action, target "
          "  FROM bioseq.reap_orphan_seq_sub_tables() "
          " WHERE target = "
          "       'bioseq_internal.seq_sub_transactional_orphan_contract';")
      == rowset_t{{
          std::string{"drop-relation"},
          std::string{k_transactional_orphan_table},
      }});
  REQUIRE(query_scalar(
              ctx,
              "SELECT (to_regclass("
              "  'bioseq_internal.seq_sub_transactional_orphan_contract') "
              "  IS NULL)::text;") == "true");
  exec_sql(ctx, "ROLLBACK;");
  REQUIRE(query_scalar(
              ctx,
              "SELECT (to_regclass("
              "  'bioseq_internal.seq_sub_transactional_orphan_contract') "
              "  IS NOT NULL)::text;") == "true");

  exec_sql(ctx, "BEGIN;");
  REQUIRE(query_rows(
              ctx,
              "SELECT action, target "
              "  FROM bioseq.reap_orphan_seq_sub_tables() "
              " WHERE target = "
              "       'bioseq_internal.seq_sub_transactional_orphan_contract';")
          .size() == 1);
  exec_sql(ctx, "COMMIT;");
  REQUIRE(query_scalar(
              ctx,
              "SELECT (to_regclass("
              "  'bioseq_internal.seq_sub_transactional_orphan_contract') "
              "  IS NULL)::text;") == "true");
  REQUIRE(query_rows(
              ctx,
              "SELECT action, target "
              "  FROM bioseq.reap_orphan_seq_sub_tables() "
              " WHERE target = "
              "       'bioseq_internal.seq_sub_transactional_orphan_contract';")
          .empty());
}

TEST_CASE(
    "Orphan reaper preserves READY sequence-source rows",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();
  constexpr long long source_epoch = 0xe74fd9d1LL;
  exec_sql(
      ctx,
      fmt::format(
          "DELETE FROM bioseq_internal.sequence_source_version "
          " WHERE source_epoch = {}::bigint;",
          source_epoch));
  exec_sql(
      ctx,
      fmt::format(
          "INSERT INTO bioseq_internal.sequence_source_version ("
          " source_epoch, user_relid, database_oid, tablespace_oid,"
          " main_relfilenode, main_storage_id, column_attno,"
          " locator_version, bucket_idx, shard_idx, fragment_relid,"
          " fragment_storage_id, first_sub_row_no, num_fragments,"
          " total_bp_size, alphabet_code, storage_unit_byte_width,"
          " padding_letters, encoding, cigar_space, encoded_payload_bytes,"
          " source_content_checksum, state"
          ") VALUES ("
          " {}::bigint, 710077::oid,"
          " (SELECT oid FROM pg_catalog.pg_database"
          "   WHERE datname = current_database()),"
          " (SELECT oid FROM pg_catalog.pg_tablespace"
          "   WHERE spcname = 'pg_default'), 710077::oid,"
          " 1::bigint, 1::smallint, 4::smallint, 0::smallint,"
          " 0::integer, '4294967294'::oid, 710077::bigint, 0::bigint,"
          " 1::bigint, 1::bigint, 1::smallint, 1::smallint,"
          " 0::integer, 0::smallint, 0::smallint, 0::bigint,"
          " 710077::bigint, 1::smallint);",
          source_epoch));
  const row_t before = query_one(
      ctx,
      fmt::format(
          "SELECT pg_catalog.to_jsonb(s)::text,"
          "       pg_catalog.encode("
          "         pg_catalog.record_send(s), 'hex')"
          "  FROM bioseq_internal.sequence_source_version AS s"
          " WHERE source_epoch = {}::bigint;",
          source_epoch));
  REQUIRE(before.size() == 2);
  static_cast<void>(query_rows(
      ctx,
      "SELECT action, target "
      "  FROM bioseq.reap_orphan_seq_sub_tables();"));
  const row_t after = query_one(
      ctx,
      fmt::format(
          "SELECT pg_catalog.to_jsonb(s)::text,"
          "       pg_catalog.encode("
          "         pg_catalog.record_send(s), 'hex')"
          "  FROM bioseq_internal.sequence_source_version AS s"
          " WHERE source_epoch = {}::bigint;",
          source_epoch));
  const std::string state_and_fragment = query_scalar(
      ctx,
      fmt::format(
          "SELECT state::text || '/' || fragment_relid::text"
          "  FROM bioseq_internal.sequence_source_version"
          " WHERE source_epoch = {}::bigint;",
          source_epoch));
  exec_sql(
      ctx,
      fmt::format(
          "DELETE FROM bioseq_internal.sequence_source_version "
          " WHERE source_epoch = {}::bigint;",
          source_epoch));
  REQUIRE(after == before);
  REQUIRE(state_and_fragment == "1/4294967294");
}

TEST_CASE(
    "Orphan reaper never drops a same-name replacement",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();
  exec_sqlf(ctx, "DROP TABLE IF EXISTS {};", k_reaper_race_table);
  // Clear existing orphans before arming the barrir, so it pauses the reaper
  // on the table created below.
  static_cast<void>(query_rows(
      ctx,
      "SELECT action, target "
      "  FROM bioseq.reap_orphan_seq_sub_tables();"));
  exec_sqlf(ctx, "CREATE TABLE {} (generation integer);", k_reaper_race_table);
  const std::string original_oid = query_scalar(
      ctx,
      fmt::format("SELECT '{}'::regclass::oid::text;", k_reaper_race_table));

  connection_test_context gate;
  connection_test_context worker;
  connection_test_context observer;
  constexpr long long advisory_key = 0xed3d0841LL;
  const long long gate_pid =
      query_scalar_int(gate, "SELECT pg_backend_pid();");
  const long long worker_pid =
      query_scalar_int(worker, "SELECT pg_backend_pid();");
  exec_sql(worker, "SET statement_timeout = '30s';");
  exec_sql(
      worker,
      fmt::format(
          "SELECT bioseq_internal.arm_sequence_source_failure("
          "  'orphan_reaper_barrier:{}');",
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

  scoped_async_query reap{worker.raw()};
  const bool sent = reap.send(fmt::format(
      "SELECT action, target "
      "  FROM bioseq.reap_orphan_seq_sub_tables() "
      " WHERE target = '{}';",
      k_reaper_race_table));
  const bool wait_observed =
      sent && wait_for_exact_advisory_lock(
          observer, worker_pid, lock_predicate, false);

  std::string replacement_oid;
  if (wait_observed) {
    exec_sqlf(gate, "DROP TABLE {};", k_reaper_race_table);
    exec_sqlf(
        gate,
        "CREATE TABLE {} (generation integer);",
        k_reaper_race_table);
    replacement_oid = query_scalar(
        gate,
        fmt::format(
            "SELECT '{}'::regclass::oid::text;",
            k_reaper_race_table));
  }
  const bool unlocked = query_scalar(
      gate,
      fmt::format(
          "SELECT pg_catalog.pg_advisory_unlock({}::bigint)::text;",
          advisory_key)) == "true";
  const async_query_outcome outcome =
      sent ? reap.drain() : async_query_outcome{};
  const bool replacement_survived =
      !replacement_oid.empty()
      && query_scalar(
             gate,
             fmt::format(
                 "SELECT (to_regclass('{}')::oid = {}::oid)::text;",
                 k_reaper_race_table,
                 replacement_oid)) == "true";
  exec_sqlf(gate, "DROP TABLE IF EXISTS {};", k_reaper_race_table);

  REQUIRE(lock_predicate != "false");
  REQUIRE(sent);
  REQUIRE(wait_observed);
  REQUIRE(replacement_oid != original_oid);
  REQUIRE(unlocked);
  REQUIRE(outcome.consume_ok);
  REQUIRE(outcome.ready);
  REQUIRE(outcome.got_result);
  REQUIRE(outcome.status == PGRES_TUPLES_OK);
  REQUIRE(outcome.rows.empty());
  REQUIRE(replacement_survived);
}

TEST_CASE(
    "Orphan reaper serializes catalog mutations for missing relations",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context({k_reaper_catalog_race_guard});
  exec_sqlf(
      ctx,
      "CREATE TABLE {} (generation integer);",
      k_reaper_catalog_race_guard);
  const std::string guard_oid = query_scalar(
      ctx,
      fmt::format(
          "SELECT '{}'::regclass::oid::text;",
          k_reaper_catalog_race_guard));
  const std::string database_oid = query_scalar(
      ctx,
      "SELECT oid::text FROM pg_catalog.pg_database "
      " WHERE datname = current_database();");

  const auto run_race =
      [&ctx, &database_oid, &guard_oid](bool insert_race, unsigned key_suffix) {
        const unsigned main_relfilenode =
            710080U + key_suffix;
        const int shard_idx =
            710080 + static_cast<int>(key_suffix);
        exec_sql(
            ctx,
            fmt::format(
                "DELETE FROM bioseq_internal.seq_sub_table "
                " WHERE main_relfilenode = {}::oid;",
                main_relfilenode));
        if (!insert_race) {
          exec_sql(
              ctx,
              fmt::format(
                  "INSERT INTO bioseq_internal.seq_sub_table ("
                  " database_oid, tablespace_oid, main_relfilenode,"
                  " column_attno, bucket_idx, shard_idx,"
                  " sub_table_relid, sidecar_relid,"
                  " bytes_written, is_full, total_payload_bytes"
                  ") VALUES ("
                  " {}::oid, (SELECT oid FROM pg_catalog.pg_tablespace"
                  "   WHERE spcname = 'pg_default'),"
                  " {}::oid, 1::smallint, 0::smallint, {}::integer,"
                  " {}::oid, {}::oid, 0::bigint, false, 0::bigint);",
                  database_oid,
                  main_relfilenode,
                  shard_idx,
                  guard_oid,
                  guard_oid));
        }

        connection_test_context writer;
        connection_test_context worker;
        connection_test_context observer;
        const long long worker_pid =
            query_scalar_int(worker, "SELECT pg_backend_pid();");
        exec_sql(worker, "SET statement_timeout = '30s';");
        exec_sql(writer, "BEGIN;");
        if (insert_race) {
          exec_sql(
              writer,
              fmt::format(
                  "INSERT INTO bioseq_internal.seq_sub_table ("
                  " database_oid, tablespace_oid, main_relfilenode,"
                  " column_attno, bucket_idx, shard_idx,"
                  " sub_table_relid, sidecar_relid,"
                  " bytes_written, is_full, total_payload_bytes"
                  ") VALUES ("
                  " {}::oid, (SELECT oid FROM pg_catalog.pg_tablespace"
                  "   WHERE spcname = 'pg_default'),"
                  " {}::oid, 1::smallint, 0::smallint, {}::integer,"
                  " '4294967294'::oid, '4294967294'::oid,"
                  " 0::bigint, false, 0::bigint);",
                  database_oid,
                  main_relfilenode,
                  shard_idx));
        } else {
          exec_sql(
              writer,
              fmt::format(
                  "UPDATE bioseq_internal.seq_sub_table "
                  "   SET sub_table_relid = '4294967294'::oid "
                  " WHERE main_relfilenode = {}::oid;",
                  main_relfilenode));
        }

        scoped_async_query reap{worker.raw()};
        const bool sent = reap.send(
            "SELECT action, target "
            "  FROM bioseq.reap_orphan_seq_sub_tables() "
            " ORDER BY action, target;");
        const bool wait_observed =
            sent && wait_for_relation_lock(
                observer,
                worker_pid,
                "bioseq_internal.seq_sub_table",
                "ShareRowExclusiveLock",
                false);
        exec_sql(writer, "COMMIT;");
        const async_query_outcome outcome =
            sent ? reap.drain() : async_query_outcome{};
        const std::string expected_target = fmt::format(
            "{}/{}/1/0/{}",
            database_oid,
            main_relfilenode,
            shard_idx);
        const bool exact_delete_observed =
            std::ranges::any_of(
                outcome.rows,
                [&expected_target](const row_t &event) {
                  return event.size() == 2
                      && event[0]
                          == std::optional<std::string>{
                              "delete-catalog-row"}
                      && event[1]
                          == std::optional<std::string>{
                              expected_target};
                });
        const long long remaining_rows = query_scalar_int(
            ctx,
            fmt::format(
                "SELECT count(*) "
                "  FROM bioseq_internal.seq_sub_table "
                " WHERE main_relfilenode = {}::oid;",
                main_relfilenode));
        exec_sql(
            ctx,
            fmt::format(
                "DELETE FROM bioseq_internal.seq_sub_table "
                " WHERE main_relfilenode = {}::oid;",
                main_relfilenode));

        REQUIRE(sent);
        REQUIRE(wait_observed);
        REQUIRE(outcome.consume_ok);
        REQUIRE(outcome.ready);
        REQUIRE(outcome.got_result);
        REQUIRE(outcome.status == PGRES_TUPLES_OK);
        REQUIRE(exact_delete_observed);
        REQUIRE(remaining_rows == 0);
      };

  run_race(true, 0);
  run_race(false, 1);
  const bool guard_survived = query_scalar(
      ctx,
      fmt::format(
          "SELECT (to_regclass('{}') IS NOT NULL)::text;",
          k_reaper_catalog_race_guard)) == "true";
  exec_sqlf(ctx, "DROP TABLE {};", k_reaper_catalog_race_guard);
  REQUIRE(guard_survived);
}

TEST_CASE(
    "Orphan reaper serializes a concurrent shard reference",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();
  exec_sqlf(ctx, "DROP TABLE IF EXISTS {};", k_reaper_reference_race_table);
  exec_sql(
      ctx,
      "DELETE FROM bioseq_internal.seq_sub_table "
      " WHERE main_relfilenode = 710078::oid;");
  static_cast<void>(query_rows(
      ctx,
      "SELECT action, target "
      "  FROM bioseq.reap_orphan_seq_sub_tables();"));
  exec_sqlf(
      ctx,
      "CREATE TABLE {} (generation integer);",
      k_reaper_reference_race_table);
  const std::string relation_oid = query_scalar(
      ctx,
      fmt::format(
          "SELECT '{}'::regclass::oid::text;",
          k_reaper_reference_race_table));

  connection_test_context writer;
  connection_test_context worker;
  connection_test_context observer;
  const long long worker_pid =
      query_scalar_int(worker, "SELECT pg_backend_pid();");
  exec_sql(worker, "SET statement_timeout = '30s';");
  exec_sql(writer, "BEGIN;");
  exec_sql(
      writer,
      fmt::format(
          "LOCK TABLE {} IN ROW EXCLUSIVE MODE;",
          k_reaper_reference_race_table));

  scoped_async_query reap{worker.raw()};
  const bool sent = reap.send(
      "SELECT action, target "
      "  FROM bioseq.reap_orphan_seq_sub_tables() "
      " ORDER BY action, target;");
  const bool wait_observed =
      sent && wait_for_relation_lock(
          observer,
          worker_pid,
          k_reaper_reference_race_table,
          "AccessExclusiveLock",
          false);

  // Follow the writer's lock order: lock the shard, then add its catalog row.
  // After this commit, the waiting reaper must see the reference and keep both.
  exec_sql(
      writer,
      fmt::format(
          "INSERT INTO bioseq_internal.seq_sub_table ("
          " database_oid, tablespace_oid, main_relfilenode,"
          " column_attno, bucket_idx, shard_idx,"
          " sub_table_relid, sidecar_relid,"
          " bytes_written, is_full, total_payload_bytes"
          ") VALUES ("
          " (SELECT oid FROM pg_catalog.pg_database"
          "   WHERE datname = current_database()),"
          " (SELECT oid FROM pg_catalog.pg_tablespace"
          "   WHERE spcname = 'pg_default'), 710078::oid,"
          " 1::smallint, 0::smallint, 710078::integer,"
          " {}::oid, {}::oid, 0::bigint, false, 0::bigint);",
          relation_oid,
          relation_oid));
  exec_sql(writer, "COMMIT;");
  const async_query_outcome outcome =
      sent ? reap.drain() : async_query_outcome{};

  const bool target_absent_from_events = std::ranges::none_of(
      outcome.rows,
      [](const row_t &event) {
        return event.size() == 2
            && event[1]
                == std::optional<std::string>{
                    k_reaper_reference_race_table};
      });
  const bool relation_survived = query_scalar(
      ctx,
      fmt::format(
          "SELECT (to_regclass('{}') IS NOT NULL)::text;",
          k_reaper_reference_race_table)) == "true";
  const long long reference_count = query_scalar_int(
      ctx,
      fmt::format(
          "SELECT count(*) "
          "  FROM bioseq_internal.seq_sub_table "
          " WHERE main_relfilenode = 710078::oid "
          "   AND sub_table_relid = {}::oid;",
          relation_oid));
  exec_sql(
      ctx,
      "DELETE FROM bioseq_internal.seq_sub_table "
      " WHERE main_relfilenode = 710078::oid;");
  exec_sqlf(ctx, "DROP TABLE {};", k_reaper_reference_race_table);

  REQUIRE(sent);
  REQUIRE(wait_observed);
  REQUIRE(outcome.consume_ok);
  REQUIRE(outcome.ready);
  REQUIRE(outcome.got_result);
  REQUIRE(outcome.status == PGRES_TUPLES_OK);
  REQUIRE(target_absent_from_events);
  REQUIRE(relation_survived);
  REQUIRE(reference_count == 1);
}

// Integration coverage for the Bioseq administration views and the native
// table-option functions.

#include <tests/support/postgres.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

import bioseq.integration_tests.common;

using namespace bioseq_int_tests;

#ifndef BIOSEQ_DOC0388_ALTER_HOOK_PROBE_PATH
#error "DOC0388 alter-hook probe DSO path is required"
#endif

namespace {

constexpr std::string_view kTable = "test_admin_t";
constexpr std::string_view kSetTable = "test_admin_alter_set_t";
constexpr std::string_view kResetTable = "test_admin_alter_reset_t";
constexpr std::string_view kErrorTable = "test_admin_alter_error_t";
constexpr std::string_view kHeapTable = "test_admin_alter_heap_t";
constexpr std::string_view kOwnershipTable = "test_admin_alter_owner_t";
constexpr std::string_view kLegacyTable = "test_admin_alter_legacy_t";
constexpr std::string_view kNonOwnerRole = "doc0388_alter_nonowner";
constexpr std::string_view kAlterHookProbePath =
    BIOSEQ_DOC0388_ALTER_HOOK_PROBE_PATH;

long long row_count(const connection_test_context &ctx,
                    std::string_view view_or_table, std::string_view filter) {
  return query_scalar_int(
      ctx, fmt::format("SELECT COUNT(*)::bigint FROM {} WHERE {};",
                       view_or_table, filter));
}

[[nodiscard]] row_t table_options(const connection_test_context &ctx,
                                  std::string_view table) {
  return query_one(ctx, fmt::format("SELECT chunk_group_row_limit::text, "
                                    "       stripe_row_limit::text, "
                                    "       compression::text, "
                                    "       compression_level::text "
                                    "  FROM bioseq.options "
                                    " WHERE relation = '{}'::regclass;",
                                    table));
}

[[nodiscard]] int compression_code(std::string_view compression) {
  if (compression == "none") {
    return 0;
  }
  if (compression == "pglz") {
    return 1;
  }
  if (compression == "lz4") {
    return 2;
  }
  if (compression == "zstd") {
    return 3;
  }
  FAIL(fmt::format("unexpected Bioseq compression value: {}", compression));
  return -1;
}

void install_alter_hook_probe(const connection_test_context &ctx) {
  REQUIRE_FALSE(kAlterHookProbePath.empty());
  exec_sqlf(ctx,
            "CREATE FUNCTION pg_temp.doc0388_alter_hook_snapshot() "
            "RETURNS text "
            "AS '{}', 'doc0388_alter_hook_snapshot' "
            "LANGUAGE C;",
            kAlterHookProbePath);
  REQUIRE(query_scalar(ctx, "SELECT pg_temp.doc0388_alter_hook_snapshot();") ==
          "0:0:0:0:0:0");
}

[[nodiscard]] std::string
alter_hook_snapshot(const connection_test_context &ctx) {
  return query_scalar(ctx, "SELECT pg_temp.doc0388_alter_hook_snapshot();");
}

void require_hook_snapshot(const connection_test_context &ctx,
                           std::uint64_t call_count, std::string_view table,
                           const row_t &options) {
  REQUIRE(options.size() == 4);
  REQUIRE(options[0].has_value());
  REQUIRE(options[1].has_value());
  REQUIRE(options[2].has_value());
  REQUIRE(options[3].has_value());
  const std::string relation_oid = query_scalar(
      ctx, fmt::format("SELECT '{}'::regclass::oid::text;", table));
  REQUIRE(alter_hook_snapshot(ctx) ==
          fmt::format("{}:{}:{}:{}:{}:{}", call_count, relation_oid,
                      options[1].value(), options[0].value(),
                      compression_code(options[2].value()),
                      options[3].value()));
}

void set_expected(row_t &options, std::size_t index, std::string_view value) {
  REQUIRE(index < options.size());
  options[index] = std::string{value};
}

void require_failed_change(const connection_test_context &ctx,
                           std::string_view table, std::string_view sql,
                           std::string_view sqlstate, std::string_view message,
                           const row_t &expected_options,
                           std::string_view expected_hook_snapshot) {
  const sql_error_info error = sql_error(ctx, sql);
  REQUIRE(error.sqlstate == sqlstate);
  REQUIRE(error.contains(message));
  REQUIRE(table_options(ctx, table) == expected_options);
  REQUIRE(alter_hook_snapshot(ctx) == expected_hook_snapshot);
  REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

} // namespace

TEST_CASE("bioseq.options carries default settings for a new columnar table",
          "[admin][options]") {
  auto ctx = fresh_context({kTable});
  seed_small_bioseq_column(ctx, kTable);

  const auto row = query_one(
      ctx, fmt::format("SELECT chunk_group_row_limit, stripe_row_limit, "
                       "       compression "
                       "  FROM bioseq.options "
                       " WHERE relation = '{}'::regclass;",
                       kTable));
  REQUIRE(row.size() == 3);
  REQUIRE(std::stoll(row[0].value()) > 0); // chunk_group_row_limit
  REQUIRE(std::stoll(row[1].value()) > 0); // stripe_row_limit
  REQUIRE_FALSE(row[2].value().empty());   // compression name
}

TEST_CASE("bioseq.storage exposes one row per columnar relation",
          "[admin][views]") {
  auto ctx = fresh_context({kTable});
  seed_small_bioseq_column(ctx, kTable);

  REQUIRE(row_count(ctx, "bioseq.storage",
                    fmt::format("relation = '{}'::regclass", kTable)) == 1);
}

TEST_CASE("bioseq.stripe / chunk_group / chunk surface stripe metadata",
          "[admin][views]") {
  auto ctx = fresh_context({kTable});
  seed_small_bioseq_column(ctx, kTable);

  REQUIRE(row_count(ctx, "bioseq.stripe",
                    fmt::format("relation = '{}'::regclass", kTable)) >= 1);
  REQUIRE(row_count(ctx, "bioseq.chunk_group",
                    fmt::format("relation = '{}'::regclass", kTable)) >= 1);
  REQUIRE(row_count(ctx, "bioseq.chunk",
                    fmt::format("relation = '{}'::regclass", kTable)) >= 1);
}

TEST_CASE("native alter-set persists every option and meaningful combinations",
          "[admin][options][doc0388][native][alter-options]") {
  auto ctx = fresh_context({kSetTable});
  seed_small_bioseq_column(ctx, kSetTable);
  install_alter_hook_probe(ctx);

  row_t expected = table_options(ctx, kSetTable);
  REQUIRE(expected.size() == 4);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_set("
            "  table_name => '{}'::regclass,"
            "  chunk_group_row_limit => 1100);",
            kSetTable);
  set_expected(expected, 0, "1100");
  REQUIRE(table_options(ctx, kSetTable) == expected);
  require_hook_snapshot(ctx, 1, kSetTable, expected);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_set("
            "  table_name => '{}'::regclass,"
            "  stripe_row_limit => 2100);",
            kSetTable);
  set_expected(expected, 1, "2100");
  REQUIRE(table_options(ctx, kSetTable) == expected);
  require_hook_snapshot(ctx, 2, kSetTable, expected);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_set("
            "  table_name => '{}'::regclass,"
            "  compression => 'none'::name);",
            kSetTable);
  set_expected(expected, 2, "none");
  REQUIRE(table_options(ctx, kSetTable) == expected);
  require_hook_snapshot(ctx, 3, kSetTable, expected);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_set("
            "  table_name => '{}'::regclass,"
            "  compression_level => 2);",
            kSetTable);
  set_expected(expected, 3, "2");
  REQUIRE(table_options(ctx, kSetTable) == expected);
  require_hook_snapshot(ctx, 4, kSetTable, expected);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_set("
            "  table_name => '{}'::regclass,"
            "  chunk_group_row_limit => 3100,"
            "  stripe_row_limit => 4100);",
            kSetTable);
  set_expected(expected, 0, "3100");
  set_expected(expected, 1, "4100");
  REQUIRE(table_options(ctx, kSetTable) == expected);
  require_hook_snapshot(ctx, 5, kSetTable, expected);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_set("
            "  table_name => '{}'::regclass,"
            "  compression => 'pglz'::name,"
            "  compression_level => 7);",
            kSetTable);
  set_expected(expected, 2, "pglz");
  set_expected(expected, 3, "7");
  REQUIRE(table_options(ctx, kSetTable) == expected);
  require_hook_snapshot(ctx, 6, kSetTable, expected);

  REQUIRE(query_scalar_int(
              ctx, fmt::format("SELECT count(*) FROM {};", kSetTable)) == 3);
  exec_sqlf(ctx, "DROP TABLE {} CASCADE;", kSetTable);
}

TEST_CASE("native alter-reset uses current GUC values per flag and combination",
          "[admin][options][doc0388][native][alter-options]") {
  auto ctx = fresh_context({kResetTable});
  seed_small_bioseq_column(ctx, kResetTable);
  install_alter_hook_probe(ctx);

  guc_scope chunk_default(ctx, "bioseq.chunk_group_row_limit", "3100");
  guc_scope stripe_default(ctx, "bioseq.stripe_row_limit", "4100");
  guc_scope compression_default(ctx, "bioseq.compression", "'pglz'");
  guc_scope compression_level_default(ctx, "bioseq.compression_level", "7");

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_set("
            "  '{}'::regclass, 1100, 2100, 'none'::name, 2);",
            kResetTable);
  row_t expected{
      std::string{"1100"},
      std::string{"2100"},
      std::string{"none"},
      std::string{"2"},
  };
  REQUIRE(table_options(ctx, kResetTable) == expected);
  require_hook_snapshot(ctx, 1, kResetTable, expected);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_reset("
            "  table_name => '{}'::regclass,"
            "  chunk_group_row_limit => true);",
            kResetTable);
  set_expected(expected, 0, "3100");
  REQUIRE(table_options(ctx, kResetTable) == expected);
  require_hook_snapshot(ctx, 2, kResetTable, expected);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_reset("
            "  table_name => '{}'::regclass,"
            "  stripe_row_limit => true);",
            kResetTable);
  set_expected(expected, 1, "4100");
  REQUIRE(table_options(ctx, kResetTable) == expected);
  require_hook_snapshot(ctx, 3, kResetTable, expected);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_reset("
            "  table_name => '{}'::regclass,"
            "  compression => true);",
            kResetTable);
  set_expected(expected, 2, "pglz");
  REQUIRE(table_options(ctx, kResetTable) == expected);
  require_hook_snapshot(ctx, 4, kResetTable, expected);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_reset("
            "  table_name => '{}'::regclass,"
            "  compression_level => true);",
            kResetTable);
  set_expected(expected, 3, "7");
  REQUIRE(table_options(ctx, kResetTable) == expected);
  require_hook_snapshot(ctx, 5, kResetTable, expected);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_set("
            "  '{}'::regclass, 1200, 2200, 'none'::name, 3);",
            kResetTable);
  expected = row_t{
      std::string{"1200"},
      std::string{"2200"},
      std::string{"none"},
      std::string{"3"},
  };
  REQUIRE(table_options(ctx, kResetTable) == expected);
  require_hook_snapshot(ctx, 6, kResetTable, expected);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_reset("
            "  '{}'::regclass, true, false, true, false);",
            kResetTable);
  set_expected(expected, 0, "3100");
  set_expected(expected, 2, "pglz");
  REQUIRE(table_options(ctx, kResetTable) == expected);
  require_hook_snapshot(ctx, 7, kResetTable, expected);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_reset("
            "  '{}'::regclass, false, true, false, true);",
            kResetTable);
  set_expected(expected, 1, "4100");
  set_expected(expected, 3, "7");
  REQUIRE(table_options(ctx, kResetTable) == expected);
  require_hook_snapshot(ctx, 8, kResetTable, expected);

  REQUIRE(query_one(ctx,
                    "SELECT current_setting("
                    "           'bioseq.chunk_group_row_limit'),"
                    "       current_setting('bioseq.stripe_row_limit'),"
                    "       current_setting('bioseq.compression'),"
                    "       current_setting('bioseq.compression_level');") ==
          expected);
  exec_sqlf(ctx, "DROP TABLE {} CASCADE;", kResetTable);
}

TEST_CASE("native alter functions preserve no-op state and reject bad changes",
          "[admin][options][doc0388][native][alter-options][errors]") {
  auto ctx = fresh_context({kErrorTable, kHeapTable});
  seed_small_bioseq_column(ctx, kErrorTable);
  exec_sqlf(ctx, "CREATE TEMP TABLE {} (id integer);", kHeapTable);
  install_alter_hook_probe(ctx);

  const row_t initial = table_options(ctx, kErrorTable);
  const std::string untouched_hook = alter_hook_snapshot(ctx);
  REQUIRE(untouched_hook == "0:0:0:0:0:0");

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_set("
            "  '{}'::regclass);",
            kErrorTable);
  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_set("
            "  '{}'::regclass, NULL, NULL, NULL, NULL);",
            kErrorTable);
  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_reset("
            "  '{}'::regclass);",
            kErrorTable);
  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_reset("
            "  '{}'::regclass, NULL, NULL, NULL, NULL);",
            kErrorTable);
  exec_sql(ctx, "SELECT pg_catalog.alter_bioseq_table_set(NULL::regclass);");
  exec_sql(ctx, "SELECT pg_catalog.alter_bioseq_table_set("
                "  NULL::regclass, NULL, NULL, NULL, NULL);");
  exec_sql(ctx, "SELECT pg_catalog.alter_bioseq_table_reset(NULL::regclass);");
  exec_sql(ctx, "SELECT pg_catalog.alter_bioseq_table_reset("
                "  NULL::regclass, NULL, NULL, NULL, NULL);");
  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_set("
            "  '{}'::regclass);",
            kHeapTable);
  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_reset("
            "  '{}'::regclass);",
            kHeapTable);
  REQUIRE(table_options(ctx, kErrorTable) == initial);
  REQUIRE(alter_hook_snapshot(ctx) == untouched_hook);

  require_failed_change(ctx, kErrorTable,
                        "SELECT pg_catalog.alter_bioseq_table_set("
                        "  NULL::regclass, chunk_group_row_limit => 1100);",
                        "XX000", "table_name must not be NULL", initial,
                        untouched_hook);
  require_failed_change(ctx, kErrorTable,
                        "SELECT pg_catalog.alter_bioseq_table_reset("
                        "  NULL::regclass, chunk_group_row_limit => true);",
                        "XX000", "table_name must not be NULL", initial,
                        untouched_hook);

  const struct {
    std::string sql;
    std::string_view message;
  } invalid_changes[] = {
      {fmt::format("SELECT pg_catalog.alter_bioseq_table_set("
                   "  '{}'::regclass, chunk_group_row_limit => 999);",
                   kErrorTable),
       "chunk group row count limit out of range"},
      {fmt::format("SELECT pg_catalog.alter_bioseq_table_set("
                   "  '{}'::regclass, chunk_group_row_limit => 100000001);",
                   kErrorTable),
       "chunk group row count limit out of range"},
      {fmt::format("SELECT pg_catalog.alter_bioseq_table_set("
                   "  '{}'::regclass, stripe_row_limit => 999);",
                   kErrorTable),
       "stripe row count limit out of range"},
      {fmt::format("SELECT pg_catalog.alter_bioseq_table_set("
                   "  '{}'::regclass, stripe_row_limit => 100000001);",
                   kErrorTable),
       "stripe row count limit out of range"},
      {fmt::format("SELECT pg_catalog.alter_bioseq_table_set("
                   "  '{}'::regclass, compression => 'not_a_codec'::name);",
                   kErrorTable),
       "unknown compression type for columnar table"},
      {fmt::format("SELECT pg_catalog.alter_bioseq_table_set("
                   "  '{}'::regclass, compression_level => 0);",
                   kErrorTable),
       "compression level out of range"},
      {fmt::format("SELECT pg_catalog.alter_bioseq_table_set("
                   "  '{}'::regclass, compression_level => 20);",
                   kErrorTable),
       "compression level out of range"},
  };
  for (const auto &invalid : invalid_changes) {
    require_failed_change(ctx, kErrorTable, invalid.sql, "XX000",
                          invalid.message, initial, untouched_hook);
  }

  const sql_error_info non_bioseq_set = sql_error(
      ctx, fmt::format("SELECT pg_catalog.alter_bioseq_table_set("
                       "  '{}'::regclass, chunk_group_row_limit => 1100);",
                       kHeapTable));
  REQUIRE(non_bioseq_set.sqlstate == "XX000");
  REQUIRE(non_bioseq_set.contains("is not a columnar table"));
  const sql_error_info non_bioseq_reset = sql_error(
      ctx, fmt::format("SELECT pg_catalog.alter_bioseq_table_reset("
                       "  '{}'::regclass, chunk_group_row_limit => true);",
                       kHeapTable));
  REQUIRE(non_bioseq_reset.sqlstate == "XX000");
  REQUIRE(non_bioseq_reset.contains("is not a columnar table"));
  REQUIRE(table_options(ctx, kErrorTable) == initial);
  REQUIRE(alter_hook_snapshot(ctx) == untouched_hook);
  exec_sqlf(ctx, "DROP TABLE {} CASCADE;", kErrorTable);
  exec_sqlf(ctx, "DROP TABLE {};", kHeapTable);
}

TEST_CASE("native alter functions require Bioseq table ownership",
          "[admin][options][doc0388][native][alter-options][ownership]") {
  auto ctx = fresh_context({kOwnershipTable});
  seed_small_bioseq_column(ctx, kOwnershipTable);
  install_alter_hook_probe(ctx);

  const row_t initial = table_options(ctx, kOwnershipTable);
  const std::string untouched_hook = alter_hook_snapshot(ctx);

  const auto require_nonowner_failure = [&](std::string_view invocation) {
    exec_sql(ctx, "BEGIN;");
    exec_sqlf(ctx, "CREATE ROLE {} NOLOGIN;", kNonOwnerRole);
    exec_sqlf(ctx, "SET LOCAL ROLE {};", kNonOwnerRole);
    const sql_error_info error = sql_error(ctx, invocation);
    REQUIRE(error.sqlstate == "42501");
    REQUIRE(error.contains("must be owner of table"));
    exec_sql(ctx, "ROLLBACK;");
    REQUIRE(query_scalar(ctx, fmt::format("SELECT (count(*) = 0)::text "
                                          "FROM pg_catalog.pg_roles "
                                          "WHERE rolname = '{}';",
                                          kNonOwnerRole)) == "true");
    REQUIRE(table_options(ctx, kOwnershipTable) == initial);
    REQUIRE(alter_hook_snapshot(ctx) == untouched_hook);
  };

  require_nonowner_failure(
      fmt::format("SELECT pg_catalog.alter_bioseq_table_set("
                  "  '{}'::regclass, chunk_group_row_limit => 1100);",
                  kOwnershipTable));
  require_nonowner_failure(
      fmt::format("SELECT pg_catalog.alter_bioseq_table_reset("
                  "  '{}'::regclass, chunk_group_row_limit => true);",
                  kOwnershipTable));
  exec_sqlf(ctx, "DROP TABLE {} CASCADE;", kOwnershipTable);
}

TEST_CASE("native alter path succeeds where legacy namespaced DDL fails",
          "[admin][options][doc0388][native][alter-options][legacy-delta]") {
  auto ctx = fresh_context({kLegacyTable});
  seed_small_bioseq_column(ctx, kLegacyTable);
  install_alter_hook_probe(ctx);

  exec_sql(ctx, R"sql(
CREATE FUNCTION pg_temp.doc0388_legacy_alter_set(
    table_name regclass,
    chunk_group_row_limit integer)
RETURNS void
LANGUAGE plpgsql
AS $legacy$
DECLARE
  cmd text := 'ALTER TABLE ' || table_name::text || ' SET (';
BEGIN
  cmd := cmd || 'bioseq.chunk_group_row_limit=' ||
         chunk_group_row_limit || ')';
  EXECUTE cmd;
END;
$legacy$;

CREATE FUNCTION pg_temp.doc0388_legacy_alter_reset(
    table_name regclass)
RETURNS void
LANGUAGE plpgsql
AS $legacy$
DECLARE
  cmd text := 'ALTER TABLE ' || table_name::text ||
              ' RESET (bioseq.chunk_group_row_limit)';
BEGIN
  EXECUTE cmd;
END;
$legacy$;
)sql");

  const row_t initial = table_options(ctx, kLegacyTable);
  const std::string untouched_hook = alter_hook_snapshot(ctx);

  const sql_error_info legacy_set =
      sql_error(ctx, fmt::format("SELECT pg_temp.doc0388_legacy_alter_set("
                                 "  '{}'::regclass, 1200);",
                                 kLegacyTable));
  REQUIRE(legacy_set.sqlstate == "22023");
  REQUIRE(legacy_set.contains("unrecognized parameter"));
  REQUIRE(table_options(ctx, kLegacyTable) == initial);
  REQUIRE(alter_hook_snapshot(ctx) == untouched_hook);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_set("
            "  '{}'::regclass, chunk_group_row_limit => 1200);",
            kLegacyTable);
  row_t changed = initial;
  set_expected(changed, 0, "1200");
  REQUIRE(table_options(ctx, kLegacyTable) == changed);
  require_hook_snapshot(ctx, 1, kLegacyTable, changed);

  // PostgreSQL rejects SET for an unknown namespaced reloption, but RESET of
  // that absent option is a valid no-op.
  exec_sqlf(ctx,
            "SELECT pg_temp.doc0388_legacy_alter_reset("
            "  '{}'::regclass);",
            kLegacyTable);
  REQUIRE(table_options(ctx, kLegacyTable) == changed);
  require_hook_snapshot(ctx, 1, kLegacyTable, changed);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_reset("
            "  '{}'::regclass, chunk_group_row_limit => true);",
            kLegacyTable);
  REQUIRE(table_options(ctx, kLegacyTable) == initial);
  require_hook_snapshot(ctx, 2, kLegacyTable, initial);
  exec_sqlf(ctx, "DROP TABLE {} CASCADE;", kLegacyTable);
}

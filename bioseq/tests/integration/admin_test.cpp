// Integration coverage for the Bioseq administration views and the native
// table-option functions.

#include <tests/support/postgres.hpp>

#include <cstddef>
#include <string>
#include <string_view>

import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view k_table = "admin_options";
constexpr std::string_view k_set_table = "admin_alter_set";
constexpr std::string_view k_reset_table = "admin_alter_reset";
constexpr std::string_view k_error_table = "admin_alter_error";
constexpr std::string_view k_heap_table = "admin_alter_heap";
constexpr std::string_view k_ownership_table = "admin_alter_owner";
constexpr std::string_view k_non_owner_role = "bioseq_alter_nonowner";

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

void set_expected(row_t &options, std::size_t index, std::string_view value) {
  REQUIRE(index < options.size());
  options[index] = std::string{value};
}

void require_failed_change(const connection_test_context &ctx,
                           std::string_view table, std::string_view sql,
                           std::string_view sqlstate, std::string_view message,
                           const row_t &expected_options) {
  const sql_error_info error = sql_error(ctx, sql);
  REQUIRE(error.sqlstate == sqlstate);
  REQUIRE(error.contains(message));
  REQUIRE(table_options(ctx, table) == expected_options);
  REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

} // namespace

TEST_CASE("bioseq.options carries default settings for a new columnar table",
          "[component:bioseq][layer:integration][feature:configuration][resource:postgres]") {
  auto ctx = fresh_context({k_table});
  seed_small_bioseq_column(ctx, k_table);

  const auto row = query_one(
      ctx, fmt::format("SELECT chunk_group_row_limit, stripe_row_limit, "
                       "       compression "
                       "  FROM bioseq.options "
                       " WHERE relation = '{}'::regclass;",
                       k_table));
  REQUIRE(row.size() == 3);
  REQUIRE(std::stoll(row[0].value()) > 0); // chunk_group_row_limit
  REQUIRE(std::stoll(row[1].value()) > 0); // stripe_row_limit
  REQUIRE_FALSE(row[2].value().empty());   // compression name
}

TEST_CASE("bioseq.storage exposes one row per columnar relation",
          "[component:bioseq][layer:integration][feature:configuration][resource:postgres]") {
  auto ctx = fresh_context({k_table});
  seed_small_bioseq_column(ctx, k_table);

  REQUIRE(row_count(ctx, "bioseq.storage",
                    fmt::format("relation = '{}'::regclass", k_table)) == 1);
}

TEST_CASE("bioseq.stripe / chunk_group / chunk surface stripe metadata",
          "[component:bioseq][layer:integration][feature:configuration][resource:postgres]") {
  auto ctx = fresh_context({k_table});
  seed_small_bioseq_column(ctx, k_table);

  REQUIRE(row_count(ctx, "bioseq.stripe",
                    fmt::format("relation = '{}'::regclass", k_table)) >= 1);
  REQUIRE(row_count(ctx, "bioseq.chunk_group",
                    fmt::format("relation = '{}'::regclass", k_table)) >= 1);
  REQUIRE(row_count(ctx, "bioseq.chunk",
                    fmt::format("relation = '{}'::regclass", k_table)) >= 1);
}

TEST_CASE("native alter-set persists every option and meaningful combinations",
          "[component:bioseq][layer:integration][feature:configuration][resource:postgres]") {
  auto ctx = fresh_context({k_set_table});
  seed_small_bioseq_column(ctx, k_set_table);

  row_t expected = table_options(ctx, k_set_table);
  REQUIRE(expected.size() == 4);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_set("
            "  table_name => '{}'::regclass,"
            "  chunk_group_row_limit => 1100);",
            k_set_table);
  set_expected(expected, 0, "1100");
  REQUIRE(table_options(ctx, k_set_table) == expected);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_set("
            "  table_name => '{}'::regclass,"
            "  stripe_row_limit => 2100);",
            k_set_table);
  set_expected(expected, 1, "2100");
  REQUIRE(table_options(ctx, k_set_table) == expected);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_set("
            "  table_name => '{}'::regclass,"
            "  compression => 'none'::name);",
            k_set_table);
  set_expected(expected, 2, "none");
  REQUIRE(table_options(ctx, k_set_table) == expected);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_set("
            "  table_name => '{}'::regclass,"
            "  compression_level => 2);",
            k_set_table);
  set_expected(expected, 3, "2");
  REQUIRE(table_options(ctx, k_set_table) == expected);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_set("
            "  table_name => '{}'::regclass,"
            "  chunk_group_row_limit => 3100,"
            "  stripe_row_limit => 4100);",
            k_set_table);
  set_expected(expected, 0, "3100");
  set_expected(expected, 1, "4100");
  REQUIRE(table_options(ctx, k_set_table) == expected);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_set("
            "  table_name => '{}'::regclass,"
            "  compression => 'pglz'::name,"
            "  compression_level => 7);",
            k_set_table);
  set_expected(expected, 2, "pglz");
  set_expected(expected, 3, "7");
  REQUIRE(table_options(ctx, k_set_table) == expected);

  REQUIRE(query_scalar_int(
              ctx, fmt::format("SELECT count(*) FROM {};", k_set_table)) == 3);
  exec_sqlf(ctx, "DROP TABLE {} CASCADE;", k_set_table);
}

TEST_CASE("native alter-reset uses current GUC values per flag and combination",
          "[component:bioseq][layer:integration][feature:configuration][resource:postgres]") {
  auto ctx = fresh_context({k_reset_table});
  seed_small_bioseq_column(ctx, k_reset_table);

  guc_scope chunk_default(ctx, "bioseq.storage.chunk_group.row_limit", "3100");
  guc_scope stripe_default(ctx, "bioseq.storage.stripe.row_limit", "4100");
  guc_scope compression_default(ctx, "bioseq.storage.compression.algorithm", "'pglz'");
  guc_scope compression_level_default(ctx, "bioseq.storage.compression.level", "7");

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_set("
            "  '{}'::regclass, 1100, 2100, 'none'::name, 2);",
            k_reset_table);
  row_t expected{
      std::string{"1100"},
      std::string{"2100"},
      std::string{"none"},
      std::string{"2"},
  };
  REQUIRE(table_options(ctx, k_reset_table) == expected);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_reset("
            "  table_name => '{}'::regclass,"
            "  chunk_group_row_limit => true);",
            k_reset_table);
  set_expected(expected, 0, "3100");
  REQUIRE(table_options(ctx, k_reset_table) == expected);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_reset("
            "  table_name => '{}'::regclass,"
            "  stripe_row_limit => true);",
            k_reset_table);
  set_expected(expected, 1, "4100");
  REQUIRE(table_options(ctx, k_reset_table) == expected);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_reset("
            "  table_name => '{}'::regclass,"
            "  compression => true);",
            k_reset_table);
  set_expected(expected, 2, "pglz");
  REQUIRE(table_options(ctx, k_reset_table) == expected);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_reset("
            "  table_name => '{}'::regclass,"
            "  compression_level => true);",
            k_reset_table);
  set_expected(expected, 3, "7");
  REQUIRE(table_options(ctx, k_reset_table) == expected);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_set("
            "  '{}'::regclass, 1200, 2200, 'none'::name, 3);",
            k_reset_table);
  expected = row_t{
      std::string{"1200"},
      std::string{"2200"},
      std::string{"none"},
      std::string{"3"},
  };
  REQUIRE(table_options(ctx, k_reset_table) == expected);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_reset("
            "  '{}'::regclass, true, false, true, false);",
            k_reset_table);
  set_expected(expected, 0, "3100");
  set_expected(expected, 2, "pglz");
  REQUIRE(table_options(ctx, k_reset_table) == expected);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_reset("
            "  '{}'::regclass, false, true, false, true);",
            k_reset_table);
  set_expected(expected, 1, "4100");
  set_expected(expected, 3, "7");
  REQUIRE(table_options(ctx, k_reset_table) == expected);

  REQUIRE(query_one(ctx,
                    "SELECT current_setting("
                    "           'bioseq.storage.chunk_group.row_limit'),"
                    "       current_setting('bioseq.storage.stripe.row_limit'),"
                    "       current_setting('bioseq.storage.compression.algorithm'),"
                    "       current_setting('bioseq.storage.compression.level');") ==
          expected);
  exec_sqlf(ctx, "DROP TABLE {} CASCADE;", k_reset_table);
}

TEST_CASE("native alter functions preserve no-op state and reject bad changes",
          "[component:bioseq][layer:integration][feature:configuration][resource:postgres]") {
  auto ctx = fresh_context({k_error_table, k_heap_table});
  seed_small_bioseq_column(ctx, k_error_table);
  exec_sqlf(ctx, "CREATE TEMP TABLE {} (id integer);", k_heap_table);

  const row_t initial = table_options(ctx, k_error_table);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_set("
            "  '{}'::regclass);",
            k_error_table);
  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_set("
            "  '{}'::regclass, NULL, NULL, NULL, NULL);",
            k_error_table);
  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_reset("
            "  '{}'::regclass);",
            k_error_table);
  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_reset("
            "  '{}'::regclass, NULL, NULL, NULL, NULL);",
            k_error_table);
  exec_sql(ctx, "SELECT pg_catalog.alter_bioseq_table_set(NULL::regclass);");
  exec_sql(ctx, "SELECT pg_catalog.alter_bioseq_table_set("
                "  NULL::regclass, NULL, NULL, NULL, NULL);");
  exec_sql(ctx, "SELECT pg_catalog.alter_bioseq_table_reset(NULL::regclass);");
  exec_sql(ctx, "SELECT pg_catalog.alter_bioseq_table_reset("
                "  NULL::regclass, NULL, NULL, NULL, NULL);");
  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_set("
            "  '{}'::regclass);",
            k_heap_table);
  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_reset("
            "  '{}'::regclass);",
            k_heap_table);
  REQUIRE(table_options(ctx, k_error_table) == initial);

  require_failed_change(ctx, k_error_table,
                        "SELECT pg_catalog.alter_bioseq_table_set("
                        "  NULL::regclass, chunk_group_row_limit => 1100);",
                        "XX000", "table_name must not be NULL", initial);
  require_failed_change(ctx, k_error_table,
                        "SELECT pg_catalog.alter_bioseq_table_reset("
                        "  NULL::regclass, chunk_group_row_limit => true);",
                        "XX000", "table_name must not be NULL", initial);

  const struct {
    std::string sql;
    std::string_view message;
  } invalid_changes[] = {
      {fmt::format("SELECT pg_catalog.alter_bioseq_table_set("
                   "  '{}'::regclass, chunk_group_row_limit => 999);",
                   k_error_table),
       "chunk group row count limit out of range"},
      {fmt::format("SELECT pg_catalog.alter_bioseq_table_set("
                   "  '{}'::regclass, chunk_group_row_limit => 100000001);",
                   k_error_table),
       "chunk group row count limit out of range"},
      {fmt::format("SELECT pg_catalog.alter_bioseq_table_set("
                   "  '{}'::regclass, stripe_row_limit => 999);",
                   k_error_table),
       "stripe row count limit out of range"},
      {fmt::format("SELECT pg_catalog.alter_bioseq_table_set("
                   "  '{}'::regclass, stripe_row_limit => 100000001);",
                   k_error_table),
       "stripe row count limit out of range"},
      {fmt::format("SELECT pg_catalog.alter_bioseq_table_set("
                   "  '{}'::regclass, compression => 'not_a_codec'::name);",
                   k_error_table),
       "unknown compression type for columnar table"},
      {fmt::format("SELECT pg_catalog.alter_bioseq_table_set("
                   "  '{}'::regclass, compression_level => 0);",
                   k_error_table),
       "compression level out of range"},
      {fmt::format("SELECT pg_catalog.alter_bioseq_table_set("
                   "  '{}'::regclass, compression_level => 20);",
                   k_error_table),
       "compression level out of range"},
  };
  for (const auto &invalid : invalid_changes) {
    require_failed_change(ctx, k_error_table, invalid.sql, "XX000",
                          invalid.message, initial);
  }

  const sql_error_info non_bioseq_set = sql_error(
      ctx, fmt::format("SELECT pg_catalog.alter_bioseq_table_set("
                       "  '{}'::regclass, chunk_group_row_limit => 1100);",
                       k_heap_table));
  REQUIRE(non_bioseq_set.sqlstate == "XX000");
  REQUIRE(non_bioseq_set.contains("is not a columnar table"));
  const sql_error_info non_bioseq_reset = sql_error(
      ctx, fmt::format("SELECT pg_catalog.alter_bioseq_table_reset("
                       "  '{}'::regclass, chunk_group_row_limit => true);",
                       k_heap_table));
  REQUIRE(non_bioseq_reset.sqlstate == "XX000");
  REQUIRE(non_bioseq_reset.contains("is not a columnar table"));
  REQUIRE(table_options(ctx, k_error_table) == initial);
  exec_sqlf(ctx, "DROP TABLE {} CASCADE;", k_error_table);
  exec_sqlf(ctx, "DROP TABLE {};", k_heap_table);
}

TEST_CASE("native alter functions require Bioseq table ownership",
          "[component:bioseq][layer:integration][feature:configuration][resource:postgres]") {
  auto ctx = fresh_context({k_ownership_table});
  seed_small_bioseq_column(ctx, k_ownership_table);

  const row_t initial = table_options(ctx, k_ownership_table);

  const auto require_nonowner_failure = [&](std::string_view invocation) {
    exec_sql(ctx, "BEGIN;");
    exec_sqlf(ctx, "CREATE ROLE {} NOLOGIN;", k_non_owner_role);
    exec_sqlf(ctx, "SET LOCAL ROLE {};", k_non_owner_role);
    const sql_error_info error = sql_error(ctx, invocation);
    REQUIRE(error.sqlstate == "42501");
    REQUIRE(error.contains("must be owner of table"));
    exec_sql(ctx, "ROLLBACK;");
    REQUIRE(query_scalar(ctx, fmt::format("SELECT (count(*) = 0)::text "
                                          "FROM pg_catalog.pg_roles "
                                          "WHERE rolname = '{}';",
                                          k_non_owner_role)) == "true");
    REQUIRE(table_options(ctx, k_ownership_table) == initial);
  };

  require_nonowner_failure(
      fmt::format("SELECT pg_catalog.alter_bioseq_table_set("
                  "  '{}'::regclass, chunk_group_row_limit => 1100);",
                  k_ownership_table));
  require_nonowner_failure(
      fmt::format("SELECT pg_catalog.alter_bioseq_table_reset("
                  "  '{}'::regclass, chunk_group_row_limit => true);",
                  k_ownership_table));
  exec_sqlf(ctx, "DROP TABLE {} CASCADE;", k_ownership_table);
}

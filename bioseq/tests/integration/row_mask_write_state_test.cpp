#include <tests/support/postgres.hpp>

import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view k_table = "row_mask_write_state";

void create_row_mask_table(const connection_test_context &ctx) {
  exec_sqlf(ctx,
            "CREATE TABLE {} (id int PRIMARY KEY, payload int NOT NULL) USING "
            "bioseq;",
            k_table);
  exec_sqlf(ctx,
            "INSERT INTO {} "
            "SELECT value, value * 10 FROM generate_series(1, 4) AS "
            "generated(value);",
            k_table);

  REQUIRE(query_scalar_int(
              ctx, "SELECT count(*) FROM bioseq_internal.row_mask;") == 1);
}

} // namespace

TEST_CASE("row-mask writes survive explicit same-transaction flush",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
  auto ctx = fresh_context({k_table});
  create_row_mask_table(ctx);

  // Keep the covered-row prefix clear while making the catalog value large
  // enough to exercise PostgreSQL's ordinary varlena detoast path.
  exec_sql(ctx,
           "UPDATE bioseq_internal.row_mask "
           "SET mask = ("
           " SELECT decode("
           "   repeat('00', 16) || "
           "   left(string_agg(md5(value::text), '' ORDER BY value), 16360),"
           "   'hex')"
           " FROM generate_series(1, 512) AS generated(value)"
           ");");
  REQUIRE(
      query_scalar_int(
          ctx, "SELECT octet_length(mask) FROM bioseq_internal.row_mask;") ==
      8196);

  exec_sql(ctx, "BEGIN; SET LOCAL enable_seqscan = off;");
  REQUIRE(
      query_scalar_int(
          ctx, fmt::format(
                   "WITH removed AS (DELETE FROM {} WHERE id = 1 RETURNING 1) "
                   "SELECT count(*) FROM removed;",
                   k_table)) == 1);
  REQUIRE(
      query_scalar_int(
          ctx, fmt::format(
                   "WITH removed AS (DELETE FROM {} WHERE id = 2 RETURNING 1) "
                   "SELECT count(*) FROM removed;",
                   k_table)) == 1);

  // A normal sequential read explicitly flushes the pending mask and then
  // consumes the persisted value in the same transaction.
  exec_sql(ctx, "SET LOCAL enable_seqscan = on; "
                "SET LOCAL enable_indexscan = off; "
                "SET LOCAL enable_indexonlyscan = off; "
                "SET LOCAL enable_bitmapscan = off;");
  REQUIRE(query_scalar_int(
              ctx, fmt::format("SELECT count(*) FROM {};", k_table)) == 2);
  REQUIRE(query_scalar_int(
              ctx, "SELECT deleted_rows FROM bioseq_internal.row_mask;") == 2);
  REQUIRE(
      query_scalar_int(
          ctx, "SELECT get_byte(mask, 0) & 3 FROM bioseq_internal.row_mask;") ==
      3);
  REQUIRE(
      query_scalar_int(
          ctx, "SELECT octet_length(mask) FROM bioseq_internal.row_mask;") ==
      8196);

  exec_sql(ctx, "COMMIT;");
  REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);

  exec_sql(ctx, "UPDATE bioseq_internal.row_mask SET mask = NULL;");
  const auto null_mask_error =
      sql_error(ctx, fmt::format("DELETE FROM {} WHERE id = 3;", k_table));
  REQUIRE(null_mask_error.contains("row mask catalog entry has a NULL mask"));
  REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

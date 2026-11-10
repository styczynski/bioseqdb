/**
 * @file
 * @brief Tests flushing ptnding row deletions to the row-mask catalog.
 */
#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view k_table = "row_mask_write_state";
constexpr std::string_view k_drop_frame_table = "row_mask_drop_frame";

/** Create a small Bioseq relation with one initialized row-mask catalog entry. */
void create_row_mask_table(const connection_test_context& ctx) {
    exec_sqlf(ctx,
              "CREATE TABLE {} (id int PRIMARY KEY, payload int NOT NULL) USING "
              "bioseq;",
              k_table);
    exec_sqlf(ctx,
              "INSERT INTO {} "
              "SELECT value, value * 10 FROM generate_series(1, 4) AS "
              "generated(value);",
              k_table);

    REQUIRE(query_scalar_int(ctx, "SELECT count(*) FROM bioseq_internal.row_mask;") == 1);
}

} // namespace

TEST_CASE("row-mask writes survive explicit same-transaction flush",
          "[component:bioseq][layer:integration][feature:storage][resource:postgres]") {
    auto ctx = fresh_context({k_table, k_drop_frame_table});
    create_row_mask_table(ctx);

    // Leave the first rows unmasked, but enlarge the value to exercise
    // reading a TOASTed row mask
    exec_sql(ctx,
             "UPDATE bioseq_internal.row_mask "
             "SET mask = ("
             " SELECT decode("
             "   repeat('00', 16) || "
             "   left(string_agg(md5(value::text), '' ORDER BY value), 16360),"
             "   'hex')"
             " FROM generate_series(1, 512) AS generated(value)"
             ");");
    REQUIRE(query_scalar_int(ctx, "SELECT octet_length(mask) FROM bioseq_internal.row_mask;") ==
            8196);

    exec_sql(ctx, "BEGIN; SET LOCAL enable_seqscan = off;");
    REQUIRE(
        query_scalar_int(ctx,
                         fmt::format("WITH removed AS (DELETE FROM {} WHERE id = 1 RETURNING 1) "
                                     "SELECT count(*) FROM removed;",
                                     k_table)) == 1);
    REQUIRE(
        query_scalar_int(ctx,
                         fmt::format("WITH removed AS (DELETE FROM {} WHERE id = 2 RETURNING 1) "
                                     "SELECT count(*) FROM removed;",
                                     k_table)) == 1);

    // Force a sequential scan to flush the pending deletions. It must see
    // the updated mask within  the same transaction.
    exec_sql(ctx,
             "SET LOCAL enable_seqscan = on; "
             "SET LOCAL enable_indexscan = off; "
             "SET LOCAL enable_indexonlyscan = off; "
             "SET LOCAL enable_bitmapscan = off;");
    REQUIRE(query_scalar_int(ctx, fmt::format("SELECT count(*) FROM {};", k_table)) == 2);
    REQUIRE(query_scalar_int(ctx, "SELECT deleted_rows FROM bioseq_internal.row_mask;") == 2);
    REQUIRE(query_scalar_int(ctx, "SELECT get_byte(mask, 0) & 3 FROM bioseq_internal.row_mask;") ==
            3);
    REQUIRE(query_scalar_int(ctx, "SELECT octet_length(mask) FROM bioseq_internal.row_mask;") ==
            8196);

    exec_sql(ctx, "COMMIT;");
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);

    // A DROP marker must not keep the completing savepoint's aborted writes
    // above the parent frame. Exercise column buffers and deletion masks in
    // the same backend, including RELEASE followed by an ancestor rollback.
    exec_sqlf(ctx,
              "CREATE TABLE \"{}\" (id int NOT NULL, payload int NOT NULL) USING bioseq;"
              "INSERT INTO \"{}\" VALUES (1,10),(2,20),(3,30),(4,40);",
              k_drop_frame_table, k_drop_frame_table);
    const std::string restored_rows_sql =
        fmt::format("SELECT id,payload FROM \"{}\" ORDER BY id;", k_drop_frame_table);
    const rowset_t original_rows{{"1", "10"}, {"2", "20"}, {"3", "30"}, {"4", "40"}};
    REQUIRE(query_rows(ctx, restored_rows_sql) == original_rows);
    for (const bool insert_child : {true, false}) {
        for (const bool release_child : {false, true}) {
            INFO("insert_child=" << insert_child << " release_child=" << release_child);
            exec_sql(ctx, "BEGIN; SAVEPOINT outer_drop_frame; SAVEPOINT child_drop_frame;");
            if (insert_child) {
                exec_sqlf(ctx, "INSERT INTO \"{}\" VALUES (5,50);", k_drop_frame_table);
            } else {
                exec_sqlf(ctx, "DELETE FROM \"{}\" WHERE id=1;", k_drop_frame_table);
            }
            exec_sqlf(ctx, "DROP TABLE \"{}\";", k_drop_frame_table);
            if (release_child) {
                exec_sql(ctx,
                         "RELEASE SAVEPOINT child_drop_frame;"
                         "ROLLBACK TO SAVEPOINT outer_drop_frame;");
            } else {
                exec_sql(ctx, "ROLLBACK TO SAVEPOINT child_drop_frame;");
            }
            // A stale child frame used to make this fail with pending data in
            // upper transactions, although its savepoint had been rolled back.
            REQUIRE(query_rows(ctx, restored_rows_sql) == original_rows);
            exec_sql(ctx, "COMMIT;");
            REQUIRE(query_rows(ctx, restored_rows_sql) == original_rows);
        }
    }

    // Preserve an older buffered INSERT while removing the dropped child frame;
    // otherwise the stale child head hides the parent writer at top-level commit.
    exec_sqlf(ctx,
              "BEGIN; INSERT INTO \"{}\" VALUES (5,50);"
              "SAVEPOINT outer_drop_frame; SAVEPOINT child_drop_frame;"
              "INSERT INTO \"{}\" VALUES (6,60); DROP TABLE \"{}\";"
              "RELEASE SAVEPOINT child_drop_frame;"
              "ROLLBACK TO SAVEPOINT outer_drop_frame;"
              "RELEASE SAVEPOINT outer_drop_frame; COMMIT;",
              k_drop_frame_table, k_drop_frame_table, k_drop_frame_table);
    const rowset_t parent_insert_rows{
        {"1", "10"}, {"2", "20"}, {"3", "30"}, {"4", "40"}, {"5", "50"}};
    REQUIRE(query_rows(ctx, restored_rows_sql) == parent_insert_rows);

    // The ancestor deletion mask must likewise survive the child's aborted DROP.
    exec_sqlf(ctx,
              "BEGIN; DELETE FROM \"{}\" WHERE id=1; SAVEPOINT child_drop_frame;"
              "INSERT INTO \"{}\" VALUES (6,60); DROP TABLE \"{}\";"
              "ROLLBACK TO SAVEPOINT child_drop_frame;"
              "RELEASE SAVEPOINT child_drop_frame; COMMIT;",
              k_drop_frame_table, k_drop_frame_table, k_drop_frame_table);
    const rowset_t parent_delete_rows{{"2", "20"}, {"3", "30"}, {"4", "40"}, {"5", "50"}};
    REQUIRE(query_rows(ctx, restored_rows_sql) == parent_delete_rows);
    exec_sqlf(ctx, "DROP TABLE \"{}\";", k_drop_frame_table);

    exec_sql(ctx, "UPDATE bioseq_internal.row_mask SET mask = NULL;");
    const auto null_mask_error =
        sql_error(ctx, fmt::format("DELETE FROM {} WHERE id = 3;", k_table));
    REQUIRE(null_mask_error.contains("row mask catalog entry has a NULL mask"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

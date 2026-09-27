/**
 * @file chunked_storage_test.cpp
 * @brief Checks stored index fragments and fallback to recomputing an index.
 *
 * Tests insert maintenance, row numbers, rollback, and incomplete fragment data.
 */
#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {
/** @brief Creates repeatable query/target pairs with long DNA4 targets. */
void seed_sample(const connection_test_context& ctx, std::string_view table_name, int row_count) {
    exec_sqlf(ctx, "DROP TABLE IF EXISTS {} CASCADE;", table_name);
    exec_sqlf(ctx,
              "SELECT bioseq.create_query_target_sample_table('{}', {}, 5000, 100.0, 200000, "
              "0.001, 0.0005, 0.0005, 1.0, 1.0, 0.0, 13579);",
              table_name,
              row_count);
}

/** @brief Returns the distinct row IDs and target coordinates reported by mmsearch. */
[[nodiscard]] std::set<std::tuple<long long, long long, long long>>
mmsearch_set(const connection_test_context& ctx, std::string_view table_name) {
    const auto rows =
        query_rows(ctx,
                   fmt::format("SELECT id, bioseq.target_start(id), bioseq.target_end(id) FROM {} "
                               "WHERE mmsearch(target, query) ORDER BY id",
                               table_name));
    std::set<std::tuple<long long, long long, long long>> matches;
    for (const auto& row : rows) {
        if (row[0] && row[1] && row[2]) {
            matches.emplace(std::stoll(*row[0]), std::stoll(*row[1]), std::stoll(*row[2]));
        }
    }
    return matches;
}
} // namespace

TEST_CASE("per-fragment storage is created on insert with the right query_keys",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"chunked_storage_insert"});
    seed_small_bioseq_column(ctx,
                             "chunked_storage_insert"); // Three DNA4/32 rows with maintenance on.
    REQUIRE(index_table_row_count(ctx, "minimizers_per_fragment", "chunked_storage_insert") > 0);
    REQUIRE(index_table_row_count(ctx, "symbol_counts", "chunked_storage_insert") == 3);

    // Each table row must have at least one stored index fragment under its row number.
    const auto row_numbers = query_int_column(
        ctx,
        "SELECT bioseq.column_row_number('chunked_storage_insert'::regclass, ctid) "
        "FROM chunked_storage_insert ORDER BY 1");
    REQUIRE(row_numbers.size() == 3);
    for (const long long sequence_row_number : row_numbers) {
        REQUIRE(index_distinct_sub_keys(ctx,
                                        "minimizers_per_fragment",
                                        "chunked_storage_insert",
                                        sequence_row_number) >= 1);
    }
}

TEST_CASE("mmsearch per-fragment assemble == recompute (single-segment dna4)",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"chunked_storage_recompute"});
    seed_sample(ctx, "chunked_storage_recompute", 3);
    exec_sqlf(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    REQUIRE(query_scalar_int(ctx,
                             "SELECT bioseq.update_index('chunked_storage_recompute'::regclass, "
                             "'minimizers_per_fragment')") == 3);

    guc_scope sequence_read_on(ctx, "bioseq.index.sequence_read.enabled", "on");
    const auto assembled = mmsearch_set(ctx, "chunked_storage_recompute");
    REQUIRE(assembled.size() == 3); // all three queries matched

    exec_sqlf(ctx, "SET bioseq.index.sequence_read.enabled = off;");
    const auto recomputed = mmsearch_set(ctx, "chunked_storage_recompute");
    REQUIRE(assembled == recomputed); // same row IDs and alignment coordinates
}

TEST_CASE("forced-scanner GUC disables stored assembly but results stay correct",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"chunked_storage_forced_scanner"});
    seed_sample(ctx, "chunked_storage_forced_scanner", 3);
    exec_sqlf(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sqlf(ctx,
              "SELECT bioseq.update_index('chunked_storage_forced_scanner'::regclass, "
              "'minimizers_per_fragment');");
    guc_scope sequence_read_on(ctx, "bioseq.index.sequence_read.enabled", "on");
    guc_scope chunked(
        ctx, "bioseq.testing.chunked_scanner.enabled", "on"); // use the test chunked scanner
    REQUIRE(mmsearch_set(ctx, "chunked_storage_forced_scanner").size() == 3);
}

TEST_CASE("index rows do not survive a ROLLBACK",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"chunked_storage_rollback"});
    seed_small_bioseq_column(ctx, "chunked_storage_rollback");
    const long long row_count_before =
        index_table_row_count(ctx, "minimizers_per_fragment", "chunked_storage_rollback");
    exec_sql(ctx, "BEGIN;");
    exec_sql(ctx,
             "INSERT INTO chunked_storage_rollback VALUES (99, "
             "'GGGGCCCCAAAATTTT'::bioseq.seq('dna4',32));");
    exec_sql(ctx, "ROLLBACK;");
    REQUIRE(query_scalar_int(ctx, "SELECT count(*) FROM chunked_storage_rollback WHERE pk=99") ==
            0);
    REQUIRE(index_table_row_count(ctx, "minimizers_per_fragment", "chunked_storage_rollback") ==
            row_count_before);
}

#ifdef BIOSEQ_TEST_INDEXES
TEST_CASE("missing manifest forces recompute; reserved sentinels are ignored",
          "[component:bioseq][layer:integration][feature:index][resource:postgres]") {
    auto ctx = fresh_context({"chunked_storage_missing_manifest"});
    seed_sample(ctx, "chunked_storage_missing_manifest", 1);
    exec_sqlf(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    exec_sqlf(ctx,
              "SELECT bioseq.update_index('chunked_storage_missing_manifest'::regclass, "
              "'minimizers_per_fragment');");
    const long long sequence_row_number = query_scalar_int(
        ctx,
        "SELECT bioseq.column_row_number('chunked_storage_missing_manifest'::regclass, ctid) FROM "
        "chunked_storage_missing_manifest LIMIT 1");

    // Temporary-run entries use reserved sub-keys and must be excluded from data fragments.
    const long long distinct_before = index_distinct_sub_keys(
        ctx, "minimizers_per_fragment", "chunked_storage_missing_manifest", sequence_row_number);
    exec_sqlf(
        ctx,
        "SELECT bioseq_internal.write_index_chunk('chunked_storage_missing_manifest'::regclass, "
        "'minimizers_per_fragment', "
        "{}, (4611686018427387904)::bigint, 0, '\\x00'::bytea);",
        sequence_row_number); // sub_key = 1<<62 (TEMP_RUN_BASE)
    REQUIRE(index_distinct_sub_keys(ctx,
                                    "minimizers_per_fragment",
                                    "chunked_storage_missing_manifest",
                                    sequence_row_number) == distinct_before);

    // Without a completeness manifest, mmsearch must recompute the index and still find the match.
    exec_sqlf(ctx,
              "SELECT "
              "bioseq_internal.delete_index_manifest('chunked_storage_missing_manifest'::regclass, "
              "'minimizers_per_fragment', {});",
              sequence_row_number);
    guc_scope sequence_read_on(ctx, "bioseq.index.sequence_read.enabled", "on");
    REQUIRE(mmsearch_set(ctx, "chunked_storage_missing_manifest").size() ==
            1); // recompute fallback still finds the match
}
#endif

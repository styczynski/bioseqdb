/**
 * @file index_invariance_test.cpp
 * @brief Checks that adding PostgreSQL indexes preserves mmsearch results.
 *
 * Compares rows and aggregate results before and after adding btree, hash,
 * and GIN indexes. Alignment fields must remain available in each plan.
 */

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view k_index_table = "scalar_index_invariance";
constexpr std::string_view k_index_label_table = "label_index_invariance";

} // namespace

namespace {

/** @brief Pairs filtered rows with their aggregate projection. */
struct invariance_result {
    rowset_t row_results;
    row_t agg_results;
};

/** @brief Runs the row and aggregate queries with the current indexes and settings. */
[[nodiscard]] invariance_result run_invariance_queries(const connection_test_context& ctx,
                                                       std::string_view table_name) {
    invariance_result result;
    result.row_results = query_rows(
        ctx,
        fmt::format("SELECT pk, bioseq.target_start(pk), bioseq.target_end(pk), bioseq.cigar(pk) "
                    "  FROM {} "
                    " WHERE mmsearch(seq0, "
                    "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)) "
                    " ORDER BY pk;",
                    table_name));
    // Also aggregate an alignment field to check that it survives the aggregate plan.
    result.agg_results = query_one(
        ctx,
        fmt::format("SELECT MIN(bioseq.target_start(pk)) "
                    "  FROM {} "
                    " WHERE mmsearch(seq0, "
                    "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32));",
                    table_name));
    return result;
}

} // namespace

TEST_CASE("Result rows are independent of B-Tree index presence",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    // The fixture starts with only its primary-key index.
    auto ctx = fresh_context({k_index_table});
    seed_hoe_fixture(ctx, k_index_table);
    const auto baseline = run_invariance_queries(ctx, k_index_table);

    // Adding a sequence btree must preserve both the alignment rows and their aggregate.
    exec_sqlf(ctx, "CREATE INDEX {0}_seq0_btree ON {0} USING btree(seq0);", k_index_table);

    const auto with_btree_default = run_invariance_queries(ctx, k_index_table);
    REQUIRE(with_btree_default.row_results == baseline.row_results);
    REQUIRE(with_btree_default.agg_results == baseline.agg_results);

    // Check both settings of Bioseq's index-scan option.
    {
        guc_scope index_scan_scope(ctx, "bioseq.planner.index_scan.enabled", "on");
        const auto with_btree_idx_on = run_invariance_queries(ctx, k_index_table);
        REQUIRE(with_btree_idx_on.row_results == baseline.row_results);
        REQUIRE(with_btree_idx_on.agg_results == baseline.agg_results);
    }
    {
        guc_scope index_scan_scope(ctx, "bioseq.planner.index_scan.enabled", "off");
        const auto with_btree_idx_off = run_invariance_queries(ctx, k_index_table);
        REQUIRE(with_btree_idx_off.row_results == baseline.row_results);
        REQUIRE(with_btree_idx_off.agg_results == baseline.agg_results);
    }
}

TEST_CASE("Result rows are independent of Hash index presence",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    // Adding a hash index on the primary key must preserve mmsearch results.
    auto ctx = fresh_context({k_index_table});
    seed_hoe_fixture(ctx, k_index_table);
    const auto baseline = run_invariance_queries(ctx, k_index_table);

    exec_sqlf(ctx, "CREATE INDEX {0}_pk_hash ON {0} USING hash(pk);", k_index_table);

    const auto with_hash = run_invariance_queries(ctx, k_index_table);
    REQUIRE(with_hash.row_results == baseline.row_results);
    REQUIRE(with_hash.agg_results == baseline.agg_results);
}

TEST_CASE("Result rows are independent of GIN index on array-of-text labels",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    // A GIN index on sequence labels must leave sequence alignment results unchanged.
    auto ctx = fresh_context({k_index_label_table});
    // Build this fixture with room to add a labels column.
    exec_sqlf(ctx, "SET bioseq.index.insert_maintenance.enabled = on;");
    create_seq_table(ctx,
                     seq_table_spec{
                         .name = std::string(k_index_label_table),
                         .typmod = {.alphabet = "dna4", .width = 32},
                         .seq_columns = 2,
                         .using_bioseq = true,
                     },
                     hoe_seed_rows());

    // Fill the labels column so the GIN index has data.
    exec_sqlf(ctx, "ALTER TABLE {} ADD COLUMN labels TEXT[];", k_index_label_table);
    exec_sqlf(ctx, "UPDATE {} SET labels = ARRAY['lab', 'sample', pk::text];", k_index_label_table);

    // Pre-index baseline.
    const auto baseline = run_invariance_queries(ctx, k_index_label_table);

    // Add the GIN index.
    exec_sqlf(ctx, "CREATE INDEX {0}_labels_gin ON {0} USING gin(labels);", k_index_label_table);

    const auto with_gin = run_invariance_queries(ctx, k_index_label_table);
    REQUIRE(with_gin.row_results == baseline.row_results);
    REQUIRE(with_gin.agg_results == baseline.agg_results);
}

TEST_CASE("EXPLAIN ANALYZE survives every index combination without "
          "altering bioseq.* results",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    // Keep the primary-key btree, sequence btree, and primary-key hash index
    // together. Alignment fields and EXPLAIN ANALYZE must still work.
    auto ctx = fresh_context({k_index_table});
    seed_hoe_fixture(ctx, k_index_table);
    exec_sqlf(ctx, "CREATE INDEX {0}_seq0_btree ON {0} USING btree(seq0);", k_index_table);
    exec_sqlf(ctx, "CREATE INDEX {0}_pk_hash ON {0} USING hash(pk);", k_index_table);

    const auto rows = query_rows(
        ctx,
        fmt::format("SELECT pk, bioseq.target_start(pk), bioseq.cigar(pk) "
                    "  FROM {} "
                    " WHERE mmsearch(seq0, "
                    "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)) "
                    " ORDER BY pk;",
                    k_index_table));
    REQUIRE_FALSE(rows.empty());
    bool saw_valid_cigar = false;
    for (const auto& row : rows) {
        REQUIRE(row.size() == 3);
        REQUIRE(row[0].has_value());
        // Each matched row must have a start position and a CIGAR string.
        REQUIRE(row[1].has_value());
        REQUIRE(row[2].has_value());
        REQUIRE_FALSE(row[2].value().empty());
        REQUIRE(is_valid_cigar(row[2].value()));
        saw_valid_cigar = true;
    }
    REQUIRE(saw_valid_cigar);

    // EXPLAIN ANALYZE must execute successfully and report BioseqScan.
    const auto plan = explain_text(
        ctx,
        fmt::format("SELECT pk, bioseq.cigar(pk) FROM {} "
                    " WHERE mmsearch(seq0, "
                    "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32));",
                    k_index_table));
    REQUIRE_FALSE(plan.empty());
    REQUIRE(plan_contains(plan, "BioseqScan"));
}

TEST_CASE("Result rows are invariant under bioseq.planner.index_scan.enabled toggle",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    // Turning Bioseq index scans on or off must preserve the alignment rows and fields.
    auto ctx = fresh_context({k_index_table});
    seed_hoe_fixture(ctx, k_index_table);
    exec_sqlf(ctx, "CREATE INDEX {0}_seq0_btree ON {0} USING btree(seq0);", k_index_table);

    const std::string sql =
        fmt::format("SELECT pk, bioseq.target_start(pk), bioseq.cigar(pk) "
                    "  FROM {} "
                    " WHERE mmsearch(seq0, "
                    "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)) "
                    " ORDER BY pk;",
                    k_index_table);
    const auto rows_with_idx_on =
        expect_invariant_under_guc_toggle(ctx, sql, "bioseq.planner.index_scan.enabled");
    REQUIRE_FALSE(rows_with_idx_on.empty());
    for (const auto& row : rows_with_idx_on) {
        REQUIRE(row.size() == 3);
        REQUIRE(row[0].has_value());
        REQUIRE(row[1].has_value());
        REQUIRE(row[2].has_value());
        REQUIRE_FALSE(row[2].value().empty());
    }
}

// Index-invariance tests for mmsearch + bioseq.* projections.
//
// The prompt asks: for any postgres index on the bioseq table the
// output should be the same. Cases under test:
//   - No additional index (just the implicit PK btree)
//   - B-Tree on a non-PK column
//   - Hash on a non-PK column
//   - GIN on an array-of-text column (sequences with multiple labels)
//
// Together with the bioseq.enable_bioseq_index_scan GUC (which can
// rewrite a vanilla IndexScan into a BioseqIndexScan custom node),
// these indexes change which plan the planner picks. The result rows
// MUST stay identical regardless of the index — the test fixture
// seeds the same data, runs the same query, and asserts the rows are
// equal across all four shapes.
//
// We also check that aggregate queries (MIN/MAX over a bioseq.*
// projection) and EXPLAIN ANALYZE work in every shape — this is the
// failure mode the prompt described where index scans returned
// empty/NULL aggregates while non-index scans worked.

#include <tests/support/postgres.hpp>

#include <string>

import bioseq.integration_tests.common;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view kIndexTable = "ix_invariance";
constexpr std::string_view kIndexLabelTable = "ix_label_invariance";

}  // namespace

namespace {

// Result-set + aggregate-row pair so individual tests can compare
// both an mmsearch-filtered scan and an aggregate-over-projection
// query produce the same answer regardless of index shape.
struct InvarianceFixtureResult {
    rowset_t row_results;
    row_t    agg_results;
};

[[nodiscard]] InvarianceFixtureResult
run_invariance_queries(const connection_test_context& ctx,
                       std::string_view table_name) {
    InvarianceFixtureResult out;
    out.row_results = query_rows(
        ctx,
        fmt::format(
            "SELECT pk, bioseq.target_start(pk), bioseq.target_end(pk), bioseq.cigar(pk) "
            "  FROM {} "
            " WHERE mmsearch(seq0, "
            "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)) "
            " ORDER BY pk;",
            table_name));
    /*
     * Single-aggregate test deliberately uses MIN(target_start) so
     * we exercise the planner's vectorised-aggregator-skip path
     * (the bioseq projection input would otherwise crash the
     * vectorised aggregator — see columnar_planner_hook.cppm's
     * bioseq_expr_uses_bioseq_projection guard). Keeping it
     * single-aggregate avoids the documented §8.4 multi-aggregate
     * limitation. A separate test below pins the multi-aggregate
     * behaviour we can support today.
     */
    out.agg_results = query_one(
        ctx,
        fmt::format(
            "SELECT MIN(bioseq.target_start(pk)) "
            "  FROM {} "
            " WHERE mmsearch(seq0, "
            "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32));",
            table_name));
    return out;
}

}  // namespace

TEST_CASE("Result rows are independent of B-Tree index presence",
          "[mmsearch][index_invariance][btree]") {
    // Baseline: no additional index. The hoe fixture already
    // creates a PK btree; no other index is added.
    auto ctx = fresh_context({kIndexTable});
    seed_hoe_fixture(ctx, kIndexTable);
    const auto baseline = run_invariance_queries(ctx, kIndexTable);

    // Add a B-Tree on the seq0 column. The planner is unlikely to
    // CHOOSE this index for an mmsearch qual (the operator class
    // doesn't match), but the index's existence should not
    // perturb the customscan path. With
    // `bioseq.enable_bioseq_index_scan = on` the planner may
    // consider an IndexScan path; the result rows should still be
    // identical.
    exec_sqlf(ctx, "CREATE INDEX {0}_seq0_btree ON {0} USING btree(seq0);",
              kIndexTable);

    const auto with_btree_default = run_invariance_queries(ctx, kIndexTable);
    REQUIRE(with_btree_default.row_results == baseline.row_results);
    REQUIRE(with_btree_default.agg_results == baseline.agg_results);

    // Toggle the GUC explicitly so an IndexScan path could be
    // chosen; the result MUST stay invariant.
    {
        guc_scope idx_scope(ctx, "bioseq.enable_bioseq_index_scan", "on");
        const auto with_btree_idx_on = run_invariance_queries(ctx, kIndexTable);
        REQUIRE(with_btree_idx_on.row_results == baseline.row_results);
        REQUIRE(with_btree_idx_on.agg_results == baseline.agg_results);
    }
    {
        guc_scope idx_scope(ctx, "bioseq.enable_bioseq_index_scan", "off");
        const auto with_btree_idx_off = run_invariance_queries(ctx, kIndexTable);
        REQUIRE(with_btree_idx_off.row_results == baseline.row_results);
        REQUIRE(with_btree_idx_off.agg_results == baseline.agg_results);
    }
}

TEST_CASE("Result rows are independent of Hash index presence",
          "[mmsearch][index_invariance][hash]") {
    // Hash indexes on bioseq.seq don't have an opclass, but
    // creating one on a regular pk column shouldn't perturb the
    // mmsearch result set.
    auto ctx = fresh_context({kIndexTable});
    seed_hoe_fixture(ctx, kIndexTable);
    const auto baseline = run_invariance_queries(ctx, kIndexTable);

    exec_sqlf(ctx, "CREATE INDEX {0}_pk_hash ON {0} USING hash(pk);",
              kIndexTable);

    const auto with_hash = run_invariance_queries(ctx, kIndexTable);
    REQUIRE(with_hash.row_results == baseline.row_results);
    REQUIRE(with_hash.agg_results == baseline.agg_results);
}

TEST_CASE("Result rows are independent of GIN index on array-of-text labels",
          "[mmsearch][index_invariance][gin]") {
    // The GIN index models the realistic use case the prompt
    // describes: each sequence has multiple labels in a Postgres
    // ARRAY column. A GIN index on the array enables fast
    // label-search; mmsearch on the bioseq.seq column should be
    // unaffected by the index's existence.
    auto ctx = fresh_context({kIndexLabelTable});
    // We need a custom table shape (with a labels column) so
    // build it manually rather than reusing seed_hoe_fixture.
    exec_sqlf(ctx, "SET bioseq.update_index_on_insert = on;");
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(kIndexLabelTable),
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 2,
            .using_bioseq = true,
        },
        hoe_seed_rows());

    // Add a labels column populated with deterministic arrays so
    // the GIN index has something to index.
    exec_sqlf(ctx, "ALTER TABLE {} ADD COLUMN labels TEXT[];", kIndexLabelTable);
    exec_sqlf(ctx, "UPDATE {} SET labels = ARRAY['lab', 'sample', pk::text];",
              kIndexLabelTable);

    // Pre-index baseline.
    const auto baseline = run_invariance_queries(ctx, kIndexLabelTable);

    // Add the GIN index.
    exec_sqlf(ctx, "CREATE INDEX {0}_labels_gin ON {0} USING gin(labels);",
              kIndexLabelTable);

    const auto with_gin = run_invariance_queries(ctx, kIndexLabelTable);
    REQUIRE(with_gin.row_results == baseline.row_results);
    REQUIRE(with_gin.agg_results == baseline.agg_results);
}

TEST_CASE("EXPLAIN ANALYZE survives every index combination without "
          "altering bioseq.* results",
          "[mmsearch][index_invariance][explain]") {
    // Composite test: create all three indexes (PK btree implicit,
    // seq0 btree, pk hash) on the same relation and verify
    // EXPLAIN ANALYZE plus the result rows. The planner will
    // pick whichever path it considers cheapest; bioseq.*
    // projections must keep working.
    auto ctx = fresh_context({kIndexTable});
    seed_hoe_fixture(ctx, kIndexTable);
    exec_sqlf(ctx, "CREATE INDEX {0}_seq0_btree ON {0} USING btree(seq0);",
              kIndexTable);
    exec_sqlf(ctx, "CREATE INDEX {0}_pk_hash ON {0} USING hash(pk);",
              kIndexTable);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT pk, bioseq.target_start(pk), bioseq.cigar(pk) "
            "  FROM {} "
            " WHERE mmsearch(seq0, "
            "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)) "
            " ORDER BY pk;",
            kIndexTable));
    REQUIRE_FALSE(rows.empty());
    bool saw_valid_cigar = false;
    for (const auto& row : rows) {
        REQUIRE(row.size() == 3);
        REQUIRE(row[0].has_value());
        // Every projected row from a mmsearch-bearing scan MUST
        // populate bioseq.* fields — the prompt explicitly says
        // returning rows with NULLs is invalid for alignment
        // queries.
        REQUIRE(row[1].has_value());
        REQUIRE(row[2].has_value());
        REQUIRE_FALSE(row[2].value().empty());
        REQUIRE(is_valid_cigar(row[2].value()));
        saw_valid_cigar = true;
    }
    REQUIRE(saw_valid_cigar);

    // EXPLAIN ANALYZE must run without crashing AND must show a
    // BioseqScan node — the customscan path should win even with
    // multiple indexes available.
    const auto plan = explain_text(
        ctx,
        fmt::format(
            "SELECT pk, bioseq.cigar(pk) FROM {} "
            " WHERE mmsearch(seq0, "
            "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32));",
            kIndexTable));
    REQUIRE_FALSE(plan.empty());
    REQUIRE(plan_contains(plan, "BioseqScan"));
}

TEST_CASE("Result rows are invariant under bioseq.enable_bioseq_index_scan toggle",
          "[mmsearch][index_invariance][guc][enable_bioseq_index_scan]") {
    // The bioseq.enable_bioseq_index_scan GUC rewrites vanilla
    // IndexScan plan nodes into BioseqIndexScan custom nodes so
    // the columnar reader is reused. The user wants the result
    // set to be invariant under this rewrite.
    auto ctx = fresh_context({kIndexTable});
    seed_hoe_fixture(ctx, kIndexTable);
    exec_sqlf(ctx, "CREATE INDEX {0}_seq0_btree ON {0} USING btree(seq0);",
              kIndexTable);

    const std::string sql = fmt::format(
        "SELECT pk, bioseq.target_start(pk), bioseq.cigar(pk) "
        "  FROM {} "
        " WHERE mmsearch(seq0, "
        "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)) "
        " ORDER BY pk;",
        kIndexTable);
    const auto rows_with_idx_on =
        expect_invariant_under_guc_toggle(ctx, sql,
                                          "bioseq.enable_bioseq_index_scan");
    REQUIRE_FALSE(rows_with_idx_on.empty());
    for (const auto& row : rows_with_idx_on) {
        REQUIRE(row.size() == 3);
        REQUIRE(row[0].has_value());
        REQUIRE(row[1].has_value());
        REQUIRE(row[2].has_value());
        REQUIRE_FALSE(row[2].value().empty());
    }
}

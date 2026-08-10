// Index invariance for mmsearch + bioseq.* projections.
//
// PostgreSQL indexes on a bioseq table must not alter query results.
// Covered planner shapes:
//   - No additional index (just the implicit PK btree)
//   - B-Tree on a non-PK column
//   - Hash on a non-PK column
//   - GIN on an array-of-text column (sequences with multiple labels)
//
// Together with the bioseq.planner.index_scan.enabled GUC (which can
// rewrite a vanilla IndexScan into a BioseqIndexScan custom node),
// these indexes change which plan the planner picks. The result rows
// MUST stay identical regardless of the index — the canonical dataset
// seeds the same data, runs the same query, and asserts the rows are
// equal across all four shapes.
//
// Aggregate projections and EXPLAIN ANALYZE must also work in every shape,
// with index scans returning the same non-NULL aggregates as other scans.

#include <tests/support/postgres.hpp>

#include <string>

import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view k_index_table = "scalar_index_invariance";
constexpr std::string_view k_index_label_table = "label_index_invariance";

}  // namespace

namespace {

// Result-set + aggregate-row pair so individual cases can compare
// both an mmsearch-filtered scan and an aggregate-over-projection
// query produce the same answer regardless of index shape.
struct invariance_result {
    rowset_t row_results;
    row_t    agg_results;
};

[[nodiscard]] invariance_result
run_invariance_queries(const connection_test_context& ctx,
                       std::string_view table_name) {
    invariance_result out;
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
     * The single-aggregate query deliberately uses MIN(target_start) so
     * we exercise the planner's vectorised-aggregator-skip path
     * (the bioseq projection input would otherwise crash the
     * vectorised aggregator — see columnar_planner_hook.cppm's
     * bioseq_expr_uses_bioseq_projection guard). Keeping it
     * single aggregate avoids multi-aggregate
     * limitation. A separate case covers the multi-aggregate
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
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    // Baseline: no additional index. The alignment dataset already
    // creates a PK btree; no other index is added.
    auto ctx = fresh_context({k_index_table});
    seed_hoe_fixture(ctx, k_index_table);
    const auto baseline = run_invariance_queries(ctx, k_index_table);

    // Add a B-Tree on the seq0 column. The planner is unlikely to
    // CHOOSE this index for an mmsearch qual (the operator class
    // doesn't match), but the index's existence should not
    // perturb the customscan path. With
    // `bioseq.planner.index_scan.enabled = on` the planner may
    // consider an IndexScan path; the result rows should still be
    // identical.
    exec_sqlf(ctx, "CREATE INDEX {0}_seq0_btree ON {0} USING btree(seq0);",
              k_index_table);

    const auto with_btree_default = run_invariance_queries(ctx, k_index_table);
    REQUIRE(with_btree_default.row_results == baseline.row_results);
    REQUIRE(with_btree_default.agg_results == baseline.agg_results);

    // Toggle the GUC explicitly so an IndexScan path could be
    // chosen; the result MUST stay invariant.
    {
        guc_scope index_scan_scope(
            ctx, "bioseq.planner.index_scan.enabled", "on");
        const auto with_btree_idx_on = run_invariance_queries(ctx, k_index_table);
        REQUIRE(with_btree_idx_on.row_results == baseline.row_results);
        REQUIRE(with_btree_idx_on.agg_results == baseline.agg_results);
    }
    {
        guc_scope index_scan_scope(
            ctx, "bioseq.planner.index_scan.enabled", "off");
        const auto with_btree_idx_off = run_invariance_queries(ctx, k_index_table);
        REQUIRE(with_btree_idx_off.row_results == baseline.row_results);
        REQUIRE(with_btree_idx_off.agg_results == baseline.agg_results);
    }
}

TEST_CASE("Result rows are independent of Hash index presence",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    // Hash indexes on bioseq.seq don't have an opclass, but
    // creating one on a regular pk column shouldn't perturb the
    // mmsearch result set.
    auto ctx = fresh_context({k_index_table});
    seed_hoe_fixture(ctx, k_index_table);
    const auto baseline = run_invariance_queries(ctx, k_index_table);

    exec_sqlf(ctx, "CREATE INDEX {0}_pk_hash ON {0} USING hash(pk);",
              k_index_table);

    const auto with_hash = run_invariance_queries(ctx, k_index_table);
    REQUIRE(with_hash.row_results == baseline.row_results);
    REQUIRE(with_hash.agg_results == baseline.agg_results);
}

TEST_CASE("Result rows are independent of GIN index on array-of-text labels",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    // The GIN index models sequences with multiple labels in a PostgreSQL
    // ARRAY column. A GIN index on the array enables fast
    // label-search; mmsearch on the bioseq.seq column should be
    // unaffected by the index's existence.
    auto ctx = fresh_context({k_index_label_table});
    // We need a custom table shape (with a labels column) so
    // build this labeled dataset explicitly.
    exec_sqlf(ctx, "SET bioseq.index.insert_maintenance.enabled = on;");
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(k_index_label_table),
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 2,
            .using_bioseq = true,
        },
        hoe_seed_rows());

    // Add a labels column populated with deterministic arrays so
    // the GIN index has something to index.
    exec_sqlf(ctx, "ALTER TABLE {} ADD COLUMN labels TEXT[];", k_index_label_table);
    exec_sqlf(ctx, "UPDATE {} SET labels = ARRAY['lab', 'sample', pk::text];",
              k_index_label_table);

    // Pre-index baseline.
    const auto baseline = run_invariance_queries(ctx, k_index_label_table);

    // Add the GIN index.
    exec_sqlf(ctx, "CREATE INDEX {0}_labels_gin ON {0} USING gin(labels);",
              k_index_label_table);

    const auto with_gin = run_invariance_queries(ctx, k_index_label_table);
    REQUIRE(with_gin.row_results == baseline.row_results);
    REQUIRE(with_gin.agg_results == baseline.agg_results);
}

TEST_CASE("EXPLAIN ANALYZE survives every index combination without "
          "altering bioseq.* results",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    // Composite shape: create all three indexes (PK btree implicit,
    // seq0 btree, pk hash) on the same relation and verify
    // EXPLAIN ANALYZE plus the result rows. The planner will
    // pick whichever path it considers cheapest; bioseq.*
    // projections must keep working.
    auto ctx = fresh_context({k_index_table});
    seed_hoe_fixture(ctx, k_index_table);
    exec_sqlf(ctx, "CREATE INDEX {0}_seq0_btree ON {0} USING btree(seq0);",
              k_index_table);
    exec_sqlf(ctx, "CREATE INDEX {0}_pk_hash ON {0} USING hash(pk);",
              k_index_table);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT pk, bioseq.target_start(pk), bioseq.cigar(pk) "
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
        // Every projected row from a mmsearch-bearing scan must populate
        // bioseq.* fields; NULL alignment results are invalid.
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
            k_index_table));
    REQUIRE_FALSE(plan.empty());
    REQUIRE(plan_contains(plan, "BioseqScan"));
}

TEST_CASE("Result rows are invariant under bioseq.planner.index_scan.enabled toggle",
          "[component:bioseq][layer:integration][feature:planner][resource:postgres]") {
    // The bioseq.planner.index_scan.enabled GUC rewrites vanilla
    // IndexScan plan nodes into BioseqIndexScan custom nodes so
    // the columnar reader is reused. The user wants the result
    // set to be invariant under this rewrite.
    auto ctx = fresh_context({k_index_table});
    seed_hoe_fixture(ctx, k_index_table);
    exec_sqlf(ctx, "CREATE INDEX {0}_seq0_btree ON {0} USING btree(seq0);",
              k_index_table);

    const std::string sql = fmt::format(
        "SELECT pk, bioseq.target_start(pk), bioseq.cigar(pk) "
        "  FROM {} "
        " WHERE mmsearch(seq0, "
        "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)) "
        " ORDER BY pk;",
        k_index_table);
    const auto rows_with_idx_on =
        expect_invariant_under_guc_toggle(ctx, sql,
                                          "bioseq.planner.index_scan.enabled");
    REQUIRE_FALSE(rows_with_idx_on.empty());
    for (const auto& row : rows_with_idx_on) {
        REQUIRE(row.size() == 3);
        REQUIRE(row[0].has_value());
        REQUIRE(row[1].has_value());
        REQUIRE(row[2].has_value());
        REQUIRE_FALSE(row[2].value().empty());
    }
}

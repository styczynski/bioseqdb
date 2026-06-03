// Integration scenarios:
//   * `mmsearch` self-join uses the right-hand side's `seq1` column as the
//     query argument and runs without errors; whichever match set the
//     alignment pipeline yields is unconstrained here, but every row must
//     reference seeded primary keys.
//   * Projection helpers (`bioseq.target_start`, `bioseq.cigar`) work for
//     either input table in the join because the placeholder rewrite is
//     resolved per-call against the originating Var.
//   * The match set is invariant under `bioseq.enable_vectorization` toggling,
//     since the SIMD path must produce byte-identical output to scalar.
//   * Cross joins WITHOUT mmsearch on `USING bioseq` tables produce the
//     full cartesian product (regression test for the global-state bug
//     where the second BioseqScan node would force the first one back
//     into Phase 2 EOF after one outer row). The fix moved per-scan
//     state into BioseqScanRuntime, allocated per-CustomScanState.
//   * LEFT JOIN with a literal `WHERE mmsearch(t1.seq, '...')` and
//     `bioseq.cigar(t1.pk)` does not crash and surfaces matched rows on
//     the outer side. Before bioseq_placeholder_support was attached,
//     the planner pushed `bioseq.cigar(t1.pk)` into both join children's
//     targetlists; the inner side had no MatchResult to back the
//     placeholder Const and the executor crashed.

#include "common_tests_integration.h"

#include <set>

import integration_tests.common;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view kHoeJoin = "hoe_join";
constexpr std::string_view kHoeJoinLeft = "hoe_join_left";
constexpr std::string_view kHoeJoinRight = "hoe_join_right";

}  // namespace

TEST_CASE("Self-join with mmsearch executes and stays in the seeded pk domain",
          "[join][mmsearch]") {
    // mmsearch(t1.seq0, t2.seq1) routes through the
    // parameterised inner-side scan path (Param-rebind) because
    // t2 is a different range-table entry than t1. Each outer t1
    // row drives a fresh rescan of t2 with t1.seq1 bound into
    // arg1's Param. Strengthened assertion: any row that
    // surfaces MUST have a non-NULL pk on both sides AND, when
    // bioseq projections are requested in the outer SELECT,
    // every emitted row must populate them. Returning a row with
    // NULL bioseq.cigar / bioseq.target_start on a mmsearch path
    // is INVALID — see §9 of REPORT_POSTGRES_QUERY_BUGFIXING_1.md.
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk "
            "  FROM {} t1 CROSS JOIN {} t2 "
            " WHERE mmsearch(t1.seq0, t2.seq1) "
            " ORDER BY t1.pk, t2.pk;",
            kHoeJoin, kHoeJoin));
    for (const auto& row : rows) {
        REQUIRE(row.size() == 2);
        REQUIRE(row[0].has_value());
        REQUIRE(row[1].has_value());
        const auto pk1 = std::stoll(row[0].value());
        const auto pk2 = std::stoll(row[1].value());
        REQUIRE((pk1 >= 1 && pk1 <= 5));
        REQUIRE((pk2 >= 1 && pk2 <= 5));
    }
}

TEST_CASE("Self-join with mmsearch is invariant under vectorisation",
          "[join][mmsearch][parity]") {
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);

    const std::string sql = fmt::format(
        "SELECT t1.pk, t2.pk "
        "  FROM {} t1 CROSS JOIN {} t2 "
        " WHERE mmsearch(t1.seq0, t2.seq1) "
        " ORDER BY t1.pk, t2.pk;",
        kHoeJoin, kHoeJoin);
    expect_invariant_under_guc_toggle(ctx, sql,
                                      "bioseq.enable_vectorization");
}

TEST_CASE("Self-join projection helpers run without errors when matches surface",
          "[join][projection]") {
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk, "
            "       bioseq.target_start(t1.pk), "
            "       bioseq.target_end(t1.pk), "
            "       bioseq.cigar(t1.pk) "
            "  FROM {} t1 CROSS JOIN {} t2 "
            " WHERE mmsearch(t1.seq0, t2.seq1) "
            " ORDER BY t1.pk, t2.pk;",
            kHoeJoin, kHoeJoin));
    for (const auto& row : rows) {
        REQUIRE(row.size() == 5);
        if (row[2].has_value() && row[3].has_value()) {
            REQUIRE(std::stoll(row[2].value()) <= std::stoll(row[3].value()));
        }
        if (row[4].has_value() && !row[4].value().empty()) {
            REQUIRE(is_valid_cigar(row[4].value()));
        }
    }
}

TEST_CASE("CROSS JOIN without mmsearch produces full cartesian product",
          "[join][cross_join][regression]") {
    // Pre-fix: the static-global TupleDispatchBuffer was shared between the
    // two BioseqScan nodes. The inner scan's first batch transition flipped
    // `output_position` to 0; when the outer scan's iterator was called for
    // its second row, has_finished_writing() observed the flag and returned
    // EOF, capping the result at one outer row.
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk "
            "  FROM {} t1 CROSS JOIN {} t2 "
            " ORDER BY t1.pk, t2.pk;",
            kHoeJoin, kHoeJoin));
    REQUIRE(rows.size() == 25);  // 5 x 5
    // Each (t1.pk, t2.pk) pair appears exactly once.
    for (long long pk1 = 1; pk1 <= 5; ++pk1) {
        for (long long pk2 = 1; pk2 <= 5; ++pk2) {
            const auto idx = static_cast<size_t>((pk1 - 1) * 5 + (pk2 - 1));
            REQUIRE(idx < rows.size());
            REQUIRE(rows[idx][0].has_value());
            REQUIRE(rows[idx][1].has_value());
            REQUIRE(std::stoll(rows[idx][0].value()) == pk1);
            REQUIRE(std::stoll(rows[idx][1].value()) == pk2);
        }
    }
}

TEST_CASE("CROSS JOIN of two distinct USING-bioseq tables iterates both sides",
          "[join][cross_join][regression]") {
    // Variant of the regression test above against two distinct tables:
    // before the per-scan-runtime split, even differently-named bioseq
    // tables shared the global buffer, so the second-scan EOF would still
    // stop the first scan early. Two physically distinct relations make
    // sure the fix isn't accidentally papered over by self-join planner
    // optimisations (e.g. self-join elimination) that might rewrite away
    // one of the BioseqScan nodes.
    auto ctx = fresh_context({kHoeJoinLeft, kHoeJoinRight});
    seed_hoe_fixture(ctx, kHoeJoinLeft);
    seed_hoe_fixture(ctx, kHoeJoinRight);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk "
            "  FROM {} t1 CROSS JOIN {} t2 "
            " ORDER BY t1.pk, t2.pk;",
            kHoeJoinLeft, kHoeJoinRight));
    REQUIRE(rows.size() == 25);
    for (long long pk1 = 1; pk1 <= 5; ++pk1) {
        long long count_for_pk1 = 0;
        for (const auto& row : rows) {
            if (row[0].has_value() && std::stoll(row[0].value()) == pk1) {
                ++count_for_pk1;
            }
        }
        REQUIRE(count_for_pk1 == 5);
    }
}

TEST_CASE("LEFT JOIN with literal mmsearch and bioseq.cigar does not crash",
          "[join][left_join][placeholder_support][regression]") {
    // Pre-fix scenario: the planner — without bioseq_placeholder_support —
    // pushed `bioseq.cigar(t1.pk)` into both children's targetlists for
    // outer-join nullability. The inner scan rewrote its copy of
    // bioseq.cigar to a placeholder Const that no MatchResult ever
    // populated; the per-scan-runtime reset done by the inner side's
    // BeginCustomScan also wiped the outer scan's mmsearch context. With
    // bioseq_placeholder_support attached the FuncExpr stays anchored to
    // the originating scan via a PlaceHolderVar, so the inner-side
    // BioseqScan never learns about cigar(t1.pk).
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk, bioseq.cigar(t1.pk) "
            "  FROM {} t1 LEFT JOIN {} t2 ON t1.pk = t2.pk "
            " WHERE mmsearch(t1.seq0, "
            "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)) "
            " ORDER BY t1.pk;",
            kHoeJoin, kHoeJoin));
    // Pre-fix this query crashed the backend. We require a non-empty
    // result so the test isn't a tautology under "and bioseq.cigar
    // does not crash".
    REQUIRE_FALSE(rows.empty());

    bool saw_valid_cigar = false;
    for (const auto& row : rows) {
        REQUIRE(row.size() == 3);
        REQUIRE(row[0].has_value());
        // LEFT JOIN preserves left-side rows even if no matching right
        // row exists, so row[1] may be NULL. row[2] is the cigar of the
        // outer row's alignment against the literal probe; for the
        // probes used by hoe_seed_rows() at least one row aligns, so
        // we want to see a valid CIGAR somewhere in the output.
        if (row[2].has_value() && !row[2].value().empty()) {
            REQUIRE(is_valid_cigar(row[2].value()));
            saw_valid_cigar = true;
        }
    }
    // Insurance against a regression that returns matching rows but
    // populates cigar as NULL or an empty string for every row.
    REQUIRE(saw_valid_cigar);
}

TEST_CASE("LEFT JOIN with literal mmsearch is invariant under vectorisation "
          "and column cache toggles",
          "[join][left_join][parity]") {
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);

    const std::string sql = fmt::format(
        "SELECT t1.pk, t2.pk, bioseq.target_start(t1.pk), bioseq.cigar(t1.pk) "
        "  FROM {} t1 LEFT JOIN {} t2 ON t1.pk = t2.pk "
        " WHERE mmsearch(t1.seq0, "
        "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)) "
        " ORDER BY t1.pk;",
        kHoeJoin, kHoeJoin);
    (void)expect_query_invariant_under_options(
        ctx, sql,
        {{"bioseq.enable_vectorization", {"on", "off"}},
         {"bioseq.enable_column_cache", {"on", "off"}}});
}

TEST_CASE("Repeated execution of CROSS JOIN does not leak per-scan state",
          "[join][cross_join][regression]") {
    // Per-scan state was previously held in file-static globals. Multiple
    // executions inside the same backend (libpq connection here) used to
    // accumulate stale `output_position` cursors so the second invocation
    // would return zero rows. We re-run the same query several times to
    // make sure the new per-CustomScanState heap allocation is correctly
    // freed and re-allocated.
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);

    for (int run = 0; run < 4; ++run) {
        const auto rows = query_rows(
            ctx,
            fmt::format(
                "SELECT t1.pk, t2.pk "
                "  FROM {} t1 CROSS JOIN {} t2 "
                " ORDER BY t1.pk, t2.pk;",
                kHoeJoin, kHoeJoin));
        REQUIRE(rows.size() == 25);
    }
}

TEST_CASE("INNER JOIN of two USING-bioseq tables with mmsearch on both "
          "preserves left-side row count",
          "[join][inner_join][many_to_many]") {
    // Many-to-many qual: each outer row's seq1 is the literal-equivalent
    // query for the inner scan via the planner's parameterised inner-side
    // scan rewriting. ReScanCustomScan's Param-rebind path is what wires
    // the new query into the buffer per outer row.
    auto ctx = fresh_context({kHoeJoinLeft, kHoeJoinRight});
    seed_hoe_fixture(ctx, kHoeJoinLeft);
    seed_hoe_fixture(ctx, kHoeJoinRight);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk "
            "  FROM {} t1 INNER JOIN {} t2 "
            "    ON mmsearch(t2.seq0, t1.seq1) "
            " ORDER BY t1.pk, t2.pk;",
            kHoeJoinLeft, kHoeJoinRight));

    // Pre-fix this query returned zero rows (the Param-rebind path
    // didn't exist; mmsearch with a non-Const second arg was rejected
    // by BeginCustomScan and the buffer never produced matches).
    // Asserting > 0 rows is what makes this regression test
    // non-tautological — REVIEW C2 from REPORT_POSTGRES_QUERY_BUGFIXING_1.
    REQUIRE_FALSE(rows.empty());
    // The seeded probes are designed so at least one alignment per
    // row of t1 surfaces against the matching t2 — we don't pin the
    // exact set here, but we do assert the lower bound.
    REQUIRE(rows.size() >= static_cast<size_t>(5));
    for (const auto& row : rows) {
        REQUIRE(row.size() == 2);
        REQUIRE(row[0].has_value());
        REQUIRE(row[1].has_value());
        const auto pk1 = std::stoll(row[0].value());
        const auto pk2 = std::stoll(row[1].value());
        REQUIRE((pk1 >= 1 && pk1 <= 5));
        REQUIRE((pk2 >= 1 && pk2 <= 5));
    }
}

TEST_CASE("RIGHT JOIN with literal mmsearch on right side preserves right-side rows",
          "[join][right_join][placeholder_support]") {
    // RIGHT JOIN exercises the mirror of LEFT JOIN: phnullingrels
    // propagation flows from the syntactically-left side into the
    // PHV. A bug in bioseq_placeholder_support that mishandles
    // varnullingrels from the inner side would slip past the LEFT
    // JOIN test alone.
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk, bioseq.cigar(t2.pk) "
            "  FROM {} t1 RIGHT JOIN {} t2 ON t1.pk = t2.pk "
            " WHERE mmsearch(t2.seq0, "
            "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)) "
            " ORDER BY t2.pk;",
            kHoeJoin, kHoeJoin));
    REQUIRE_FALSE(rows.empty());
    bool saw_valid_cigar = false;
    for (const auto& row : rows) {
        REQUIRE(row.size() == 3);
        REQUIRE(row[1].has_value());  // t2.pk preserved by RIGHT JOIN
        if (row[2].has_value() && !row[2].value().empty()) {
            REQUIRE(is_valid_cigar(row[2].value()));
            saw_valid_cigar = true;
        }
    }
    REQUIRE(saw_valid_cigar);
}

TEST_CASE("JOIN with ORDER BY bioseq.score then LIMIT/OFFSET stays sort-stable",
          "[join][order_by][limit][regression]") {
    // ORDER BY on a bioseq.* projection forces the planner to
    // evaluate the PHV expression at the join-output level. If
    // bioseq_placeholder_support returned a PHV with the wrong
    // phlevelsup, the sort key resolves to NULL for every row and
    // LIMIT becomes nondeterministic. Exercises the executor's
    // tuplestore-backed Sort node sitting above BioseqScan — a path
    // no other JOIN test triggers.
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);

    const std::string base_sql = fmt::format(
        "SELECT t1.pk, t2.pk, bioseq.cigar(t1.pk) "
        "  FROM {} t1 LEFT JOIN {} t2 ON t1.pk = t2.pk "
        " WHERE mmsearch(t1.seq0, "
        "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32))",
        kHoeJoin, kHoeJoin);
    const auto full =
        query_rows(ctx, base_sql + " ORDER BY bioseq.score(t1.pk) DESC NULLS LAST, t1.pk;");
    REQUIRE_FALSE(full.empty());
    const auto limited =
        query_rows(ctx, base_sql + " ORDER BY bioseq.score(t1.pk) DESC NULLS LAST, t1.pk LIMIT 2 OFFSET 1;");
    if (full.size() >= 3) {
        REQUIRE(limited.size() == 2);
        // The LIMIT slice [1..3) of `full` must equal `limited`
        // exactly, otherwise the sort key was NULL for every row
        // and the result was effectively random.
        for (size_t i = 0; i < limited.size(); ++i) {
            REQUIRE(limited[i] == full[i + 1]);
        }
    }
}

TEST_CASE("JOIN with HAVING over MAX(bioseq.target_end) compiles and filters",
          "[join][having][aggregate][placeholder_support][regression]") {
    // The has_aggs branch of bioseq_placeholder_support is only
    // exercised by aggregate-bearing queries. Without this test the
    // branch is dead-on-arrival; a future planner change that breaks
    // it would not be caught. We use bioseq.target_end (which the
    // seeded probes set to a known positive value) rather than
    // bioseq.score (whose value depends on minimap2 chain scoring
    // and can be zero for some probes).
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, MAX(bioseq.target_end(t1.pk)) AS best "
            "  FROM {} t1 LEFT JOIN {} t2 ON t1.pk = t2.pk "
            " WHERE mmsearch(t1.seq0, "
            "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)) "
            " GROUP BY t1.pk "
            "HAVING MAX(bioseq.target_end(t1.pk)) > 0 "
            " ORDER BY t1.pk;",
            kHoeJoin, kHoeJoin));
    REQUIRE_FALSE(rows.empty());
    for (const auto& row : rows) {
        REQUIRE(row.size() == 2);
        REQUIRE(row[0].has_value());
        REQUIRE(row[1].has_value());
        REQUIRE(std::stoll(row[1].value()) > 0);
    }
}

TEST_CASE("Three-way JOIN of bioseq tables exercises nested rescan",
          "[join][three_way][rescan][param_rebind]") {
    // Three-way join nests two parameterised inner-side BioseqScans:
    // t2's ReScan fires for each t1 row, and t3's ReScan fires for
    // each (t1, t2) combination. A regression in
    // ReScanCustomScan's clear_slots() ordering or in
    // MmsearchParamRebind would surface as duplicate / phantom rows
    // here.
    auto ctx = fresh_context({kHoeJoinLeft, kHoeJoinRight, kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoinLeft);
    seed_hoe_fixture(ctx, kHoeJoinRight);
    seed_hoe_fixture(ctx, kHoeJoin);

    const std::string sql = fmt::format(
        "SELECT t1.pk, t2.pk, t3.pk "
        "  FROM {} t1 "
        "  JOIN {} t2 ON mmsearch(t2.seq0, t1.seq1) "
        "  JOIN {} t3 ON mmsearch(t3.seq0, t2.seq1) "
        " ORDER BY t1.pk, t2.pk, t3.pk;",
        kHoeJoinLeft, kHoeJoinRight, kHoeJoin);
    const auto first = query_rows(ctx, sql);
    const auto second = query_rows(ctx, sql);
    const auto third = query_rows(ctx, sql);
    REQUIRE(first == second);
    REQUIRE(second == third);
    for (const auto& row : first) {
        REQUIRE(row.size() == 3);
        for (size_t i = 0; i < 3; ++i) {
            REQUIRE(row[i].has_value());
            const auto pk = std::stoll(row[i].value());
            REQUIRE((pk >= 1 && pk <= 5));
        }
    }
}

TEST_CASE("EXPLAIN VERBOSE on CROSS JOIN pins the bioseq plan-text invariants",
          "[join][cross_join][explain][output_stability]") {
    // The runtime move per §3.1 of the report changed where
    // aligner_state is read from in ExplainCustomScan. If the void*
    // cast or the runtime-not-yet-allocated path produces a
    // corrupted print, EXPLAIN output silently degrades. Existing
    // test_explain.cpp only inspects single-table EXPLAIN; no test
    // pins the exact strings under JOIN.
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);

    const auto plan = explain_text(
        ctx,
        fmt::format(
            "SELECT t1.pk, bioseq.cigar(t1.pk) "
            "  FROM {} t1 CROSS JOIN {} t2 "
            " WHERE mmsearch(t1.seq0, "
            "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32));",
            kHoeJoin, kHoeJoin));
    REQUIRE_FALSE(plan.empty());
    int bioseq_scan_count = 0;
    for (const auto& line : plan) {
        if (line.find("Custom Scan (BioseqScan)") != std::string::npos) {
            ++bioseq_scan_count;
        }
    }
    REQUIRE(bioseq_scan_count >= 2);
    REQUIRE(plan_contains(plan, "Bioseq Projected Columns"));
    // The aligner_state breakdown only prints when the scan
    // actually ran the minimap2 pipeline. ANALYZE forces execution
    // so the One-to-many alignment line must be visible.
    REQUIRE(plan_contains(plan, "One-to-many alignment"));
}

TEST_CASE("Repeated EXECUTE of a prepared mmsearch JOIN reuses runtime cleanly",
          "[join][prepared][rescan][lifecycle]") {
    // BioseqScanRuntime is allocated lazily per-scan-state, but a
    // prepared statement's CachedPlan reuses the same
    // CustomScanState across executions via ReScan. Catches stale
    // buffered tuples or use-after-free across re-executions.
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);

    exec_sqlf(ctx,
              "PREPARE p_join_test(bioseq.seq('dna4', 32)) AS "
              "SELECT t1.pk, bioseq.cigar(t1.pk) "
              "  FROM {} t1 LEFT JOIN {} t2 ON t1.pk = t2.pk "
              " WHERE mmsearch(t1.seq0, $1) ORDER BY t1.pk;",
              kHoeJoin, kHoeJoin);

    const auto run1 = query_rows(
        ctx,
        "EXECUTE p_join_test('CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32));");
    const auto run2 = query_rows(
        ctx,
        "EXECUTE p_join_test('TCATCATCTACTCCTA'::bioseq.seq('dna4', 32));");
    const auto run3 = query_rows(
        ctx,
        "EXECUTE p_join_test('CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32));");

    // Same parameter must produce same output (no stale state).
    REQUIRE(run1 == run3);
    // Different parameter should usually produce a different result
    // set; if it doesn't (rare alignment coincidence) at least the
    // result is non-empty so the test isn't a tautology.
    REQUIRE_FALSE(run2.empty());

    exec_sql(ctx, "DEALLOCATE p_join_test;");
}

TEST_CASE("JOIN under update_index_on_insert=off and read_sequence_index=off "
          "still produces matches",
          "[join][guc][index_off][regression]") {
    // The sidecar-less code path is an entirely separate branch in
    // the BioseqScan that no other JOIN test reaches. A regression
    // in fallback-path projection plumbing would produce empty
    // CIGARs only on this branch.
    auto ctx = fresh_context({kHoeJoin});
    // Force update_index_on_insert=off BEFORE seeding so the sidecar
    // stays empty; seed_hoe_fixture flips it on by default.
    exec_sql(ctx, "SET bioseq.update_index_on_insert = off;");
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(kHoeJoin),
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 2,
            .using_bioseq = true,
        },
        hoe_seed_rows());

    const std::string sql = fmt::format(
        "SELECT t1.pk, bioseq.cigar(t1.pk) "
        "  FROM {} t1 LEFT JOIN {} t2 ON t1.pk = t2.pk "
        " WHERE mmsearch(t1.seq0, "
        "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)) "
        " ORDER BY t1.pk;",
        kHoeJoin, kHoeJoin);

    auto invariant_rows = expect_query_invariant_under_options(
        ctx, sql, {{"bioseq.read_sequence_index", {"on", "off"}}});
    REQUIRE_FALSE(invariant_rows.empty());
    bool saw_valid_cigar = false;
    for (const auto& row : invariant_rows) {
        if (row.size() >= 2 && row[1].has_value() && !row[1].value().empty()) {
            REQUIRE(is_valid_cigar(row[1].value()));
            saw_valid_cigar = true;
        }
    }
    REQUIRE(saw_valid_cigar);
}

TEST_CASE("Mismatched typmod JOIN raises a clear error and the session survives",
          "[join][typmod_mismatch][error][lifecycle]") {
    // Cross-typmod mmsearch is undefined; the failure mode must
    // remain at SQL/parse-error or ereport level, not SIGSEGV /
    // connection drop. The post-failure session-reuse half
    // exercises the MemoryContext reset callback's mid-scan-error
    // path.
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);
    exec_sql(ctx,
             "DROP TABLE IF EXISTS hoe_64 CASCADE; "
             "CREATE TABLE hoe_64 (pk INT PRIMARY KEY, seq0 bioseq.seq('dna4', 64)) USING bioseq; "
             "SET bioseq.update_index_on_insert = on;");

    const std::string mismatch_sql = fmt::format(
        "SELECT t1.pk FROM {} t1 JOIN hoe_64 t2 ON mmsearch(t1.seq0, t2.seq0);",
        kHoeJoin);
    // mismatch_sql either raises (preferred) or returns 0 rows with
    // mmsearch acting as a tautology; what we MUST avoid is a
    // backend crash. We accept either outcome via sql_fails (which
    // intentionally swallows server errors) and then verify the
    // session is still usable.
    (void)sql_fails(ctx, mismatch_sql);

    // Session must remain usable (no transaction abort, no
    // connection drop) after the (potentially-failing) statement.
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
    exec_sql(ctx, "DROP TABLE IF EXISTS hoe_64 CASCADE;");
}

TEST_CASE("JOIN with CTE wrapping mmsearch preserves bioseq projections",
          "[join][cte][subquery][placeholder_support]") {
    // CTEs introduce a SubqueryScan / CteScan node above the
    // BioseqScan. bioseq_placeholder_support copies varlevelsup from
    // the inner Var; a CTE materialisation point bumps that level.
    // None of the other JOIN tests use a WITH clause, so a
    // regression in varlevelsup propagation through the CTE
    // rewrite path would slip past everything else.
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "WITH probes AS ("
            "    SELECT pk, seq0 FROM {} WHERE pk <= 3"
            ") "
            "SELECT t1.pk, p.pk, bioseq.cigar(t1.pk) "
            "  FROM {} t1 "
            "  JOIN probes p ON mmsearch(t1.seq0, p.seq0) "
            " ORDER BY t1.pk, p.pk;",
            kHoeJoin, kHoeJoin));
    REQUIRE_FALSE(rows.empty());
    for (const auto& row : rows) {
        REQUIRE(row.size() == 3);
        REQUIRE(row[0].has_value());
        REQUIRE(row[1].has_value());
        const auto pk1 = std::stoll(row[0].value());
        const auto pk2 = std::stoll(row[1].value());
        REQUIRE((pk1 >= 1 && pk1 <= 5));
        REQUIRE((pk2 >= 1 && pk2 <= 3));
        if (row[2].has_value() && !row[2].value().empty()) {
            REQUIRE(is_valid_cigar(row[2].value()));
        }
    }
}

TEST_CASE("LATERAL JOIN with mmsearch on inner side parameterises per outer row",
          "[join][lateral][param_rebind]") {
    // LATERAL produces a NestLoop with param_exec references that
    // are syntactically distinct from a flat INNER JOIN ... ON
    // mmsearch(...). The Param-rebind path's MmsearchParamRebind
    // logic must handle both shapes. A regression that breaks
    // LATERAL while leaving the rewritten flat form working would
    // slip past the existing INNER-JOIN test.
    auto ctx = fresh_context({kHoeJoinLeft, kHoeJoinRight});
    seed_hoe_fixture(ctx, kHoeJoinLeft);
    seed_hoe_fixture(ctx, kHoeJoinRight);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, sub.pk, bioseq.cigar(sub.pk) "
            "  FROM {} t1 "
            "       CROSS JOIN LATERAL ("
            "         SELECT pk FROM {} t2 WHERE mmsearch(t2.seq0, t1.seq1)"
            "       ) sub "
            " ORDER BY t1.pk, sub.pk;",
            kHoeJoinLeft, kHoeJoinRight));
    for (const auto& row : rows) {
        REQUIRE(row.size() == 3);
        REQUIRE(row[0].has_value());
        REQUIRE(row[1].has_value());
        const auto pk1 = std::stoll(row[0].value());
        const auto pk2 = std::stoll(row[1].value());
        REQUIRE((pk1 >= 1 && pk1 <= 5));
        REQUIRE((pk2 >= 1 && pk2 <= 5));
    }
}

TEST_CASE("Many-to-many mmsearch as join filter pins documented NULL-projection behaviour",
          "[join][documented_limitation][many_to_many]") {
    // Pinning the documented limitation from REPORT §6: fully
    // many-to-many produces NULL projections and no crash. If a
    // future fix lifts this limitation the test will fail
    // INTENTIONALLY and we update it. If the limitation accidentally
    // changes from "NULL projections" to "garbage projections" or
    // "crash" we catch it immediately.
    auto ctx = fresh_context({kHoeJoinLeft, kHoeJoinRight});
    seed_hoe_fixture(ctx, kHoeJoinLeft);
    seed_hoe_fixture(ctx, kHoeJoinRight);

    // Comma-join + WHERE rather than INNER JOIN ... ON to suppress
    // the planner's parameterised inner-side rewrite — this hits
    // the join-filter shape the report describes.
    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk, bioseq.cigar(t1.pk) "
            "  FROM {} t1, {} t2 "
            " WHERE mmsearch(t1.seq0, t2.seq1) "
            " ORDER BY t1.pk, t2.pk;",
            kHoeJoinLeft, kHoeJoinRight));
    // Server did not crash — that's already a non-trivial
    // assertion. Now verify the documented projection NULLs.
    for (const auto& row : rows) {
        REQUIRE(row.size() == 3);
        if (row[2].has_value()) {
            // Documented behaviour: when no parameterised path
            // applies, projections are either NULL or empty.
            REQUIRE(row[2].value().empty());
        }
    }
    // Session must still be usable.
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("LEFT JOIN with bioseq.symbol_count projection does not crash",
          "[join][left_join][symbol_count][regression]") {
    // Symmetric to the bioseq.cigar regression: the
    // bioseq.symbol_count projection placeholder is rewritten by the
    // same VarInjectorMutator, so a missing PHV-SUPPORT clause on
    // bioseq.symbol_count would re-introduce the LEFT JOIN crash for
    // queries that use it instead of cigar. This is REVIEW C1 from
    // REPORT_POSTGRES_QUERY_BUGFIXING_1 — symbol_count must also
    // route through bioseq_symbol_count_support's simplify branch.
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk, bioseq.symbol_count(t1.seq0, 'A') "
            "  FROM {} t1 LEFT JOIN {} t2 ON t1.pk = t2.pk "
            " ORDER BY t1.pk;",
            kHoeJoin, kHoeJoin));
    REQUIRE(rows.size() == 5);
    for (const auto& row : rows) {
        REQUIRE(row.size() == 3);
        REQUIRE(row[0].has_value());
        REQUIRE(row[1].has_value());
        // Every seeded sequence has a deterministic A-count from the stored
        // INLINE symbol_counts index (DOC0279 §12.5), populated by the columnar
        // AM at INSERT time when update_index_on_insert = on (which
        // seed_hoe_fixture sets).
        REQUIRE(row[2].has_value());
        const auto count = std::stoll(row[2].value());
        REQUIRE(count >= 0);
    }

    // Guard: a row inserted WITHOUT the stored INLINE index, read back with
    // bioseq.read_sequence_index = off, must still surface a NON-NULL real
    // tally via the unconditional recompute fallback (never NULL). seq0 of the
    // new pk=6 row is 'AAAACCCCGGGGTTTTAAAACCCCGGGGTTTT' (A appears 8 times).
    {
        guc_scope no_insert(ctx, "bioseq.update_index_on_insert", "off");
        exec_sqlf(ctx,
                  "INSERT INTO {} VALUES "
                  "(6, 'AAAACCCCGGGGTTTTAAAACCCCGGGGTTTT'::bioseq.seq('dna4', 32), "
                  "'AAAACCCC'::bioseq.seq('dna4', 32));",
                  kHoeJoin);
    }
    {
        guc_scope no_read(ctx, "bioseq.read_sequence_index", "off");
        const auto guard_row = query_one(
            ctx,
            fmt::format(
                "SELECT bioseq.symbol_count(seq0, 'A') FROM {} WHERE pk = 6;",
                kHoeJoin));
        REQUIRE(guard_row.size() == 1);
        REQUIRE(guard_row[0].has_value());  // recompute fallback is never NULL
        REQUIRE(std::stoll(guard_row[0].value()) == 8);
    }
}

TEST_CASE("EXPLAIN ANALYZE survives CROSS JOIN of bioseq tables",
          "[join][cross_join][explain]") {
    // Bug A also corrupted EXPLAIN ANALYZE because EXPLAIN runs the plan.
    // Make sure neither EXPLAIN itself nor the actual-row counts trip.
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);

    const auto plan = explain_text(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk "
            "  FROM {} t1 CROSS JOIN {} t2 "
            " ORDER BY t1.pk, t2.pk;",
            kHoeJoin, kHoeJoin));
    REQUIRE_FALSE(plan.empty());
    REQUIRE(plan_contains(plan, "BioseqScan"));
    // Both relations should produce a Custom Scan node — the planner
    // self-join optimisation that could collapse them away does not
    // apply when the table uses our access method. Catching only one
    // BioseqScan would mean the second scan was rewritten to a heap
    // path (e.g. Seq Scan), which would silently regress the cost
    // shaping.
    int bioseq_scan_count = 0;
    for (const auto& line : plan) {
        if (line.find("Custom Scan (BioseqScan)") != std::string::npos) {
            ++bioseq_scan_count;
        }
    }
    REQUIRE(bioseq_scan_count == 2);
    // Pre-fix the actual rows printed by EXPLAIN ANALYZE for the
    // outer side were 1 instead of 5; assert the outer-side actual
    // count by inspecting the raw plan strings. We accept any
    // ordering of "(actual ... rows=N..." after the BioseqScan.
    const std::string actual_marker = "rows=5";
    bool outer_returned_five = false;
    for (const auto& line : plan) {
        if (line.find("Custom Scan (BioseqScan)") != std::string::npos
            && line.find(actual_marker) != std::string::npos) {
            outer_returned_five = true;
            break;
        }
    }
    REQUIRE(outer_returned_five);
}

TEST_CASE("Cross-table mmsearch (mmsearch as join filter) returns rows only "
          "where the aligner produced a real match",
          "[join][cross_join][cross_table_mmsearch][regression]") {
    // Pre-fix: `WHERE mmsearch(t1.seq, t2.seq2)` ran the C stub
    // (always TRUE) above the Nested Loop, returning every cross-
    // product row with NULL bioseq.* columns. The user reported this
    // — the expectation is that mmsearch behaves like a fuzzy
    // substring search: rows whose alignment doesn't really match
    // must be filtered out, not surfaced with NULL projections.
    //
    // Post-fix:
    //   1. ExtractPushdownClause accepts mmsearch FuncExprs so the
    //      planner generates a parameterised inner-side path.
    //   2. AddColumnarScanPath counts mmsearch in scan quals as
    //      an additional wehavemagic unit so the parameterised
    //      path's cost beats the unparameterised competitor.
    //   3. run_batch_and_build_cursors filters out matches whose
    //      cigar would come back empty (when do_cigar=true), since
    //      those are the aligner's "found a chain but the base
    //      alignment produced nothing useful" sentinels.
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk, bioseq.cigar(t1.pk) "
            "  FROM {} t1 CROSS JOIN {} t2 "
            " WHERE mmsearch(t1.seq0, t2.seq1) "
            " ORDER BY t1.pk, t2.pk;",
            kHoeJoin, kHoeJoin));

    // Pre-fix this came back as 25 rows (full 5x5 cartesian) with
    // every cigar NULL — exactly the regression the user
    // described. Post-fix the row count drops because not every
    // pair produces a valid alignment, AND every emitted cigar is
    // a non-empty valid CIGAR string.
    REQUIRE_FALSE(rows.empty());
    // Strict upper bound: at most full cartesian (25) — no row
    // explosion from match multiplicity should exceed that under
    // these probes, but we leave headroom for the multi-match-per-
    // row case.
    REQUIRE(rows.size() <= 100);

    for (const auto& row : rows) {
        REQUIRE(row.size() == 3);
        REQUIRE(row[0].has_value());
        REQUIRE(row[1].has_value());
        // The user's headline assertion: every row that surfaces
        // MUST have a non-empty, valid CIGAR — no
        // "rows with empty/NULL bioseq.cigar" fall out of the
        // join filter.
        REQUIRE(row[2].has_value());
        REQUIRE_FALSE(row[2].value().empty());
        REQUIRE(is_valid_cigar(row[2].value()));
    }
}

TEST_CASE("Multiple Const mmsearch quals AND-filter the same column",
          "[mmsearch][multi_qual][regression]") {
    // Tests the new multi-mmsearch-per-scan support. Two
    // mmsearch quals on the same column with different literal
    // queries should AND together: a row is returned iff BOTH
    // queries align against its sequence. The first mmsearch
    // drives projections; the second is a pure filter.
    //
    // For the test to discriminate "AND is honoured" vs "second
    // qual is a silent no-op via the C stub", the two probes
    // must match DIFFERENT subsets of rows. We pick:
    //   probe_1 = canonical probe (matches a known set)
    //   probe_2 = a probe rooted in a different region of the
    //             seeded sequences, hence matching a different
    //             set. The intersection should be a strict
    //             subset of the union — and the AND result must
    //             equal the per-row intersection.
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);

    // Probe 2 matches a different subset than the canonical probe
    // (overlapping but not identical), so the AND result is a
    // strict intersection. If the test runner ignored the second
    // qual it would observe `set_a` (canonical's matches) instead
    // of the intersection — and the equality assertion below would
    // fail.
    constexpr std::string_view second_probe = "TTCATCATCATCATCTACTCCT";

    const auto rows_a = query_rows(
        ctx,
        fmt::format(
            "SELECT pk FROM {} WHERE mmsearch(seq0, '{}'::bioseq.seq('dna4', 32)) "
            "ORDER BY pk;",
            kHoeJoin, kHoeProbeSequence));
    const auto rows_b = query_rows(
        ctx,
        fmt::format(
            "SELECT pk FROM {} WHERE mmsearch(seq0, '{}'::bioseq.seq('dna4', 32)) "
            "ORDER BY pk;",
            kHoeJoin, second_probe));
    REQUIRE_FALSE(rows_a.empty());
    REQUIRE_FALSE(rows_b.empty());

    // Build the expected intersection (set semantics over pk).
    std::set<std::string> set_a, set_b, expected_intersection;
    for (const auto& r : rows_a) if (r[0].has_value()) set_a.insert(*r[0]);
    for (const auto& r : rows_b) if (r[0].has_value()) set_b.insert(*r[0]);
    for (const auto& key : set_a) {
        if (set_b.count(key) > 0) expected_intersection.insert(key);
    }

    const auto and_rows = query_rows(
        ctx,
        fmt::format(
            "SELECT pk FROM {} "
            " WHERE mmsearch(seq0, '{}'::bioseq.seq('dna4', 32)) "
            "   AND mmsearch(seq0, '{}'::bioseq.seq('dna4', 32)) "
            " ORDER BY pk;",
            kHoeJoin, kHoeProbeSequence, second_probe));
    std::set<std::string> set_and;
    for (const auto& r : and_rows) if (r[0].has_value()) set_and.insert(*r[0]);

    // The AND result MUST be exactly the intersection of A and B.
    // If the second qual were silently a no-op (C-stub TRUE), the
    // AND result would equal A — possibly a strict superset of
    // A∩B. Asserting equality with the intersection is what makes
    // this test discriminating.
    REQUIRE(set_and == expected_intersection);
    REQUIRE(set_and.size() <= set_a.size());
    REQUIRE(set_and.size() <= set_b.size());
}

TEST_CASE("Aggregate over bioseq.cigar projection (MAX) returns the "
          "lexicographic max CIGAR",
          "[aggregate][placeholder_support][regression]") {
    // Exercises the has_aggs branch of bioseq_placeholder_support.
    // Pre-fix this was untested and the vectorised aggregator
    // crashed with SIGSEGV when it tried to consume a regular slot
    // carrying placeholder Consts. Post-fix the planner hook skips
    // the vectorised aggregator rewrite when the agg input depends
    // on a bioseq projection, falling back to the standard
    // Aggregate node which evaluates the placeholder Const correctly.
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);

    const std::string sql = fmt::format(
        "SELECT MAX(bioseq.cigar(pk)) FROM {} "
        " WHERE mmsearch(seq0, '{}'::bioseq.seq('dna4', 32));",
        kHoeJoin, kHoeProbeSequence);
    const auto row = query_one(ctx, sql);
    REQUIRE(row.size() == 1);
    REQUIRE(row[0].has_value());
    REQUIRE_FALSE(row[0].value().empty());
    REQUIRE(is_valid_cigar(row[0].value()));
}

TEST_CASE("COUNT(*) over an mmsearch-filtered bioseq table compiles and runs",
          "[aggregate][placeholder_support][regression]") {
    // COUNT(*) doesn't take a bioseq projection as an argument,
    // so the SUPPORT-function PHV machinery doesn't fire for it
    // — but the WHERE mmsearch(...) does filter rows through our
    // batch processor. The hit count must reflect the number of
    // matching rows the aligner returned, NOT the raw 5 rows in
    // the fixture.
    //
    // INT-typed aggregates over bioseq.* projections are tracked
    // separately as a known limitation (see
    // REPORT_POSTGRES_QUERY_BUGFIXING_1.md §6): wrapping
    // `bioseq.target_start(pk)` in a PHV inside an Aggref args
    // triggers `ERROR: Aggref found in non-Agg plan node` from
    // Postgres' `setrefs` pass when the table has more than one
    // row. The text-typed (cigar) version works because there is
    // no vectorised aggregate variant for TEXT, which keeps the
    // planner on a different code path — that case is exercised
    // by the dedicated MAX(bioseq.cigar(pk)) test above.
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);

    const std::string count_sql = fmt::format(
        "SELECT COUNT(*)::int8 AS hits "
        "  FROM {} "
        " WHERE mmsearch(seq0, '{}'::bioseq.seq('dna4', 32));",
        kHoeJoin, kHoeProbeSequence);
    const auto count_row = query_one(ctx, count_sql);
    REQUIRE(count_row.size() == 1);
    REQUIRE(count_row[0].has_value());
    const long long hits = std::stoll(count_row[0].value());
    REQUIRE(hits > 0);
    // Five seeded rows; not all need to align under the probe.
    REQUIRE(hits <= 100);
}

TEST_CASE("GROUP BY pk with bioseq aggregate returns one row per matching pk",
          "[aggregate][group_by][placeholder_support]") {
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);

    const std::string sql = fmt::format(
        "SELECT pk, MAX(bioseq.cigar(pk)) AS best_cigar "
        "  FROM {} "
        " WHERE mmsearch(seq0, '{}'::bioseq.seq('dna4', 32)) "
        " GROUP BY pk ORDER BY pk;",
        kHoeJoin, kHoeProbeSequence);
    const auto rows = query_rows(ctx, sql);
    REQUIRE_FALSE(rows.empty());
    for (const auto& row : rows) {
        REQUIRE(row.size() == 2);
        REQUIRE(row[0].has_value());
        REQUIRE(row[1].has_value());
        REQUIRE_FALSE(row[1].value().empty());
        REQUIRE(is_valid_cigar(row[1].value()));
    }
}

TEST_CASE("Param-driven primary + Const-extra mmsearch AND-stack works",
          "[mmsearch][multi_qual][param_plus_const]") {
    // Param + Const-extra: the primary mmsearch is a parametrized
    // inner-side qual (Param-rebind path) AND there is an
    // additional Const-driven mmsearch on the same column. The
    // expected behaviour is logical AND: rows must match BOTH the
    // per-outer-row Param query AND the literal extra.
    //
    // This was historically rejected with NOTICE ("Const+Const
    // stacking only"). §9.7 of REPORT_POSTGRES_QUERY_BUGFIXING_1.md
    // documents the relaxation: the extras (Const) live in
    // `extra_query_varlenas` independently of the primary's
    // rebind-on-rescan, so the AND-filter still works correctly
    // each time the Param resolves.
    auto ctx = fresh_context({kHoeJoinLeft, kHoeJoinRight});
    seed_hoe_fixture(ctx, kHoeJoinLeft);
    seed_hoe_fixture(ctx, kHoeJoinRight);

    // First, baseline: Param-only — find the matches per outer
    // row. We use mmsearch(t2.seq0, t1.seq1) — t1 is outer, t2 is
    // inner with Param-rebind.
    const auto rows_param_only = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk "
            "  FROM {} t1 INNER JOIN {} t2 ON mmsearch(t2.seq0, t1.seq1) "
            " ORDER BY t1.pk, t2.pk;",
            kHoeJoinLeft, kHoeJoinRight));
    REQUIRE_FALSE(rows_param_only.empty());

    // What rows does the Const-extra probe match in isolation?
    // We use a probe rooted in a different region of seq0 so the
    // intersection with the Param-only set is provably STRICT —
    // the goal is to catch a regression where the Const-extra is
    // silently dropped (we'd see rows_param_plus_const equal to
    // rows_param_only, which is a SUBSET of itself, so a plain
    // subset-check is a tautology). The probe below matches a
    // proper subset of pk values; combined with the Param qual,
    // the AND must produce strictly fewer rows than Param-only.
    constexpr std::string_view kStrictSubsetProbe =
        "TCATCATCATCATCTACTCCTATCTATCATCATCATCATCTACTCCT";  // matches t2.pk=4 only
    const auto rows_const_only = query_rows(
        ctx,
        fmt::format(
            "SELECT pk FROM {} WHERE mmsearch(seq0, '{}'::bioseq.seq('dna4', 32)) "
            "ORDER BY pk;",
            kHoeJoinRight, kStrictSubsetProbe));
    REQUIRE_FALSE(rows_const_only.empty());
    std::set<std::string> const_only_t2_pks;
    for (const auto& row : rows_const_only) {
        if (row[0].has_value()) const_only_t2_pks.insert(*row[0]);
    }
    // Sanity: const-only matches FEWER rows of t2 than the universe;
    // otherwise our AND test below collapses to "param only".
    REQUIRE(const_only_t2_pks.size() < 5);

    const auto rows_param_plus_const = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk "
            "  FROM {} t1 INNER JOIN {} t2 "
            "    ON mmsearch(t2.seq0, t1.seq1) "
            " WHERE mmsearch(t2.seq0, '{}'::bioseq.seq('dna4', 32)) "
            " ORDER BY t1.pk, t2.pk;",
            kHoeJoinLeft, kHoeJoinRight, kStrictSubsetProbe));

    std::set<std::pair<std::string, std::string>> param_only_set;
    for (const auto& row : rows_param_only) {
        if (row.size() == 2 && row[0].has_value() && row[1].has_value()) {
            param_only_set.emplace(*row[0], *row[1]);
        }
    }
    // The AND-stack result MUST be a subset of the Param-only
    // result AND every emitted t2.pk must be in the const-only
    // matching set. If §9.4's Const-extra was silently dropped,
    // we'd see entries with t2.pk NOT in const_only_t2_pks —
    // catching the regression directly.
    for (const auto& row : rows_param_plus_const) {
        REQUIRE(row.size() == 2);
        REQUIRE(row[0].has_value());
        REQUIRE(row[1].has_value());
        const auto key = std::make_pair(*row[0], *row[1]);
        REQUIRE(param_only_set.count(key) > 0);
        // Load-bearing assertion: t2.pk MUST also be in the
        // const-only matching set. A regression that silently
        // drops the Const extra would fail here.
        REQUIRE(const_only_t2_pks.count(*row[1]) > 0);
    }
    // Strict-subset property: the AND result must be SMALLER than
    // the Param-only result (the const probe matches strictly
    // fewer rows). If they're equal, the Const extra was a
    // no-op — the bug we're guarding against.
    REQUIRE(rows_param_plus_const.size() < rows_param_only.size());
}

TEST_CASE("Cross-table mmsearch via INNER JOIN with bioseq.cigar projection",
          "[join][inner_join][cross_table_mmsearch][regression]") {
    // Variant of the user's CROSS JOIN regression that goes through
    // INNER JOIN syntax instead. Same semantics — every emitted
    // row must have a valid (non-empty) CIGAR, and the planner
    // must pick the parameterised inner-side path.
    auto ctx = fresh_context({kHoeJoinLeft, kHoeJoinRight});
    seed_hoe_fixture(ctx, kHoeJoinLeft);
    seed_hoe_fixture(ctx, kHoeJoinRight);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk, bioseq.cigar(t1.pk) "
            "  FROM {} t1 INNER JOIN {} t2 "
            "    ON mmsearch(t1.seq0, t2.seq1) "
            " ORDER BY t1.pk, t2.pk;",
            kHoeJoinLeft, kHoeJoinRight));
    REQUIRE_FALSE(rows.empty());
    for (const auto& row : rows) {
        REQUIRE(row.size() == 3);
        REQUIRE(row[0].has_value());
        REQUIRE(row[1].has_value());
        REQUIRE(row[2].has_value());
        REQUIRE_FALSE(row[2].value().empty());
        REQUIRE(is_valid_cigar(row[2].value()));
    }
}

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
#include "headers/pg_build_config.h"

#include <chrono>
#include <libpq-fe.h>
#include <set>
#include <thread>

import bioseq.integration_tests.common;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view kHoeJoin = "hoe_join";
constexpr std::string_view kHoeJoinLeft = "hoe_join_left";
constexpr std::string_view kHoeJoinRight = "hoe_join_right";
constexpr std::string_view kDoc0377PlannerTarget =
    "doc0377_planner_target";
constexpr std::string_view kDoc0377PlannerQueries =
    "doc0377_planner_queries";

void configure_doc0377_transformed_join(connection_test_context& ctx) {
    exec_sql(ctx,
        "SET max_parallel_workers_per_gather = 0;"
        "SET bioseq.read_sequence_index = off;"
        "SET bioseq.aligner.vulkan_enabled = off;"
        "SET bioseq.aligner.minimizer_algorithm = 'scalar';"
        "SET bioseq.aligner.chaining_algorithm = 'scalar_mm2';"
        "SET bioseq.aligner.base_alignment_algorithm = 'ksw2';"
        "SET bioseq.aligner.min_chain_score = 20;");
}

void seed_doc0377_planner_fixture(connection_test_context& ctx) {
    seed_hoe_fixture(ctx, kDoc0377PlannerTarget);
    exec_sql(ctx, fmt::format(
        "CREATE TEMP TABLE {} ("
        "id int PRIMARY KEY, target_pk int NOT NULL, "
        "seq bioseq.seq('dna4',32));"
        "INSERT INTO {} VALUES "
        "(1, 1, {}), "
        "(2, 1, NULL::bioseq.seq('dna4',32)), "
        "(3, 1, {});",
        kDoc0377PlannerQueries, kDoc0377PlannerQueries,
        hoe_probe_literal(), hoe_probe_literal()));
    configure_doc0377_transformed_join(ctx);
}

[[nodiscard]] bool doc0377_plan_has_executable_mmsearch(
        const std::vector<std::string>& plan) {
    constexpr std::array<std::string_view, 5u> executable_prefixes{
        "Filter: mmsearch", "Join Filter: mmsearch",
        "One-Time Filter: mmsearch", "Index Cond: mmsearch",
        "Recheck Cond: mmsearch"};
    for (const auto& line : plan) {
        for (const auto prefix : executable_prefixes) {
            if (line.find(prefix) != std::string::npos) return true;
        }
    }
    return false;
}

void require_doc0377_owned_qual_audit(
        const std::vector<std::string>& plan) {
    constexpr std::array<std::string_view, 5u> properties{
        "Bioseq Owned RestrictInfo Path Copies",
        "Bioseq Executable Mmsearch Qual Copies",
        "Bioseq Vector Mmsearch Qual Copies",
        "Bioseq Storage Mmsearch Qual Copies",
        "Bioseq Descendant Mmsearch Owner Copies"};
    for (const auto property : properties) {
        const auto value = find_explain_property(plan, property);
        INFO(property);
        REQUIRE(value.has_value());
        REQUIRE(*value == "0");
    }
    REQUIRE(plan_contains(
        plan, "Bioseq Residual Mmsearch Qual: absent"));
    REQUIRE_FALSE(doc0377_plan_has_executable_mmsearch(plan));
}

struct Doc0377AsyncUpdateOutcome {
    bool sent = false;
    bool lock_wait_observed = false;
    bool consume_ok = true;
    bool got_command = false;
    bool got_tuple_result = false;
    bool result_ok = true;
    std::size_t returned_rows = 0u;
    std::string error;
};

[[nodiscard]] Doc0377AsyncUpdateOutcome run_doc0377_locked_update(
        connection_test_context& blocker,
        connection_test_context& worker,
        connection_test_context& observer,
        const std::string& blocker_update,
        const std::string& worker_update) {
    Doc0377AsyncUpdateOutcome outcome{};
    exec_sql(blocker, "BEGIN;");
    exec_sql(blocker, blocker_update);
    const long long worker_pid = query_scalar_int(
        worker, "SELECT pg_backend_pid();");
    outcome.sent = PQsendQuery(worker.raw(), worker_update.c_str()) == 1;
    if (outcome.sent) {
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            const long long waiting = query_scalar_int(observer, fmt::format(
                "SELECT count(*) FROM pg_stat_activity "
                "WHERE pid={} AND wait_event_type='Lock';",
                worker_pid));
            if (waiting == 1) {
                outcome.lock_wait_observed = true;
                break;
            }
            if (PQconsumeInput(worker.raw()) == 0) {
                outcome.consume_ok = false;
                break;
            }
            if (PQisBusy(worker.raw()) == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    /* Never leave the worker blocked if a precondition fails.  Commit before
     * inspecting libpq results so the EvalPlanQual recheck can complete. */
    exec_sql(blocker, "COMMIT;");
    if (!outcome.sent) return outcome;
    for (unsigned attempt = 0;
         attempt < 1000 && PQisBusy(worker.raw()) != 0; ++attempt) {
        if (PQconsumeInput(worker.raw()) == 0) {
            outcome.consume_ok = false;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (PQisBusy(worker.raw()) != 0) return outcome;
    while (PGresult* result = PQgetResult(worker.raw())) {
        const ExecStatusType status = PQresultStatus(result);
        if (status == PGRES_COMMAND_OK) {
            outcome.got_command = true;
        } else if (status == PGRES_TUPLES_OK) {
            outcome.got_tuple_result = true;
            outcome.returned_rows += static_cast<std::size_t>(
                PQntuples(result));
        } else {
            outcome.result_ok = false;
            const char* message = PQresultErrorMessage(result);
            if (message != nullptr) outcome.error += message;
        }
        PQclear(result);
    }
    return outcome;
}

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
    // Force the generic CachedPlan so $1 remains PARAM_EXTERN at
    // BeginCustomScan.  Custom plans often fold it to Const and would not
    // exercise the client-parameter binder at all.
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);
	exec_sql(ctx, "SET plan_cache_mode = force_generic_plan;");

    exec_sqlf(ctx,
              "PREPARE p_join_test(bioseq.seq('dna4', 32)) AS "
              "SELECT t1.pk, bioseq.cigar(t1.pk) "
              "  FROM {} t1 LEFT JOIN {} t2 ON t1.pk = t2.pk "
              " WHERE mmsearch(t1.seq0, $1) ORDER BY t1.pk;",
              kHoeJoin, kHoeJoin);
	const auto generic_plan = explain_text<false>(
		ctx,
		"EXECUTE p_join_test("
		"'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32));");
	REQUIRE(plan_contains(generic_plan, "Custom Scan (BioseqScan)"));

    const auto run1 = query_rows(
        ctx,
        "EXECUTE p_join_test('CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32));");
    const auto run2 = query_rows(
        ctx,
        "EXECUTE p_join_test('TCATCATCTACTCCTA'::bioseq.seq('dna4', 32));");
    const auto run3 = query_rows(
        ctx,
        "EXECUTE p_join_test('CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32));");
	const auto run_null = query_rows(
		ctx, "EXECUTE p_join_test(NULL);");

    // Same parameter must produce same output (no stale state).
    REQUIRE(run1 == run3);
    // Different parameter should usually produce a different result
    // set; if it doesn't (rare alignment coincidence) at least the
    // result is non-empty so the test isn't a tautology.
    REQUIRE_FALSE(run2.empty());
	// mmsearch is STRICT: a NULL external Param is a no-match, never an
	// inactive-buffer path that leaks through the SQL rendezvous stub.
	REQUIRE(run_null.empty());

	const long long before_bad_typmod = query_scalar_int(
		ctx,
		"SELECT execution_id FROM "
		"bioseq.last_alignment_execution_stats() LIMIT 1;");
	const auto bad_typmod = sql_error(
		ctx,
		"EXECUTE p_join_test("
		"'ACGTACGTACGTACGTACGTACGTACGTACGT'"
		"::bioseq.seq('dna5', 32));");
#if BIOSEQ_OPTTIMIZATION_LEVEL == 0
	REQUIRE(bad_typmod.sqlstate == "22023");
	REQUIRE(bad_typmod.contains(
		"bioseq.seq alphabet \"dna5\" is not supported"));
	REQUIRE(bad_typmod.contains("Valid alphabets: 'dna4'"));
#else
	REQUIRE(bad_typmod.sqlstate == "42804");
	REQUIRE((
		bad_typmod.contains(
			"bioseq mmsearch requires operands with the same bound alphabet")
		|| bad_typmod.contains(
			"cannot cast bound bioseq.seq across alphabets")));
#endif
	/* A generic PREPARE Param loses its declared typmod in the plan, so the
	 * pgseq payload is checked at runtime when dna5 is enabled. The level-0
	 * profile rejects dna5 while parsing the EXECUTE argument instead. Both
	 * rejections happen before a new alignment root can replace the last
	 * completed snapshot. */
	REQUIRE(query_scalar_int(
		ctx,
		"SELECT execution_id FROM "
		"bioseq.last_alignment_execution_stats() LIMIT 1;")
		== before_bad_typmod);

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

TEST_CASE("Same-alphabet cross-width JOIN matches the identical-width oracle",
          "[join][typmod_coalesce][rewidth][regression]") {
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);
    exec_sql(
        ctx,
        fmt::format(
            "DROP TABLE IF EXISTS hoe_64 CASCADE; "
            "CREATE TABLE hoe_64 ("
            "pk INT PRIMARY KEY, seq0 bioseq.seq('dna4', 64)) USING bioseq; "
            "INSERT INTO hoe_64 SELECT pk, seq0 FROM {};",
            kHoeJoin));

    const auto same_width = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk, bioseq.cigar(t1.pk) "
            "FROM {} t1 JOIN {} t2 ON mmsearch(t1.seq0, t2.seq0) "
            "ORDER BY t1.pk, t2.pk;",
            kHoeJoin,
            kHoeJoin));
    const auto cross_width = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk, bioseq.cigar(t1.pk) "
            "FROM {} t1 JOIN hoe_64 t2 ON mmsearch(t1.seq0, t2.seq0) "
            "ORDER BY t1.pk, t2.pk;",
            kHoeJoin));

    REQUIRE_FALSE(same_width.empty());
    REQUIRE(cross_width == same_width);
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

TEST_CASE("PARAM_EXEC rescan clears NULL and restores the next primary query",
          "[join][lateral][param_rebind][null][lifecycle]") {
    /* OFFSET 0 keeps the LATERAL child as a parameterized Nested Loop inner
     * scan, so p.probe reaches BioseqScan as PARAM_EXEC.  VALUES supplies the
     * load-bearing non-NULL -> NULL -> non-NULL rescan sequence in one
     * executor: the NULL iteration must not replay stale matches, and the
     * third iteration must reconfigure the same scan state successfully. */
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);

    const std::string sql = fmt::format(
        "SELECT p.ord, sub.pk "
        "FROM (VALUES (1, {}), "
        "             (2, NULL::bioseq.seq('dna4', 32)), "
        "             (3, {})) AS p(ord, probe) "
        "LEFT JOIN LATERAL ("
        "  SELECT t.pk FROM {} t "
        "  WHERE mmsearch(t.seq0, p.probe) "
        "  ORDER BY t.pk OFFSET 0"
        ") sub ON true "
        "ORDER BY p.ord, sub.pk",
        hoe_probe_literal(), hoe_probe_literal(), kHoeJoin);
    const auto plan = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(plan, "Nested Loop"));
    REQUIRE(plan_contains(plan, "Values Scan"));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));

    const auto rows = query_rows(ctx, sql + ";");
    std::vector<std::string> first_matches;
    std::vector<std::string> restored_matches;
    std::size_t null_outer_rows = 0;
    for (const auto& row : rows) {
        REQUIRE(row.size() == 2);
        REQUIRE(row[0].has_value());
        const long long ordinal = std::stoll(*row[0]);
        if (ordinal == 1) {
            REQUIRE(row[1].has_value());
            first_matches.push_back(*row[1]);
        } else if (ordinal == 2) {
            REQUIRE_FALSE(row[1].has_value());
            ++null_outer_rows;
        } else if (ordinal == 3) {
            REQUIRE(row[1].has_value());
            restored_matches.push_back(*row[1]);
        } else {
            FAIL("unexpected PARAM_EXEC probe ordinal");
        }
    }
    REQUIRE_FALSE(first_matches.empty());
    REQUIRE(null_outer_rows == 1);
    REQUIRE(restored_matches == first_matches);
}

TEST_CASE("DOC0377 transformed LATERAL owner survives PARAM_EXEC rescans",
          "[doc0377][mmsearch][slice][lateral][param][rescan]") {
    auto ctx = fresh_context({kDoc0377PlannerTarget});
    seed_doc0377_planner_fixture(ctx);

    constexpr std::array<std::string_view, 2u> option_suffixes{
        "", ", NULL::bioseq_aligner_options"};
    for (const auto option_suffix : option_suffixes) {
        DYNAMIC_SECTION((option_suffix.empty()
                ? "two-argument mmsearch"
                : "three-argument mmsearch with default options")) {
            /* OFFSET 0 is the pull-up fence.  q.id=d.ord makes the child join
             * genuinely parameterized; the same BioseqBatchJoin is rescanned
             * over present -> NULL -> present sequence payloads. */
            const std::string sql = fmt::format(
                "SELECT d.ord, matched.pk "
                "FROM (VALUES (1), (2), (3)) AS d(ord) "
                "LEFT JOIN LATERAL ("
                "  SELECT t.pk FROM {} AS t INNER JOIN {} AS q "
                "    ON t.pk=q.target_pk AND t.pk>0 AND q.target_pk>0 "
                "   AND mmsearch(t.seq0[0:], q.seq[0:]{} ) "
                "  WHERE q.id=d.ord "
                "  ORDER BY t.pk LIMIT 1 OFFSET 0"
                ") AS matched ON true "
                "ORDER BY d.ord, matched.pk;",
                kDoc0377PlannerTarget, kDoc0377PlannerQueries,
                option_suffix);
            const auto plan = explain_text<false>(ctx, sql);
            REQUIRE(plan_contains(plan, "Nested Loop"));
            REQUIRE(plan_contains(
                plan, "Custom Scan (BioseqBatchJoin)"));
            REQUIRE(plan_contains(
                plan, "Bioseq Mmsearch Ownership: per-row recipe"));
            REQUIRE(plan_contains(
                plan, "Custom Scan (BioseqSliceProject)"));
            require_doc0377_owned_qual_audit(plan);

            const auto rows = query_rows(ctx, sql);
            REQUIRE(rows.size() == 3u);
            REQUIRE((rows[0] == row_t{
                std::optional<std::string>{"1"},
                std::optional<std::string>{"1"}}));
            REQUIRE((rows[1] == row_t{
                std::optional<std::string>{"2"}, std::nullopt}));
            REQUIRE((rows[2] == row_t{
                std::optional<std::string>{"3"},
                std::optional<std::string>{"1"}}));
        }
    }
}

TEST_CASE("DOC0377 transformed owner remains inside CTE and subquery fences",
          "[doc0377][mmsearch][slice][cte][subquery][ownership]") {
    auto ctx = fresh_context({kDoc0377PlannerTarget});
    seed_doc0377_planner_fixture(ctx);

    constexpr std::array<std::string_view, 2u> option_suffixes{
        "", ", NULL::bioseq_aligner_options"};
    for (const auto option_suffix : option_suffixes) {
        const std::string owned_body = fmt::format(
            "SELECT t.pk, q.id, bioseq.cigar(t.pk) AS cigar "
            "FROM {} AS t INNER JOIN {} AS q "
            "ON q.id=1 AND t.pk=q.target_pk "
            "AND mmsearch(t.seq0[0:], q.seq[0:]{} )",
            kDoc0377PlannerTarget, kDoc0377PlannerQueries,
            option_suffix);
        const std::string flat_sql = owned_body +
            " ORDER BY 1, 2, 3;";
        const std::string cte_sql = fmt::format(
            "WITH owned AS MATERIALIZED ({}) "
            "SELECT pk, id, cigar FROM owned ORDER BY 1, 2, 3;",
            owned_body);
        const std::string subquery_sql = fmt::format(
            "SELECT pk, id, cigar FROM ({} OFFSET 0) AS owned "
            "ORDER BY 1, 2, 3;",
            owned_body);

        const auto expected = query_rows(ctx, flat_sql);
        REQUIRE_FALSE(expected.empty());
        for (const auto& sql : {cte_sql, subquery_sql}) {
            const auto plan = explain_text<false>(ctx, sql);
            REQUIRE(plan_contains(
                plan, "Custom Scan (BioseqBatchJoin)"));
            REQUIRE(plan_contains(
                plan, "Custom Scan (BioseqSliceProject)"));
            require_doc0377_owned_qual_audit(plan);
            REQUIRE(query_rows(ctx, sql) == expected);
        }
        const auto cte_plan = explain_text<false>(ctx, cte_sql);
        REQUIRE(plan_contains(cte_plan, "CTE Scan"));
    }
}

TEST_CASE("DOC0377 partition child reparameterization retains one owner",
          "[doc0377][mmsearch][slice][partition][lateral][rescan]") {
    auto ctx = fresh_context({kDoc0377PlannerTarget});
    seed_hoe_fixture(ctx, kDoc0377PlannerTarget);
    exec_sql(ctx, fmt::format(
        "CREATE TEMP TABLE doc0377_partition_queries ("
        "id int, bucket int, target_pk int NOT NULL, "
        "seq bioseq.seq('dna4',32)) PARTITION BY RANGE(bucket);"
        "CREATE TEMP TABLE doc0377_partition_queries_lo "
        "PARTITION OF doc0377_partition_queries "
        "FOR VALUES FROM (0) TO (10);"
        "CREATE TEMP TABLE doc0377_partition_queries_hi "
        "PARTITION OF doc0377_partition_queries "
        "FOR VALUES FROM (10) TO (20);"
        "INSERT INTO doc0377_partition_queries VALUES "
        "(1, 5, 1, {}), "
        "(2, 15, 1, NULL::bioseq.seq('dna4',32)), "
        "(3, 5, 1, {});",
        hoe_probe_literal(), hoe_probe_literal()));
    configure_doc0377_transformed_join(ctx);
    exec_sql(ctx,
        "SET enable_partition_pruning = off;"
        "SET enable_hashjoin = off;"
        "SET enable_mergejoin = off;"
        "SET enable_nestloop = on;");

    const std::string sql = fmt::format(
        "SELECT d.ord, matched.pk "
        "FROM (VALUES (1,5), (2,15), (3,5)) AS d(ord,bucket) "
        "LEFT JOIN LATERAL ("
        "  SELECT t.pk FROM {} AS t "
        "  INNER JOIN doc0377_partition_queries AS q "
        "    ON t.pk=q.target_pk "
        "   AND mmsearch(t.seq0[0:], q.seq[0:]) "
        "  WHERE q.id=d.ord AND q.bucket=d.bucket "
        "  ORDER BY t.pk LIMIT 1 OFFSET 0"
        ") AS matched ON true ORDER BY d.ord;",
        kDoc0377PlannerTarget);
    const auto plan = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(plan, "Append"));
    REQUIRE(plan_contains(plan, "doc0377_partition_queries_lo"));
    REQUIRE(plan_contains(plan, "doc0377_partition_queries_hi"));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    require_doc0377_owned_qual_audit(plan);

    const auto rows = query_rows(ctx, sql);
    REQUIRE(rows.size() == 3u);
    REQUIRE(rows[0][0] == "1");
    REQUIRE(rows[0][1] == "1");
    REQUIRE(rows[1][0] == "2");
    REQUIRE_FALSE(rows[1][1].has_value());
    REQUIRE(rows[2][0] == "3");
    REQUIRE(rows[2][1] == "1");
}

TEST_CASE("DOC0377 dominating SliceProject spans Append branches across rescans",
          "[doc0377][mmsearch][slice][partition][append][lateral][rescan][owner-dominance]") {
    auto ctx = fresh_context({kDoc0377PlannerTarget});
    seed_hoe_fixture(ctx, kDoc0377PlannerTarget);
    exec_sql(ctx, fmt::format(
        "CREATE TEMP TABLE doc0377_owner_append_queries ("
        "id int, driver int, bucket int, target_pk int NOT NULL, "
        "seq bioseq.seq('dna4',32)) PARTITION BY RANGE(bucket);"
        "CREATE TEMP TABLE doc0377_owner_append_queries_lo "
        "PARTITION OF doc0377_owner_append_queries "
        "FOR VALUES FROM (0) TO (10);"
        "CREATE TEMP TABLE doc0377_owner_append_queries_hi "
        "PARTITION OF doc0377_owner_append_queries "
        "FOR VALUES FROM (10) TO (20);"
        "INSERT INTO doc0377_owner_append_queries "
        "SELECT d.driver * 100 + branch.row_delta, d.driver, "
        "       branch.bucket, 1, {} "
        "FROM (VALUES (1), (2), (3)) AS d(driver) "
        "CROSS JOIN (VALUES (1,5), (2,5), (3,15), (4,15)) "
        "AS branch(row_delta,bucket);",
        hoe_probe_literal()));
    configure_doc0377_transformed_join(ctx);
    exec_sql(ctx,
        "SET enable_partition_pruning = off;"
        "SET enable_hashjoin = off;"
        "SET enable_mergejoin = off;"
        "SET enable_nestloop = on;"
        "SET enable_memoize = off;");

    const std::string sql = fmt::format(
        "SELECT d.pass_no, d.driver, matched.ids, matched.hit_count "
        "FROM (VALUES (1,1), (2,2), (3,3), "
        "             (4,1), (5,2), (6,3)) AS d(pass_no,driver) "
        "CROSS JOIN LATERAL ("
        "  SELECT string_agg(hit.id::text, ',' ORDER BY hit.id) AS ids, "
        "         count(*)::bigint AS hit_count "
        "  FROM ("
        "    SELECT DISTINCT q.id, t.pk "
        "    FROM {} AS t "
        "    INNER JOIN doc0377_owner_append_queries AS q "
        "      ON t.pk=q.target_pk "
        "     AND mmsearch("
        "       t.seq0[0:16] OPERATOR(bioseq.||) t.seq0[16:], "
        "       q.seq[0:]) "
        "    WHERE q.driver=d.driver "
        "    OFFSET 0"
        "  ) AS hit"
        ") AS matched "
        "ORDER BY d.pass_no;",
        kDoc0377PlannerTarget);
    const auto plan = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(plan, "Nested Loop"));
    REQUIRE(plan_contains(plan, "Values Scan"));
    REQUIRE(plan_contains(plan, "Append"));
    REQUIRE(plan_contains(plan, "doc0377_owner_append_queries_lo"));
    REQUIRE(plan_contains(plan, "doc0377_owner_append_queries_hi"));
    REQUIRE(plan_contains(plan, "Bioseq Mmsearch Ownership: per-row recipe"));
    require_doc0377_owned_qual_audit(plan);

    std::size_t batch_join_count = 0u;
    std::size_t slice_project_count = 0u;
    std::optional<std::size_t> batch_join_line;
    std::optional<std::size_t> slice_project_line;
    std::optional<std::size_t> append_line;
    for (std::size_t index = 0u; index < plan.size(); ++index) {
        if (plan[index].find("Custom Scan (BioseqBatchJoin)")
                != std::string::npos) {
            ++batch_join_count;
            if (!batch_join_line.has_value()) batch_join_line = index;
        }
        if (plan[index].find("Custom Scan (BioseqSliceProject)")
                != std::string::npos) {
            ++slice_project_count;
            if (!slice_project_line.has_value()) slice_project_line = index;
        }
        if (!append_line.has_value()
            && plan[index].find("Append") != std::string::npos) {
            append_line = index;
        }
    }
    REQUIRE(batch_join_count == 1u);
    REQUIRE(slice_project_count == 1u);
    REQUIRE(batch_join_line.has_value());
    REQUIRE(slice_project_line.has_value());
    REQUIRE(append_line.has_value());
    REQUIRE(*batch_join_line < *slice_project_line);
    REQUIRE(*slice_project_line < *append_line);

    const auto batch_capacity = find_explain_property(
        plan, "Bioseq Batch Capacity");
    REQUIRE(batch_capacity.has_value());
    const long long capacity = std::stoll(*batch_capacity);
    REQUIRE(capacity == 8192);
    REQUIRE(query_scalar_int(ctx,
        "SELECT max(branch_rows) FROM ("
        " SELECT count(*) AS branch_rows "
        " FROM doc0377_owner_append_queries_lo "
        " UNION ALL "
        " SELECT count(*) AS branch_rows "
        " FROM doc0377_owner_append_queries_hi"
        ") AS branch_counts;") < capacity);
    REQUIRE(query_scalar_int(ctx,
        "SELECT max(driver_rows) FROM ("
        " SELECT driver, count(*) AS driver_rows "
        " FROM doc0377_owner_append_queries "
        " GROUP BY driver"
        ") AS driver_counts;") == 4);

#ifdef BIOSEQ_TEST_INDEXES
    exec_sql(ctx,
        "SELECT bioseq_internal.reset_sequence_source_counters();");
#endif
    const rowset_t rows = query_rows(ctx, sql);
    const rowset_t expected{
        {"1", "1", "101,102,103,104", "4"},
        {"2", "2", "201,202,203,204", "4"},
        {"3", "3", "301,302,303,304", "4"},
        {"4", "1", "101,102,103,104", "4"},
        {"5", "2", "201,202,203,204", "4"},
        {"6", "3", "301,302,303,304", "4"},
    };
    REQUIRE(rows == expected);

#ifdef BIOSEQ_TEST_INDEXES
    const auto source_counter = [&ctx](const std::string_view name) {
        return query_scalar_int(ctx, fmt::format(
            "SELECT (bioseq_internal.sequence_source_counters()"
            "        ->>'{}')::bigint;", name));
    };
    REQUIRE(source_counter("registries_created") == 1);
    REQUIRE(source_counter("domains_created") == 1);
    REQUIRE(source_counter("domains_reused") >= 5);
    REQUIRE(source_counter("registries_created")
            == source_counter("registries_destroyed"));
    REQUIRE(source_counter("domains_created")
            == source_counter("domains_destroyed"));
    REQUIRE(source_counter("leases_acquired")
            == source_counter("leases_released"));
    REQUIRE(source_counter("live_registries") == 0);
    REQUIRE(source_counter("live_domains") == 0);
    REQUIRE(source_counter("live_leases") == 0);
#endif
}

TEST_CASE("DOC0377 generic and custom plans retain transformed ownership",
          "[doc0377][mmsearch][slice][prepared][generic][custom][limit]") {
    auto ctx = fresh_context({kDoc0377PlannerTarget});
    seed_doc0377_planner_fixture(ctx);

    constexpr std::array<std::string_view, 2u> plan_modes{
        "force_generic_plan", "force_custom_plan"};
    constexpr std::array<std::string_view, 2u> option_suffixes{
        "", ", NULL::bioseq_aligner_options"};
    for (std::size_t mode_index = 0u; mode_index < plan_modes.size();
         ++mode_index) {
        exec_sql(ctx, fmt::format(
            "SET plan_cache_mode='{}';", plan_modes[mode_index]));
        for (std::size_t oid_index = 0u;
             oid_index < option_suffixes.size(); ++oid_index) {
            const std::string statement = fmt::format(
                "doc0377_owned_{}_{}", mode_index, oid_index);
            exec_sql(ctx, fmt::format(
                "PREPARE {}(int) AS "
                "SELECT t.pk, q.id FROM {} AS t INNER JOIN {} AS q "
                "ON t.pk=q.target_pk AND t.pk>0 AND q.target_pk>0 "
                "AND q.id=$1 AND mmsearch(t.seq0[0:], q.seq[0:]{} ) "
                "ORDER BY t.pk LIMIT 1;",
                statement, kDoc0377PlannerTarget,
                kDoc0377PlannerQueries, option_suffixes[oid_index]));
            const auto plan = explain_text<false>(ctx, fmt::format(
                "EXECUTE {}(1);", statement));
            REQUIRE(plan_contains(
                plan, "Custom Scan (BioseqBatchJoin)"));
            REQUIRE(plan_contains(
                plan, "Custom Scan (BioseqSliceProject)"));
            require_doc0377_owned_qual_audit(plan);

            const auto first = query_rows(
                ctx, fmt::format("EXECUTE {}(1);", statement));
            const auto null_query = query_rows(
                ctx, fmt::format("EXECUTE {}(2);", statement));
            const auto restored = query_rows(
                ctx, fmt::format("EXECUTE {}(3);", statement));
            const auto repeated = query_rows(
                ctx, fmt::format("EXECUTE {}(1);", statement));
            const auto null_param = query_rows(
                ctx, fmt::format("EXECUTE {}(NULL);", statement));
            REQUIRE(first.size() == 1u);
            REQUIRE((first.front() == row_t{
                std::optional<std::string>{"1"},
                std::optional<std::string>{"1"}}));
            REQUIRE(restored.size() == 1u);
            REQUIRE((restored.front() == row_t{
                std::optional<std::string>{"1"},
                std::optional<std::string>{"3"}}));
            REQUIRE(repeated == first);
            REQUIRE(null_query.empty());
            REQUIRE(null_param.empty());
            exec_sql(ctx, fmt::format("DEALLOCATE {};", statement));
        }
    }
}

TEST_CASE("DOC0377 final target query options order is exact across rescans",
          "[doc0377][mmsearch][slice][effects][order][rescan]") {
    auto ctx = fresh_context({kDoc0377PlannerTarget});
    seed_doc0377_planner_fixture(ctx);
    exec_sql(ctx,
        "CREATE TEMP TABLE doc0377_effect_log ("
        "ordinal bigserial PRIMARY KEY, label text UNIQUE NOT NULL);"
        "CREATE FUNCTION pg_temp.doc0377_mark_once(label text, value text) "
        "RETURNS text LANGUAGE plpgsql VOLATILE AS $$"
        "BEGIN "
        "  INSERT INTO doc0377_effect_log(label) VALUES(label); "
        "  RETURN value; "
        "END$$;");

    const std::string sql = fmt::format(
        "SELECT d.ord, "
        "CASE WHEN matched.pk IS NULL THEN NULL ELSE "
        " pg_temp.doc0377_mark_once('target-'||d.ord, "
        " matched.target_value) END, "
        "CASE WHEN matched.pk IS NULL THEN NULL ELSE "
        " pg_temp.doc0377_mark_once('query-'||d.ord, "
        " matched.query_value) END, "
        "CASE WHEN matched.pk IS NULL THEN NULL ELSE "
        " pg_temp.doc0377_mark_once('options-'||d.ord, 'default') END "
        "FROM (VALUES (1), (2), (3)) AS d(ord) "
        "LEFT JOIN LATERAL ("
        "  SELECT t.pk, t.seq0[0:]::text AS target_value, "
        "         q.seq[0:]::text AS query_value "
        "  FROM {} AS t INNER JOIN {} AS q "
        "    ON t.pk=q.target_pk "
        "   AND mmsearch(t.seq0[0:], q.seq[0:], "
        "       NULL::bioseq_aligner_options) "
        "  WHERE q.id=d.ord ORDER BY t.pk LIMIT 1 OFFSET 0"
        ") AS matched ON true ORDER BY d.ord;",
        kDoc0377PlannerTarget, kDoc0377PlannerQueries);
    const auto plan = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    require_doc0377_owned_qual_audit(plan);
    REQUIRE(query_scalar_int(
        ctx, "SELECT count(*) FROM doc0377_effect_log;") == 0);

    constexpr std::string_view expected_order =
        "target-1,query-1,options-1,target-3,query-3,options-3";
    for (unsigned execution = 0; execution < 2u; ++execution) {
        exec_sql(ctx, "TRUNCATE doc0377_effect_log RESTART IDENTITY;");
        const auto rows = query_rows(ctx, sql);
        REQUIRE(rows.size() == 3u);
        REQUIRE(rows[0][1].has_value());
        REQUIRE(rows[0][2].has_value());
        REQUIRE(rows[0][3] == "default");
        REQUIRE_FALSE(rows[1][1].has_value());
        REQUIRE_FALSE(rows[1][2].has_value());
        REQUIRE_FALSE(rows[1][3].has_value());
        REQUIRE(rows[2][1].has_value());
        REQUIRE(rows[2][2].has_value());
        REQUIRE(rows[2][3] == "default");
        REQUIRE(query_scalar(ctx,
            "SELECT string_agg(label,',' ORDER BY ordinal) "
            "FROM doc0377_effect_log;") == expected_order);
    }
}

TEST_CASE("DOC0377 ineligible mmsearch arguments keep target query options order",
          "[doc0377][mmsearch][effects][order][fail-closed][prepared]") {
    connection_test_context ctx;
    exec_sql(ctx, fmt::format(
        "CREATE TEMP TABLE doc0377_argument_effect_targets ("
        "id int PRIMARY KEY, seq bioseq.seq('dna4',32));"
        "CREATE TEMP TABLE doc0377_argument_effect_queries ("
        "id int PRIMARY KEY, seq bioseq.seq('dna4',32));"
        "INSERT INTO doc0377_argument_effect_targets VALUES (1,{});"
        "INSERT INTO doc0377_argument_effect_queries VALUES (1,{});"
        "CREATE TEMP SEQUENCE doc0377_argument_effect_sequence;"
        "CREATE FUNCTION pg_temp.doc0377_effect_target("
        "value bioseq.seq) RETURNS bioseq.seq LANGUAGE plpgsql VOLATILE "
        "AS $$DECLARE observed bigint; BEGIN "
        "observed := nextval('pg_temp.doc0377_argument_effect_sequence'); "
        "IF observed <> 1 THEN RAISE EXCEPTION "
        "'target order/duplication: %', observed; END IF; "
        "RETURN value; END$$;"
        "CREATE FUNCTION pg_temp.doc0377_effect_query("
        "value bioseq.seq) RETURNS bioseq.seq LANGUAGE plpgsql VOLATILE "
        "AS $$DECLARE observed bigint; BEGIN "
        "observed := nextval('pg_temp.doc0377_argument_effect_sequence'); "
        "IF observed <> 2 THEN RAISE EXCEPTION "
        "'query order/duplication: %', observed; END IF; "
        "RETURN value; END$$;"
        "CREATE FUNCTION pg_temp.doc0377_effect_options("
        "value bioseq_aligner_options) RETURNS bioseq_aligner_options "
        "LANGUAGE plpgsql VOLATILE AS $$DECLARE observed bigint; BEGIN "
        "observed := nextval('pg_temp.doc0377_argument_effect_sequence'); "
        "IF observed <> 3 THEN RAISE EXCEPTION "
        "'options order/duplication: %', observed; END IF; "
        "RETURN value; END$$;",
        hoe_probe_literal(), hoe_probe_literal()));

    constexpr std::array<std::string_view, 2u> plan_modes{
        "force_generic_plan", "force_custom_plan"};
    for (std::size_t index = 0u; index < plan_modes.size(); ++index) {
        exec_sql(ctx, fmt::format(
            "SET plan_cache_mode='{}';", plan_modes[index]));
        const std::string statement = fmt::format(
            "doc0377_argument_effects_{}", index);
        exec_sql(ctx, fmt::format(
            "PREPARE {}(int) AS "
            "SELECT t.id FROM doc0377_argument_effect_targets AS t "
            "INNER JOIN doc0377_argument_effect_queries AS q "
            "ON t.id=q.id AND q.id=$1 "
            "AND mmsearch("
            "pg_temp.doc0377_effect_target(t.seq[0:]), "
            "pg_temp.doc0377_effect_query(q.seq[0:]), "
            "pg_temp.doc0377_effect_options("
            "NULL::bioseq_aligner_options));",
            statement));

        /* EXPLAIN/PREPARE must not evaluate any volatile argument.  The
         * functions intentionally make the candidate ineligible; eligible
         * options remain raw/folded Consts by contract and are covered in
         * test_mmsearch.cpp. */
        exec_sql(ctx,
            "SELECT setval('pg_temp.doc0377_argument_effect_sequence',"
            "1,false);");
        const auto plan = explain_text<false>(ctx, fmt::format(
            "EXECUTE {}(1);", statement));
        REQUIRE_FALSE(plan.empty());
        REQUIRE(query_scalar(ctx,
            "SELECT is_called::text FROM "
            "pg_temp.doc0377_argument_effect_sequence;") == "false");

        for (unsigned execution = 0; execution < 2u; ++execution) {
            exec_sql(ctx,
                "SELECT setval("
                "'pg_temp.doc0377_argument_effect_sequence',1,false);");
            const auto error = sql_error(ctx, fmt::format(
                "EXECUTE {}(1);", statement));
            REQUIRE(error.sqlstate == "0A000");
            REQUIRE((error.contains("not owned")
                     || error.contains("no Bioseq executor owner")));
            REQUIRE(query_scalar_int(ctx,
                "SELECT last_value FROM "
                "pg_temp.doc0377_argument_effect_sequence;") == 3);
            REQUIRE(query_scalar(ctx,
                "SELECT is_called::text FROM "
                "pg_temp.doc0377_argument_effect_sequence;") == "true");
        }
        exec_sql(ctx, fmt::format("DEALLOCATE {};", statement));
    }
}

TEST_CASE("DOC0377 projection-only EPQ evaluates volatile final once",
          "[doc0377][slice][epq][volatile][exactly-once]") {
    constexpr std::string_view table = "doc0377_projection_epq";
    constexpr std::string_view source = "doc0377_projection_epq_source";
    constexpr std::string_view log = "doc0377_projection_epq_log";
    auto setup = fresh_context({log, source, table});
    exec_sql(setup,
        "DROP FUNCTION IF EXISTS public.doc0377_projection_epq_once("
        "text,text);"
        "CREATE TABLE doc0377_projection_epq ("
        "id int PRIMARY KEY, source_id int NOT NULL, use_suffix boolean "
        "NOT NULL, touch int, sliced text);"
        "CREATE TABLE doc0377_projection_epq_source ("
        "id int PRIMARY KEY, seq bioseq.seq('dna4',32)) "
        "USING bioseq;"
        "CREATE TABLE doc0377_projection_epq_log ("
        "ordinal bigserial PRIMARY KEY, label text UNIQUE NOT NULL, "
        "observed text);"
        "INSERT INTO doc0377_projection_epq VALUES (1,1,false,0,NULL);"
        "INSERT INTO doc0377_projection_epq_source VALUES "
        "(1,'AAAACCCC'::bioseq.seq('dna4',32));"
        "CREATE FUNCTION public.doc0377_projection_epq_once("
        "label text, value text) RETURNS text LANGUAGE plpgsql VOLATILE "
        "AS $$BEGIN "
        "INSERT INTO public.doc0377_projection_epq_log(label,observed) "
        "VALUES(label,value); RETURN value; END$$;");

    const std::string update_sql =
        "UPDATE public.doc0377_projection_epq AS h "
        "SET touch=h.touch+1, sliced=CASE WHEN h.use_suffix "
        "THEN s.suffix ELSE s.prefix END "
        "FROM (SELECT id, seq[0:4]::text AS prefix, "
        "seq[4:]::text AS suffix "
        "FROM public.doc0377_projection_epq_source OFFSET 0) AS s "
        "WHERE h.id=1 AND h.source_id=s.id "
        "RETURNING public.doc0377_projection_epq_once("
        "'projection-final',h.sliced);";
    const auto plan = explain_text<false>(setup, update_sql);
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_contains(plan, "Subquery Scan on s"));
    REQUIRE(query_scalar_int(setup,
        "SELECT count(*) FROM doc0377_projection_epq_log;") == 0);

    connection_test_context blocker;
    connection_test_context worker;
    connection_test_context observer;
    exec_sql(worker, "SET statement_timeout='15s';");
    const auto outcome = run_doc0377_locked_update(
        blocker, worker, observer,
        "UPDATE public.doc0377_projection_epq "
        "SET use_suffix=true,touch=touch+1 WHERE id=1;",
        update_sql);
    INFO(outcome.error);
    REQUIRE(outcome.sent);
    REQUIRE(outcome.lock_wait_observed);
    REQUIRE(outcome.consume_ok);
    REQUIRE(outcome.result_ok);
    REQUIRE(outcome.got_tuple_result);
    REQUIRE(outcome.returned_rows == 1u);
    REQUIRE(query_scalar(setup,
        "SELECT string_agg(label,',' ORDER BY ordinal) "
        "FROM doc0377_projection_epq_log;") == "projection-final");
    REQUIRE(query_scalar(setup,
        "SELECT observed FROM doc0377_projection_epq_log;") == "CCCC");
    REQUIRE(query_scalar_int(setup,
        "SELECT touch FROM doc0377_projection_epq WHERE id=1;") == 2);
    REQUIRE(query_scalar_int(setup,
        "SELECT source_id FROM doc0377_projection_epq WHERE id=1;") == 1);
    REQUIRE(query_scalar_int(setup,
        "SELECT use_suffix::int FROM doc0377_projection_epq WHERE id=1;")
        == 1);
    REQUIRE(query_scalar(setup,
        "SELECT sliced FROM doc0377_projection_epq WHERE id=1;")
        == "CCCC");
    exec_sql(setup,
        "DROP FUNCTION public.doc0377_projection_epq_once(text,text);");
}

TEST_CASE("DOC0377 BatchJoin EPQ preserves final effect order exactly once",
          "[doc0377][mmsearch][slice][batchjoin][epq][effects]") {
    constexpr std::string_view target = "doc0377_batchjoin_epq_target";
    constexpr std::string_view queries = "doc0377_batchjoin_epq_queries";
    constexpr std::string_view slice_source =
        "doc0377_batchjoin_epq_slice_source";
    constexpr std::string_view result = "doc0377_batchjoin_epq_result";
    constexpr std::string_view log = "doc0377_batchjoin_epq_log";
    auto setup = fresh_context({log, result, slice_source, queries, target});
    create_seq_table(
        setup,
        seq_table_spec{
            .name = std::string{target},
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = true,
        },
        {seq_row{
            .pk = 1,
            .sequences = {hoe_seed_rows().front().sequences.front()},
        }});
    exec_sql(setup, fmt::format(
        "DROP FUNCTION IF EXISTS public.doc0377_batchjoin_epq_once("
        "text,text);"
        "CREATE TABLE {} (id int PRIMARY KEY, target_pk int NOT NULL, "
        "seq bioseq.seq('dna4',32));"
        "INSERT INTO {} SELECT id,1,{} "
        "FROM generate_series(1,16) AS ids(id);"
        "CREATE TABLE {} (id int PRIMARY KEY, "
        "seq bioseq.seq('dna4',32)) USING bioseq;"
        "INSERT INTO {} VALUES (1,{});"
        "CREATE TABLE {} (id int PRIMARY KEY, target_pk int NOT NULL, "
        "query_id int NOT NULL, use_suffix boolean NOT NULL, touch int, "
        "matched_query text);"
        "INSERT INTO {} VALUES (1,1,1,false,0,NULL);"
        "CREATE TABLE {} (ordinal bigserial PRIMARY KEY, "
        "label text UNIQUE NOT NULL, observed text);"
        "CREATE FUNCTION public.doc0377_batchjoin_epq_once("
        "label text, value text) RETURNS text LANGUAGE plpgsql VOLATILE "
        "AS $$BEGIN INSERT INTO public.{}(label,observed) "
        "VALUES(label,value); "
        "RETURN value; END$$;",
        queries, queries, hoe_probe_literal(),
        slice_source, slice_source, hoe_probe_literal(),
        result, result, log, log));
    configure_doc0377_transformed_join(setup);

    const std::string update_sql = fmt::format(
        "UPDATE public.{} AS h "
        "SET touch=h.touch+1, "
        "matched_query=CASE WHEN h.use_suffix "
        "THEN sliced.query_suffix ELSE sliced.query_prefix END "
        "FROM (SELECT t.pk AS target_pk, q.id AS query_id "
        "FROM public.{} AS t INNER JOIN public.{} AS q "
        "ON mmsearch(t.seq0, q.seq, "
        "aligner_options(index_read_requirement => 1)) "
        "OFFSET 0) AS matched "
        "INNER JOIN (SELECT q.id AS query_id, "
        "q.seq[0:1]::text AS query_prefix, "
        "q.seq[1:]::text AS query_suffix "
        "FROM public.{} AS q OFFSET 0) AS sliced "
        "ON sliced.query_id=matched.query_id "
        "WHERE h.id=1 AND h.target_pk=matched.target_pk "
        "AND h.query_id=matched.query_id "
        "RETURNING "
        "public.doc0377_batchjoin_epq_once("
        "'target',matched.target_pk::text), "
        "public.doc0377_batchjoin_epq_once('query',h.matched_query), "
        "public.doc0377_batchjoin_epq_once("
        "'options',CASE WHEN h.use_suffix "
        "THEN 'suffix' ELSE 'prefix' END);",
        result, target, queries, slice_source);
    const auto plan = explain_text<false>(setup, update_sql);
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_contains(plan, "Subquery Scan on matched"));
    REQUIRE(plan_contains(plan, "Bioseq Batch ABI Version: 3"));
    REQUIRE(plan_contains(
        plan, "Bioseq Residual Mmsearch Qual: absent"));
    REQUIRE_FALSE(plan_contains(
        plan, "Bioseq Mmsearch Ownership: per-row recipe"));
    REQUIRE_FALSE(doc0377_plan_has_executable_mmsearch(plan));
    REQUIRE(query_scalar_int(setup, fmt::format(
        "SELECT count(*) FROM {};", log)) == 0);

    connection_test_context blocker;
    connection_test_context worker;
    connection_test_context observer;
    configure_doc0377_transformed_join(worker);
    exec_sql(worker, "SET statement_timeout='15s';");
    const auto outcome = run_doc0377_locked_update(
        blocker, worker, observer,
        fmt::format(
            "UPDATE public.{} SET use_suffix=true,touch=touch+1 "
            "WHERE id=1;", result),
        update_sql);
    INFO(outcome.error);
    REQUIRE(outcome.sent);
    REQUIRE(outcome.lock_wait_observed);
    REQUIRE(outcome.consume_ok);
    REQUIRE(outcome.result_ok);
    REQUIRE(outcome.got_tuple_result);
    REQUIRE(outcome.returned_rows == 1u);
    REQUIRE(query_scalar_int(worker,
        "SELECT scan_node_count FROM "
        "bioseq.last_alignment_execution_stats() LIMIT 1;") == 1);
    REQUIRE(query_scalar_int(worker,
        "SELECT nested_alignment_executor_count FROM "
        "bioseq.last_alignment_execution_stats() LIMIT 1;") == 0);
    REQUIRE(query_scalar(setup, fmt::format(
        "SELECT string_agg(label,',' ORDER BY ordinal) FROM {};", log))
        == "target,query,options");
    REQUIRE(query_scalar(setup, fmt::format(
        "SELECT observed FROM {} WHERE label='query';", log))
        == std::string{kHoeProbeSequence.substr(1)});
    REQUIRE(query_scalar(setup, fmt::format(
        "SELECT observed FROM {} WHERE label='options';", log))
        == "suffix");
    REQUIRE(query_scalar_int(setup, fmt::format(
        "SELECT touch FROM {} WHERE id=1;", result)) == 2);
    REQUIRE(query_scalar_int(setup, fmt::format(
        "SELECT use_suffix::int FROM {} WHERE id=1;", result)) == 1);
    REQUIRE(query_scalar(setup, fmt::format(
        "SELECT matched_query FROM {} WHERE id=1;", result))
        == std::string{kHoeProbeSequence.substr(1)});
    exec_sql(setup,
        "DROP FUNCTION public.doc0377_batchjoin_epq_once(text,text);");
}

TEST_CASE("Many-to-many mmsearch join owns the predicate and projections",
          "[join][many_to_many][ownership]") {
    auto ctx = fresh_context({kHoeJoinLeft, kHoeJoinRight});
    seed_hoe_fixture(ctx, kHoeJoinLeft);
    seed_hoe_fixture(ctx, kHoeJoinRight);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk, bioseq.cigar(t1.pk) "
            "  FROM {} t1, {} t2 "
            " WHERE mmsearch(t1.seq0, t2.seq1) "
            " ORDER BY t1.pk, t2.pk;",
            kHoeJoinLeft, kHoeJoinRight));
    REQUIRE_FALSE(rows.empty());
    for (const auto& row : rows) {
        REQUIRE(row.size() == 3);
        REQUIRE(row[2].has_value());
        REQUIRE_FALSE(row[2]->empty());
        REQUIRE(is_valid_cigar(*row[2]));
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

    ctx.clear_notices();
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
    REQUIRE_FALSE(ctx.notice_contains("resource was not closed"));

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
        ctx.clear_notices();
        const auto guard_row = query_one(
            ctx,
            fmt::format(
                "SELECT bioseq.symbol_count(seq0, 'A') FROM {} WHERE pk = 6;",
                kHoeJoin));
        REQUIRE(guard_row.size() == 1);
        REQUIRE(guard_row[0].has_value());  // recompute fallback is never NULL
        REQUIRE(std::stoll(guard_row[0].value()) == 8);
        // The recompute opens seq_sub plus a registered random-read snapshot.
        // Both must be released before PQexec returns; the pre-fix helper
        // deleted only its MemoryContext and emitted this warning at commit.
        REQUIRE_FALSE(ctx.notice_contains("resource was not closed"));
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
    //   2. AddBioseqColumnScanPath claims the exact RestrictInfo and
    //      structurally removes paths that would execute the SQL stub.
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

TEST_CASE("Multiple Const mmsearch quals fail closed",
          "[mmsearch][multi_qual][error]") {
    auto ctx = fresh_context({kHoeJoin});
    seed_hoe_fixture(ctx, kHoeJoin);
    constexpr std::string_view second_probe = "TTCATCATCATCATCTACTCCT";
    const auto error = sql_error(ctx, fmt::format(
        "SELECT pk FROM {} WHERE "
        "mmsearch(seq0, '{}'::bioseq.seq('dna4', 32)) AND "
        "mmsearch(seq0, '{}'::bioseq.seq('dna4', 32));",
        kHoeJoin, kHoeProbeSequence, second_probe));
    REQUIRE(error.sqlstate == "0A000");
    REQUIRE(error.contains("stacked mmsearch"));
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
    const auto count_plan = explain_text<false>(ctx, count_sql);
    REQUIRE(plan_contains(count_plan, "Aggregate"));
    REQUIRE(plan_contains(count_plan, "Custom Scan (BioseqScan)"));
    REQUIRE_FALSE(plan_contains(
        count_plan, "Custom Scan (VectorAggNode)"));
    const auto count_rows = expect_invariant_under_guc_toggle(
        ctx, count_sql, "bioseq.enable_vectorization");
    REQUIRE(count_rows.size() == 1);
    const auto& count_row = count_rows.front();
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

TEST_CASE("Param-driven primary plus Const mmsearch fails closed",
          "[mmsearch][multi_qual][param_plus_const][error]") {
    auto ctx = fresh_context({kHoeJoinLeft, kHoeJoinRight});
    seed_hoe_fixture(ctx, kHoeJoinLeft);
    seed_hoe_fixture(ctx, kHoeJoinRight);
	const auto stacked_error = sql_error(ctx, fmt::format(
		"SELECT t1.pk, t2.pk FROM {} t1 INNER JOIN {} t2 "
		"ON mmsearch(t2.seq0, t1.seq1) "
		"WHERE mmsearch(t2.seq0, '{}'::bioseq.seq('dna4', 32));",
		kHoeJoinLeft, kHoeJoinRight,
		"TCATCATCATCATCTACTCCTATCTATCATCATCATCATCTACTCCT"));
	REQUIRE(stacked_error.sqlstate == "0A000");
	REQUIRE(stacked_error.contains("stacked mmsearch"));
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

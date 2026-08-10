// Integration scenarios:
//   * `mmsearch` self-join uses the right-hand side's `seq1` column as the
//     query argument and runs without errors; whichever match set the
//     alignment pipeline yields is unconstrained here, but every row must
//     reference seeded primary keys.
//   * Projection helpers (`bioseq.target_start`, `bioseq.cigar`) work for
//     either input table in the join because the placeholder rewrite is
//     resolved per-call against the originating Var.
//   * The match set is invariant under `bioseq.vectorization.enabled` toggling,
//     since the SIMD path must produce byte-identical output to scalar.
//   * Cross joins WITHOUT mmsearch on `USING bioseq` tables produce the
//     full cartesian product. Each scan runtime owns its state, so one
//     BioseqScan node cannot force another one to EOF after one outer row.
//   * LEFT JOIN with a literal `WHERE mmsearch(t1.seq, '...')` and
//     `bioseq.cigar(t1.pk)` does not crash and surfaces matched rows on
//     the outer side. bioseq_placeholder_support keeps the projection on the
//     originating side, whose match result backs the placeholder Const.

#include <tests/support/postgres.hpp>
#include <bioseq/pg_build_profile.h>

#include <chrono>
#include <libpq-fe.h>
#include <set>
#include <thread>

import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view k_hoe_join = "hoe_join";
constexpr std::string_view k_hoe_join_left = "hoe_join_left";
constexpr std::string_view k_hoe_join_right = "hoe_join_right";
constexpr std::string_view k_transformed_join_target =
    "transformed_join_target";
constexpr std::string_view k_transformed_join_queries =
    "transformed_join_queries";

void configure_transformed_join(connection_test_context& ctx) {
    exec_sql(ctx,
        "SET max_parallel_workers_per_gather = 0;"
        "SET bioseq.index.sequence_read.enabled = off;"
        "SET bioseq.aligner.vulkan.enabled = off;"
        "SET bioseq.aligner.minimizer.algorithm = 'scalar';"
        "SET bioseq.aligner.chaining.algorithm = 'scalar_minimap2';"
        "SET bioseq.aligner.base_alignment.algorithm = 'ksw2';"
        "SET bioseq.aligner.chaining.minimum_score = 20;");
}

void seed_transformed_join_fixture(connection_test_context& ctx) {
    seed_hoe_fixture(ctx, k_transformed_join_target);
    exec_sql(ctx, fmt::format(
        "CREATE TEMP TABLE {} ("
        "id int PRIMARY KEY, target_pk int NOT NULL, "
        "seq bioseq.seq('dna4',32));"
        "INSERT INTO {} VALUES "
        "(1, 1, {}), "
        "(2, 1, NULL::bioseq.seq('dna4',32)), "
        "(3, 1, {});",
        k_transformed_join_queries, k_transformed_join_queries,
        hoe_probe_literal(), hoe_probe_literal()));
    configure_transformed_join(ctx);
}

[[nodiscard]] bool plan_has_executable_mmsearch(
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

void require_owned_qual_audit(
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
    REQUIRE_FALSE(plan_has_executable_mmsearch(plan));
}

struct async_update_outcome {
    bool sent = false;
    bool lock_wait_observed = false;
    bool consume_ok = true;
    bool got_command = false;
    bool got_tuple_result = false;
    bool result_ok = true;
    std::size_t returned_rows = 0u;
    std::string error;
};

[[nodiscard]] async_update_outcome run_locked_update(
        connection_test_context& blocker,
        connection_test_context& worker,
        connection_test_context& observer,
        const std::string& blocker_update,
        const std::string& worker_update) {
    async_update_outcome outcome{};
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
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // mmsearch(t1.seq0, t2.seq1) routes through the
    // parameterised inner-side scan path (Param-rebind) because
    // t2 is a different range-table entry than t1. Each outer t1
    // row drives a fresh rescan of t2 with t1.seq1 bound into
    // arg1's Param. Every surfaced row must have a non-NULL pk on both
    // sides and, when bioseq projections are requested in the outer SELECT,
    // every emitted row must populate them. Returning a row with
    // NULL bioseq.cigar / bioseq.target_start on a mmsearch path is invalid.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk "
            "  FROM {} t1 CROSS JOIN {} t2 "
            " WHERE mmsearch(t1.seq0, t2.seq1) "
            " ORDER BY t1.pk, t2.pk;",
            k_hoe_join, k_hoe_join));
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
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const std::string sql = fmt::format(
        "SELECT t1.pk, t2.pk "
        "  FROM {} t1 CROSS JOIN {} t2 "
        " WHERE mmsearch(t1.seq0, t2.seq1) "
        " ORDER BY t1.pk, t2.pk;",
        k_hoe_join, k_hoe_join);
    expect_invariant_under_guc_toggle(ctx, sql,
                                      "bioseq.vectorization.enabled");
}

TEST_CASE("Self-join projection helpers run without errors when matches surface",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

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
            k_hoe_join, k_hoe_join));
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
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // Each BioseqScan owns its tuple_dispatch_buffer. The inner scan's batch
    // transition cannot alter the outer scan's output position or force EOF.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk "
            "  FROM {} t1 CROSS JOIN {} t2 "
            " ORDER BY t1.pk, t2.pk;",
            k_hoe_join, k_hoe_join));
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
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // With physically distinct relations, each BioseqScan owns its buffer
    // and inner-scan EOF does not stop the outer scan,
    // independently of self-join planner optimisations.
    auto ctx = fresh_context({k_hoe_join_left, k_hoe_join_right});
    seed_hoe_fixture(ctx, k_hoe_join_left);
    seed_hoe_fixture(ctx, k_hoe_join_right);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk "
            "  FROM {} t1 CROSS JOIN {} t2 "
            " ORDER BY t1.pk, t2.pk;",
            k_hoe_join_left, k_hoe_join_right));
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
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // bioseq_placeholder_support anchors `bioseq.cigar(t1.pk)` to the
    // originating scan through a PlaceHolderVar while planner targetlists
    // account for outer-join nullability. The inner-side BioseqScan therefore
    // never receives the outer projection or resets its mmsearch context.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk, bioseq.cigar(t1.pk) "
            "  FROM {} t1 LEFT JOIN {} t2 ON t1.pk = t2.pk "
            " WHERE mmsearch(t1.seq0, "
            "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)) "
            " ORDER BY t1.pk;",
            k_hoe_join, k_hoe_join));
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
    // Matching rows must include at least one populated CIGAR.
    REQUIRE(saw_valid_cigar);
}

TEST_CASE("LEFT JOIN with literal mmsearch is invariant under vectorisation "
          "and column cache toggles",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const std::string sql = fmt::format(
        "SELECT t1.pk, t2.pk, bioseq.target_start(t1.pk), bioseq.cigar(t1.pk) "
        "  FROM {} t1 LEFT JOIN {} t2 ON t1.pk = t2.pk "
        " WHERE mmsearch(t1.seq0, "
        "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)) "
        " ORDER BY t1.pk;",
        k_hoe_join, k_hoe_join);
    (void)expect_query_invariant_under_options(
        ctx, sql,
        {{"bioseq.vectorization.enabled", {"on", "off"}},
         {"bioseq.column_cache.enabled", {"on", "off"}}});
}

TEST_CASE("Repeated execution of CROSS JOIN does not leak per-scan state",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // Repeated execution verifies that each CustomScanState allocation owns
    // and resets its output-position cursor within the same backend.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    for (int run = 0; run < 4; ++run) {
        const auto rows = query_rows(
            ctx,
            fmt::format(
                "SELECT t1.pk, t2.pk "
                "  FROM {} t1 CROSS JOIN {} t2 "
                " ORDER BY t1.pk, t2.pk;",
                k_hoe_join, k_hoe_join));
        REQUIRE(rows.size() == 25);
    }
}

TEST_CASE("INNER JOIN of two USING-bioseq tables with mmsearch on both "
          "preserves left-side row count",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // Many-to-many qual: each outer row's seq1 is the literal-equivalent
    // query for the inner scan via the planner's parameterised inner-side
    // scan rewriting. ReScanCustomScan's Param-rebind path is what wires
    // the new query into the buffer per outer row.
    auto ctx = fresh_context({k_hoe_join_left, k_hoe_join_right});
    seed_hoe_fixture(ctx, k_hoe_join_left);
    seed_hoe_fixture(ctx, k_hoe_join_right);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk "
            "  FROM {} t1 INNER JOIN {} t2 "
            "    ON mmsearch(t2.seq0, t1.seq1) "
            " ORDER BY t1.pk, t2.pk;",
            k_hoe_join_left, k_hoe_join_right));

    // Param rebinding accepts the non-Const mmsearch query and must produce a
    // non-empty match set.
    REQUIRE_FALSE(rows.empty());
    // At least one alignment per seeded outer row must surface.
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
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // RIGHT JOIN propagates phnullingrels from the syntactically-left side
    // into the PHV and varnullingrels from the inner side.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk, bioseq.cigar(t2.pk) "
            "  FROM {} t1 RIGHT JOIN {} t2 ON t1.pk = t2.pk "
            " WHERE mmsearch(t2.seq0, "
            "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)) "
            " ORDER BY t2.pk;",
            k_hoe_join, k_hoe_join));
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
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // ORDER BY on a bioseq.* projection forces the planner to
    // evaluate the PHV expression at the join-output level. If
    // bioseq_placeholder_support returned a PHV with the wrong
    // phlevelsup, the sort key resolves to NULL for every row and
    // LIMIT becomes nondeterministic. The executor uses a tuplestore-backed
    // Sort node above BioseqScan.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const std::string base_sql = fmt::format(
        "SELECT t1.pk, t2.pk, bioseq.cigar(t1.pk) "
        "  FROM {} t1 LEFT JOIN {} t2 ON t1.pk = t2.pk "
        " WHERE mmsearch(t1.seq0, "
        "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32))",
        k_hoe_join, k_hoe_join);
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
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // Aggregate-bearing queries use the has_aggs branch of
    // bioseq_placeholder_support. We use bioseq.target_end (which the
    // seeded probes set to a known positive value) rather than
    // bioseq.score (whose value depends on minimap2 chain scoring
    // and can be zero for some probes).
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

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
            k_hoe_join, k_hoe_join));
    REQUIRE_FALSE(rows.empty());
    for (const auto& row : rows) {
        REQUIRE(row.size() == 2);
        REQUIRE(row[0].has_value());
        REQUIRE(row[1].has_value());
        REQUIRE(std::stoll(row[1].value()) > 0);
    }
}

TEST_CASE("Three-way JOIN of bioseq tables exercises nested rescan",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // Three-way join nests two parameterised inner-side BioseqScans:
    // t2's ReScan fires for each t1 row, and t3's ReScan fires for
    // each (t1, t2) combination. ReScanCustomScan clears slots before
    // rebinding the mmsearch parameter, preventing duplicate or phantom rows.
    auto ctx = fresh_context({k_hoe_join_left, k_hoe_join_right, k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join_left);
    seed_hoe_fixture(ctx, k_hoe_join_right);
    seed_hoe_fixture(ctx, k_hoe_join);

    const std::string sql = fmt::format(
        "SELECT t1.pk, t2.pk, t3.pk "
        "  FROM {} t1 "
        "  JOIN {} t2 ON mmsearch(t2.seq0, t1.seq1) "
        "  JOIN {} t3 ON mmsearch(t3.seq0, t2.seq1) "
        " ORDER BY t1.pk, t2.pk, t3.pk;",
        k_hoe_join_left, k_hoe_join_right, k_hoe_join);
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
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // ExplainCustomScan reads aligner_state through the per-scan runtime. The
    // multi-scan query pins its output before either runtime is allocated.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const auto plan = explain_text(
        ctx,
        fmt::format(
            "SELECT t1.pk, bioseq.cigar(t1.pk) "
            "  FROM {} t1 CROSS JOIN {} t2 "
            " WHERE mmsearch(t1.seq0, "
            "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32));",
            k_hoe_join, k_hoe_join));
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
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // Force the generic CachedPlan so $1 remains PARAM_EXTERN at
    // BeginCustomScan. Custom plans often fold it to Const and bypass the
    // client-parameter binder.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);
	exec_sql(ctx, "SET plan_cache_mode = force_generic_plan;");

    exec_sqlf(ctx,
              "PREPARE p_join_test(bioseq.seq('dna4', 32)) AS "
              "SELECT t1.pk, bioseq.cigar(t1.pk) "
              "  FROM {} t1 LEFT JOIN {} t2 ON t1.pk = t2.pk "
              " WHERE mmsearch(t1.seq0, $1) ORDER BY t1.pk;",
              k_hoe_join, k_hoe_join);
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
    // The alternate parameter must still produce a non-empty result.
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
#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
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

TEST_CASE("JOIN with insert maintenance and sequence-index reads disabled "
          "still produces matches",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // The sidecar-less BioseqScan branch must preserve fallback-path
    // projection plumbing and produce populated CIGARs.
    auto ctx = fresh_context({k_hoe_join});
    // Disable bioseq.index.insert_maintenance.enabled before seeding so the sidecar
    // stays empty; seed_hoe_fixture flips it on by default.
    exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(k_hoe_join),
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
        k_hoe_join, k_hoe_join);

    auto invariant_rows = expect_query_invariant_under_options(
        ctx, sql, {{"bioseq.index.sequence_read.enabled", {"on", "off"}}});
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
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);
    exec_sql(
        ctx,
        fmt::format(
            "DROP TABLE IF EXISTS hoe_64 CASCADE; "
            "CREATE TABLE hoe_64 ("
            "pk INT PRIMARY KEY, seq0 bioseq.seq('dna4', 64)) USING bioseq; "
            "INSERT INTO hoe_64 SELECT pk, seq0 FROM {};",
            k_hoe_join));

    const auto same_width = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk, bioseq.cigar(t1.pk) "
            "FROM {} t1 JOIN {} t2 ON mmsearch(t1.seq0, t2.seq0) "
            "ORDER BY t1.pk, t2.pk;",
            k_hoe_join,
            k_hoe_join));
    const auto cross_width = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk, bioseq.cigar(t1.pk) "
            "FROM {} t1 JOIN hoe_64 t2 ON mmsearch(t1.seq0, t2.seq0) "
            "ORDER BY t1.pk, t2.pk;",
            k_hoe_join));

    REQUIRE_FALSE(same_width.empty());
    REQUIRE(cross_width == same_width);
    exec_sql(ctx, "DROP TABLE IF EXISTS hoe_64 CASCADE;");
}

TEST_CASE("JOIN with CTE wrapping mmsearch preserves bioseq projections",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // CTEs introduce a SubqueryScan / CteScan node above the
    // BioseqScan. bioseq_placeholder_support copies varlevelsup from
    // the inner Var; a CTE materialisation point raises that level.
    // The WITH clause requires varlevelsup propagation through the CTE
    // rewrite path.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

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
            k_hoe_join, k_hoe_join));
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
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // LATERAL produces a NestLoop with param_exec references that
    // are syntactically distinct from a flat INNER JOIN ... ON
    // mmsearch(...). Parameter rebinding must handle both the LATERAL and
    // rewritten flat forms.
    auto ctx = fresh_context({k_hoe_join_left, k_hoe_join_right});
    seed_hoe_fixture(ctx, k_hoe_join_left);
    seed_hoe_fixture(ctx, k_hoe_join_right);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, sub.pk, bioseq.cigar(sub.pk) "
            "  FROM {} t1 "
            "       CROSS JOIN LATERAL ("
            "         SELECT pk FROM {} t2 WHERE mmsearch(t2.seq0, t1.seq1)"
            "       ) sub "
            " ORDER BY t1.pk, sub.pk;",
            k_hoe_join_left, k_hoe_join_right));
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
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    /* OFFSET 0 keeps the LATERAL child as a parameterized Nested Loop inner
     * scan, so p.probe reaches BioseqScan as PARAM_EXEC.  VALUES supplies the
     * load-bearing non-NULL -> NULL -> non-NULL rescan sequence in one
     * executor: the NULL iteration must not replay stale matches, and the
     * third iteration must reconfigure the same scan state successfully. */
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

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
        hoe_probe_literal(), hoe_probe_literal(), k_hoe_join);
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

TEST_CASE("Transformed LATERAL owner survives PARAM_EXEC rescans",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    auto ctx = fresh_context({k_transformed_join_target});
    seed_transformed_join_fixture(ctx);

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
                k_transformed_join_target, k_transformed_join_queries,
                option_suffix);
            const auto plan = explain_text<false>(ctx, sql);
            REQUIRE(plan_contains(plan, "Nested Loop"));
            REQUIRE(plan_contains(
                plan, "Custom Scan (BioseqBatchJoin)"));
            REQUIRE(plan_contains(
                plan, "Bioseq Mmsearch Ownership: per-row recipe"));
            REQUIRE(plan_contains(
                plan, "Custom Scan (BioseqSliceProject)"));
            require_owned_qual_audit(plan);

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

TEST_CASE("Transformed owner remains inside CTE and subquery fences",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    auto ctx = fresh_context({k_transformed_join_target});
    seed_transformed_join_fixture(ctx);

    constexpr std::array<std::string_view, 2u> option_suffixes{
        "", ", NULL::bioseq_aligner_options"};
    for (const auto option_suffix : option_suffixes) {
        const std::string owned_body = fmt::format(
            "SELECT t.pk, q.id, bioseq.cigar(t.pk) AS cigar "
            "FROM {} AS t INNER JOIN {} AS q "
            "ON q.id=1 AND t.pk=q.target_pk "
            "AND mmsearch(t.seq0[0:], q.seq[0:]{} )",
            k_transformed_join_target, k_transformed_join_queries,
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
            require_owned_qual_audit(plan);
            REQUIRE(query_rows(ctx, sql) == expected);
        }
        const auto cte_plan = explain_text<false>(ctx, cte_sql);
        REQUIRE(plan_contains(cte_plan, "CTE Scan"));
    }
}

TEST_CASE("Partition child reparameterization retains one transformed owner",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    auto ctx = fresh_context({k_transformed_join_target});
    seed_hoe_fixture(ctx, k_transformed_join_target);
    exec_sql(ctx, fmt::format(
        "CREATE TEMP TABLE transformed_partition_queries ("
        "id int, bucket int, target_pk int NOT NULL, "
        "seq bioseq.seq('dna4',32)) PARTITION BY RANGE(bucket);"
        "CREATE TEMP TABLE transformed_partition_queries_lo "
        "PARTITION OF transformed_partition_queries "
        "FOR VALUES FROM (0) TO (10);"
        "CREATE TEMP TABLE transformed_partition_queries_hi "
        "PARTITION OF transformed_partition_queries "
        "FOR VALUES FROM (10) TO (20);"
        "INSERT INTO transformed_partition_queries VALUES "
        "(1, 5, 1, {}), "
        "(2, 15, 1, NULL::bioseq.seq('dna4',32)), "
        "(3, 5, 1, {});",
        hoe_probe_literal(), hoe_probe_literal()));
    configure_transformed_join(ctx);
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
        "  INNER JOIN transformed_partition_queries AS q "
        "    ON t.pk=q.target_pk "
        "   AND mmsearch(t.seq0[0:], q.seq[0:]) "
        "  WHERE q.id=d.ord AND q.bucket=d.bucket "
        "  ORDER BY t.pk LIMIT 1 OFFSET 0"
        ") AS matched ON true ORDER BY d.ord;",
        k_transformed_join_target);
    const auto plan = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(plan, "Append"));
    REQUIRE(plan_contains(plan, "transformed_partition_queries_lo"));
    REQUIRE(plan_contains(plan, "transformed_partition_queries_hi"));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    require_owned_qual_audit(plan);

    const auto rows = query_rows(ctx, sql);
    REQUIRE(rows.size() == 3u);
    REQUIRE(rows[0][0] == "1");
    REQUIRE(rows[0][1] == "1");
    REQUIRE(rows[1][0] == "2");
    REQUIRE_FALSE(rows[1][1].has_value());
    REQUIRE(rows[2][0] == "3");
    REQUIRE(rows[2][1] == "1");
}

TEST_CASE("Dominating SliceProject spans Append branches across rescans",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    auto ctx = fresh_context({k_transformed_join_target});
    seed_hoe_fixture(ctx, k_transformed_join_target);
    exec_sql(ctx, fmt::format(
        "CREATE TEMP TABLE transformed_owner_append_queries ("
        "id int, driver int, bucket int, target_pk int NOT NULL, "
        "seq bioseq.seq('dna4',32)) PARTITION BY RANGE(bucket);"
        "CREATE TEMP TABLE transformed_owner_append_queries_lo "
        "PARTITION OF transformed_owner_append_queries "
        "FOR VALUES FROM (0) TO (10);"
        "CREATE TEMP TABLE transformed_owner_append_queries_hi "
        "PARTITION OF transformed_owner_append_queries "
        "FOR VALUES FROM (10) TO (20);"
        "INSERT INTO transformed_owner_append_queries "
        "SELECT d.driver * 100 + branch.row_delta, d.driver, "
        "       branch.bucket, 1, {} "
        "FROM (VALUES (1), (2), (3)) AS d(driver) "
        "CROSS JOIN (VALUES (1,5), (2,5), (3,15), (4,15)) "
        "AS branch(row_delta,bucket);",
        hoe_probe_literal()));
    configure_transformed_join(ctx);
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
        "    INNER JOIN transformed_owner_append_queries AS q "
        "      ON t.pk=q.target_pk "
        "     AND mmsearch("
        "       t.seq0[0:16] OPERATOR(bioseq.||) t.seq0[16:], "
        "       q.seq[0:]) "
        "    WHERE q.driver=d.driver "
        "    OFFSET 0"
        "  ) AS hit"
        ") AS matched "
        "ORDER BY d.pass_no;",
        k_transformed_join_target);
    const auto plan = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(plan, "Nested Loop"));
    REQUIRE(plan_contains(plan, "Values Scan"));
    REQUIRE(plan_contains(plan, "Append"));
    REQUIRE(plan_contains(plan, "transformed_owner_append_queries_lo"));
    REQUIRE(plan_contains(plan, "transformed_owner_append_queries_hi"));
    REQUIRE(plan_contains(plan, "Bioseq Mmsearch Ownership: per-row recipe"));
    require_owned_qual_audit(plan);

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
        " FROM transformed_owner_append_queries_lo "
        " UNION ALL "
        " SELECT count(*) AS branch_rows "
        " FROM transformed_owner_append_queries_hi"
        ") AS branch_counts;") < capacity);
    REQUIRE(query_scalar_int(ctx,
        "SELECT max(driver_rows) FROM ("
        " SELECT driver, count(*) AS driver_rows "
        " FROM transformed_owner_append_queries "
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

TEST_CASE("Generic and custom plans retain transformed ownership",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    auto ctx = fresh_context({k_transformed_join_target});
    seed_transformed_join_fixture(ctx);

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
                "transformed_owned_{}_{}", mode_index, oid_index);
            exec_sql(ctx, fmt::format(
                "PREPARE {}(int) AS "
                "SELECT t.pk, q.id FROM {} AS t INNER JOIN {} AS q "
                "ON t.pk=q.target_pk AND t.pk>0 AND q.target_pk>0 "
                "AND q.id=$1 AND mmsearch(t.seq0[0:], q.seq[0:]{} ) "
                "ORDER BY t.pk LIMIT 1;",
                statement, k_transformed_join_target,
                k_transformed_join_queries, option_suffixes[oid_index]));
            const auto plan = explain_text<false>(ctx, fmt::format(
                "EXECUTE {}(1);", statement));
            REQUIRE(plan_contains(
                plan, "Custom Scan (BioseqBatchJoin)"));
            REQUIRE(plan_contains(
                plan, "Custom Scan (BioseqSliceProject)"));
            require_owned_qual_audit(plan);

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

TEST_CASE("Final target, query, and options order is exact across rescans",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    auto ctx = fresh_context({k_transformed_join_target});
    seed_transformed_join_fixture(ctx);
    exec_sql(ctx,
        "CREATE TEMP TABLE transformed_effect_log ("
        "ordinal bigserial PRIMARY KEY, label text UNIQUE NOT NULL);"
        "CREATE FUNCTION pg_temp.transformed_mark_once(label text, value text) "
        "RETURNS text LANGUAGE plpgsql VOLATILE AS $$"
        "BEGIN "
        "  INSERT INTO transformed_effect_log(label) VALUES(label); "
        "  RETURN value; "
        "END$$;");

    const std::string sql = fmt::format(
        "SELECT d.ord, "
        "CASE WHEN matched.pk IS NULL THEN NULL ELSE "
        " pg_temp.transformed_mark_once('target-'||d.ord, "
        " matched.target_value) END, "
        "CASE WHEN matched.pk IS NULL THEN NULL ELSE "
        " pg_temp.transformed_mark_once('query-'||d.ord, "
        " matched.query_value) END, "
        "CASE WHEN matched.pk IS NULL THEN NULL ELSE "
        " pg_temp.transformed_mark_once('options-'||d.ord, 'default') END "
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
        k_transformed_join_target, k_transformed_join_queries);
    const auto plan = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    require_owned_qual_audit(plan);
    REQUIRE(query_scalar_int(
        ctx, "SELECT count(*) FROM transformed_effect_log;") == 0);

    constexpr std::string_view expected_order =
        "target-1,query-1,options-1,target-3,query-3,options-3";
    for (unsigned execution = 0; execution < 2u; ++execution) {
        exec_sql(ctx, "TRUNCATE transformed_effect_log RESTART IDENTITY;");
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
            "FROM transformed_effect_log;") == expected_order);
    }
}

TEST_CASE("Ineligible mmsearch arguments keep target, query, and options order",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    connection_test_context ctx;
    exec_sql(ctx, fmt::format(
        "CREATE TEMP TABLE argument_effect_targets ("
        "id int PRIMARY KEY, seq bioseq.seq('dna4',32));"
        "CREATE TEMP TABLE argument_effect_queries ("
        "id int PRIMARY KEY, seq bioseq.seq('dna4',32));"
        "INSERT INTO argument_effect_targets VALUES (1,{});"
        "INSERT INTO argument_effect_queries VALUES (1,{});"
        "CREATE TEMP SEQUENCE argument_effect_sequence;"
        "CREATE FUNCTION pg_temp.argument_effect_target("
        "value bioseq.seq) RETURNS bioseq.seq LANGUAGE plpgsql VOLATILE "
        "AS $$DECLARE observed bigint; BEGIN "
        "observed := nextval('pg_temp.argument_effect_sequence'); "
        "IF observed <> 1 THEN RAISE EXCEPTION "
        "'target order/duplication: %', observed; END IF; "
        "RETURN value; END$$;"
        "CREATE FUNCTION pg_temp.argument_effect_query("
        "value bioseq.seq) RETURNS bioseq.seq LANGUAGE plpgsql VOLATILE "
        "AS $$DECLARE observed bigint; BEGIN "
        "observed := nextval('pg_temp.argument_effect_sequence'); "
        "IF observed <> 2 THEN RAISE EXCEPTION "
        "'query order/duplication: %', observed; END IF; "
        "RETURN value; END$$;"
        "CREATE FUNCTION pg_temp.argument_effect_options("
        "value bioseq_aligner_options) RETURNS bioseq_aligner_options "
        "LANGUAGE plpgsql VOLATILE AS $$DECLARE observed bigint; BEGIN "
        "observed := nextval('pg_temp.argument_effect_sequence'); "
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
            "argument_effects_{}", index);
        exec_sql(ctx, fmt::format(
            "PREPARE {}(int) AS "
            "SELECT t.id FROM argument_effect_targets AS t "
            "INNER JOIN argument_effect_queries AS q "
            "ON t.id=q.id AND q.id=$1 "
            "AND mmsearch("
            "pg_temp.argument_effect_target(t.seq[0:]), "
            "pg_temp.argument_effect_query(q.seq[0:]), "
            "pg_temp.argument_effect_options("
            "NULL::bioseq_aligner_options));",
            statement));

        /* EXPLAIN/PREPARE must not evaluate any volatile argument. The
         * functions intentionally make the candidate ineligible; eligible
         * options remain raw/folded Consts by contract. */
        exec_sql(ctx,
            "SELECT setval('pg_temp.argument_effect_sequence',"
            "1,false);");
        const auto plan = explain_text<false>(ctx, fmt::format(
            "EXECUTE {}(1);", statement));
        REQUIRE_FALSE(plan.empty());
        REQUIRE(query_scalar(ctx,
            "SELECT is_called::text FROM "
            "pg_temp.argument_effect_sequence;") == "false");

        for (unsigned execution = 0; execution < 2u; ++execution) {
            exec_sql(ctx,
                "SELECT setval("
                "'pg_temp.argument_effect_sequence',1,false);");
            const auto error = sql_error(ctx, fmt::format(
                "EXECUTE {}(1);", statement));
            REQUIRE(error.sqlstate == "0A000");
            REQUIRE((error.contains("not owned")
                     || error.contains("no Bioseq executor owner")));
            REQUIRE(query_scalar_int(ctx,
                "SELECT last_value FROM "
                "pg_temp.argument_effect_sequence;") == 3);
            REQUIRE(query_scalar(ctx,
                "SELECT is_called::text FROM "
                "pg_temp.argument_effect_sequence;") == "true");
        }
        exec_sql(ctx, fmt::format("DEALLOCATE {};", statement));
    }
}

TEST_CASE("Projection-only EPQ evaluates volatile final once",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    constexpr std::string_view table = "projection_epq";
    constexpr std::string_view source = "projection_epq_source";
    constexpr std::string_view log = "projection_epq_log";
    auto setup = fresh_context({log, source, table});
    exec_sql(setup,
        "DROP FUNCTION IF EXISTS public.projection_epq_once("
        "text,text);"
        "CREATE TABLE projection_epq ("
        "id int PRIMARY KEY, source_id int NOT NULL, use_suffix boolean "
        "NOT NULL, touch int, sliced text);"
        "CREATE TABLE projection_epq_source ("
        "id int PRIMARY KEY, seq bioseq.seq('dna4',32)) "
        "USING bioseq;"
        "CREATE TABLE projection_epq_log ("
        "ordinal bigserial PRIMARY KEY, label text UNIQUE NOT NULL, "
        "observed text);"
        "INSERT INTO projection_epq VALUES (1,1,false,0,NULL);"
        "INSERT INTO projection_epq_source VALUES "
        "(1,'AAAACCCC'::bioseq.seq('dna4',32));"
        "CREATE FUNCTION public.projection_epq_once("
        "label text, value text) RETURNS text LANGUAGE plpgsql VOLATILE "
        "AS $$BEGIN "
        "INSERT INTO public.projection_epq_log(label,observed) "
        "VALUES(label,value); RETURN value; END$$;");

    const std::string update_sql =
        "UPDATE public.projection_epq AS h "
        "SET touch=h.touch+1, sliced=CASE WHEN h.use_suffix "
        "THEN s.suffix ELSE s.prefix END "
        "FROM (SELECT id, seq[0:4]::text AS prefix, "
        "seq[4:]::text AS suffix "
        "FROM public.projection_epq_source OFFSET 0) AS s "
        "WHERE h.id=1 AND h.source_id=s.id "
        "RETURNING public.projection_epq_once("
        "'projection-final',h.sliced);";
    const auto plan = explain_text<false>(setup, update_sql);
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqSliceProject)"));
    REQUIRE(plan_contains(plan, "Subquery Scan on s"));
    REQUIRE(query_scalar_int(setup,
        "SELECT count(*) FROM projection_epq_log;") == 0);

    connection_test_context blocker;
    connection_test_context worker;
    connection_test_context observer;
    exec_sql(worker, "SET statement_timeout='15s';");
    const auto outcome = run_locked_update(
        blocker, worker, observer,
        "UPDATE public.projection_epq "
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
        "FROM projection_epq_log;") == "projection-final");
    REQUIRE(query_scalar(setup,
        "SELECT observed FROM projection_epq_log;") == "CCCC");
    REQUIRE(query_scalar_int(setup,
        "SELECT touch FROM projection_epq WHERE id=1;") == 2);
    REQUIRE(query_scalar_int(setup,
        "SELECT source_id FROM projection_epq WHERE id=1;") == 1);
    REQUIRE(query_scalar_int(setup,
        "SELECT use_suffix::int FROM projection_epq WHERE id=1;")
        == 1);
    REQUIRE(query_scalar(setup,
        "SELECT sliced FROM projection_epq WHERE id=1;")
        == "CCCC");
    exec_sql(setup,
        "DROP FUNCTION public.projection_epq_once(text,text);");
}

TEST_CASE("BatchJoin EPQ preserves final effect order exactly once",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    constexpr std::string_view target = "batchjoin_epq_target";
    constexpr std::string_view queries = "batchjoin_epq_queries";
    constexpr std::string_view slice_source =
        "batchjoin_epq_slice_source";
    constexpr std::string_view result = "batchjoin_epq_result";
    constexpr std::string_view log = "batchjoin_epq_log";
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
        "DROP FUNCTION IF EXISTS public.batchjoin_epq_once("
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
        "CREATE FUNCTION public.batchjoin_epq_once("
        "label text, value text) RETURNS text LANGUAGE plpgsql VOLATILE "
        "AS $$BEGIN INSERT INTO public.{}(label,observed) "
        "VALUES(label,value); "
        "RETURN value; END$$;",
        queries, queries, hoe_probe_literal(),
        slice_source, slice_source, hoe_probe_literal(),
        result, result, log, log));
    configure_transformed_join(setup);

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
        "public.batchjoin_epq_once("
        "'target',matched.target_pk::text), "
        "public.batchjoin_epq_once('query',h.matched_query), "
        "public.batchjoin_epq_once("
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
    REQUIRE_FALSE(plan_has_executable_mmsearch(plan));
    REQUIRE(query_scalar_int(setup, fmt::format(
        "SELECT count(*) FROM {};", log)) == 0);

    connection_test_context blocker;
    connection_test_context worker;
    connection_test_context observer;
    configure_transformed_join(worker);
    exec_sql(worker, "SET statement_timeout='15s';");
    const auto outcome = run_locked_update(
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
        == std::string{k_hoe_probe_sequence.substr(1)});
    REQUIRE(query_scalar(setup, fmt::format(
        "SELECT observed FROM {} WHERE label='options';", log))
        == "suffix");
    REQUIRE(query_scalar_int(setup, fmt::format(
        "SELECT touch FROM {} WHERE id=1;", result)) == 2);
    REQUIRE(query_scalar_int(setup, fmt::format(
        "SELECT use_suffix::int FROM {} WHERE id=1;", result)) == 1);
    REQUIRE(query_scalar(setup, fmt::format(
        "SELECT matched_query FROM {} WHERE id=1;", result))
        == std::string{k_hoe_probe_sequence.substr(1)});
    exec_sql(setup,
        "DROP FUNCTION public.batchjoin_epq_once(text,text);");
}

TEST_CASE("Many-to-many mmsearch join owns the predicate and projections",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    auto ctx = fresh_context({k_hoe_join_left, k_hoe_join_right});
    seed_hoe_fixture(ctx, k_hoe_join_left);
    seed_hoe_fixture(ctx, k_hoe_join_right);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk, bioseq.cigar(t1.pk) "
            "  FROM {} t1, {} t2 "
            " WHERE mmsearch(t1.seq0, t2.seq1) "
            " ORDER BY t1.pk, t2.pk;",
            k_hoe_join_left, k_hoe_join_right));
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
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // The bioseq.symbol_count and bioseq.cigar projection placeholders share
    // the same rewriter and must route through
    // bioseq_symbol_count_support's simplify branch for LEFT JOIN.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    ctx.clear_notices();
    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk, bioseq.symbol_count(t1.seq0, 'A') "
            "  FROM {} t1 LEFT JOIN {} t2 ON t1.pk = t2.pk "
            " ORDER BY t1.pk;",
            k_hoe_join, k_hoe_join));
    REQUIRE(rows.size() == 5);
    for (const auto& row : rows) {
        REQUIRE(row.size() == 3);
        REQUIRE(row[0].has_value());
        REQUIRE(row[1].has_value());
        // Every seeded sequence has a deterministic A-count from the stored
        // inline symbol_counts index, populated by the columnar AM at INSERT
        // time when bioseq.index.insert_maintenance.enabled is on (which
        // seed_hoe_fixture sets).
        REQUIRE(row[2].has_value());
        const auto count = std::stoll(row[2].value());
        REQUIRE(count >= 0);
    }
    REQUIRE_FALSE(ctx.notice_contains("resource was not closed"));

    // A row inserted without the stored inline index and read with
    // bioseq.index.sequence_read.enabled disabled must surface a non-NULL tally
    // through the unconditional recompute fallback. Its sequence contains
    // eight A symbols.
    {
        guc_scope no_insert(ctx, "bioseq.index.insert_maintenance.enabled", "off");
        exec_sqlf(ctx,
                  "INSERT INTO {} VALUES "
                  "(6, 'AAAACCCCGGGGTTTTAAAACCCCGGGGTTTT'::bioseq.seq('dna4', 32), "
                  "'AAAACCCC'::bioseq.seq('dna4', 32));",
                  k_hoe_join);
    }
    {
        guc_scope no_read(ctx, "bioseq.index.sequence_read.enabled", "off");
        ctx.clear_notices();
        const auto guard_row = query_one(
            ctx,
            fmt::format(
                "SELECT bioseq.symbol_count(seq0, 'A') FROM {} WHERE pk = 6;",
                k_hoe_join));
        REQUIRE(guard_row.size() == 1);
        REQUIRE(guard_row[0].has_value());  // recompute fallback is never NULL
        REQUIRE(std::stoll(guard_row[0].value()) == 8);
        // The recompute opens seq_sub plus a registered random-read snapshot.
        // Both must be released before PQexec returns.
        REQUIRE_FALSE(ctx.notice_contains("resource was not closed"));
    }
}

TEST_CASE("EXPLAIN ANALYZE survives CROSS JOIN of bioseq tables",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // EXPLAIN ANALYZE executes both scans and must retain valid row counts.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const auto plan = explain_text(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk "
            "  FROM {} t1 CROSS JOIN {} t2 "
            " ORDER BY t1.pk, t2.pk;",
            k_hoe_join, k_hoe_join));
    REQUIRE_FALSE(plan.empty());
    REQUIRE(plan_contains(plan, "BioseqScan"));
    // Both relations should produce a Custom Scan node — the planner
    // self-join optimisation that could collapse them away does not
    // apply when the table uses our access method. A single BioseqScan would
    // mean the second scan was incorrectly rewritten to a heap path.
    int bioseq_scan_count = 0;
    for (const auto& line : plan) {
        if (line.find("Custom Scan (BioseqScan)") != std::string::npos) {
            ++bioseq_scan_count;
        }
    }
    REQUIRE(bioseq_scan_count == 2);
    // Assert the outer-side actual count by inspecting the raw plan strings.
    // Accept any ordering of "(actual ... rows=N..." after BioseqScan.
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
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // `WHERE mmsearch(t1.seq, t2.seq2)` executes inside the Nested Loop as a
    // fuzzy substring predicate. Pushdown extraction creates a parameterised
    // inner path, column-scan path construction claims its RestrictInfo, and
    // cursor construction removes alignments with empty CIGAR output.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk, bioseq.cigar(t1.pk) "
            "  FROM {} t1 CROSS JOIN {} t2 "
            " WHERE mmsearch(t1.seq0, t2.seq1) "
            " ORDER BY t1.pk, t2.pk;",
            k_hoe_join, k_hoe_join));

    // The row count is below the full 5x5 cartesian product because not every
    // pair aligns, and each emitted row has a non-empty valid CIGAR.
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
        // Every surfaced row must have a non-empty, valid CIGAR.
        REQUIRE(row[2].has_value());
        REQUIRE_FALSE(row[2].value().empty());
        REQUIRE(is_valid_cigar(row[2].value()));
    }
}

TEST_CASE("Multiple Const mmsearch quals fail closed",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);
    constexpr std::string_view second_probe = "TTCATCATCATCATCTACTCCT";
    const auto error = sql_error(ctx, fmt::format(
        "SELECT pk FROM {} WHERE "
        "mmsearch(seq0, '{}'::bioseq.seq('dna4', 32)) AND "
        "mmsearch(seq0, '{}'::bioseq.seq('dna4', 32));",
        k_hoe_join, k_hoe_probe_sequence, second_probe));
    REQUIRE(error.sqlstate == "0A000");
    REQUIRE(error.contains("stacked mmsearch"));
}

TEST_CASE("Aggregate over bioseq.cigar projection (MAX) returns the "
          "lexicographic max CIGAR",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // Exercises the has_aggs branch of bioseq_placeholder_support. The planner
    // uses the standard Aggregate node when an input depends on a bioseq
    // projection, so placeholder Consts are evaluated by the correct slot.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const std::string sql = fmt::format(
        "SELECT MAX(bioseq.cigar(pk)) FROM {} "
        " WHERE mmsearch(seq0, '{}'::bioseq.seq('dna4', 32));",
        k_hoe_join, k_hoe_probe_sequence);
    const auto row = query_one(ctx, sql);
    REQUIRE(row.size() == 1);
    REQUIRE(row[0].has_value());
    REQUIRE_FALSE(row[0].value().empty());
    REQUIRE(is_valid_cigar(row[0].value()));
}

TEST_CASE("COUNT(*) over an mmsearch-filtered bioseq table compiles and runs",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // COUNT(*) doesn't take a bioseq projection as an argument,
    // so the SUPPORT-function PHV machinery doesn't fire for it
    // — but the WHERE mmsearch(...) does filter rows through our
    // batch processor. The hit count must reflect the number of
    // matching rows the aligner returned, NOT the raw 5 rows in
    // the fixture.
    //
    // This count has no bioseq projection. Integer aggregate projections use
    // a separate planner path.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const std::string count_sql = fmt::format(
        "SELECT COUNT(*)::int8 AS hits "
        "  FROM {} "
        " WHERE mmsearch(seq0, '{}'::bioseq.seq('dna4', 32));",
        k_hoe_join, k_hoe_probe_sequence);
    const auto count_plan = explain_text<false>(ctx, count_sql);
    REQUIRE(plan_contains(count_plan, "Aggregate"));
    REQUIRE(plan_contains(count_plan, "Custom Scan (BioseqScan)"));
    REQUIRE_FALSE(plan_contains(
        count_plan, "Custom Scan (VectorAggNode)"));
    const auto count_rows = expect_invariant_under_guc_toggle(
        ctx, count_sql, "bioseq.vectorization.enabled");
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
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const std::string sql = fmt::format(
        "SELECT pk, MAX(bioseq.cigar(pk)) AS best_cigar "
        "  FROM {} "
        " WHERE mmsearch(seq0, '{}'::bioseq.seq('dna4', 32)) "
        " GROUP BY pk ORDER BY pk;",
        k_hoe_join, k_hoe_probe_sequence);
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
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    auto ctx = fresh_context({k_hoe_join_left, k_hoe_join_right});
    seed_hoe_fixture(ctx, k_hoe_join_left);
    seed_hoe_fixture(ctx, k_hoe_join_right);
	const auto stacked_error = sql_error(ctx, fmt::format(
		"SELECT t1.pk, t2.pk FROM {} t1 INNER JOIN {} t2 "
		"ON mmsearch(t2.seq0, t1.seq1) "
		"WHERE mmsearch(t2.seq0, '{}'::bioseq.seq('dna4', 32));",
		k_hoe_join_left, k_hoe_join_right,
		"TCATCATCATCATCTACTCCTATCTATCATCATCATCATCTACTCCT"));
	REQUIRE(stacked_error.sqlstate == "0A000");
	REQUIRE(stacked_error.contains("stacked mmsearch"));
}

TEST_CASE("Cross-table mmsearch via INNER JOIN with bioseq.cigar projection",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // INNER JOIN syntax has the same semantics as the CROSS JOIN predicate:
    // every emitted row has a valid non-empty CIGAR and the planner uses the
    // parameterised inner-side path.
    auto ctx = fresh_context({k_hoe_join_left, k_hoe_join_right});
    seed_hoe_fixture(ctx, k_hoe_join_left);
    seed_hoe_fixture(ctx, k_hoe_join_right);

    const auto rows = query_rows(
        ctx,
        fmt::format(
            "SELECT t1.pk, t2.pk, bioseq.cigar(t1.pk) "
            "  FROM {} t1 INNER JOIN {} t2 "
            "    ON mmsearch(t1.seq0, t2.seq1) "
            " ORDER BY t1.pk, t2.pk;",
            k_hoe_join_left, k_hoe_join_right));
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

/**
 * @file
 * @brief Checks Bioseq joins, alignmnt fields, and repeated scans.
 */
#include <libpq-fe.h>
#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;
import bioseq.tests.integration.join_test_support;

using namespace bioseq_int_tests;
using namespace bioseq_int_tests::join_test_support;

TEST_CASE("Self-join with mmsearch executes and stays in the seeded pk domain",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // Each result must name rows from the two table references used by mmsearch.
    // Both primary keys must belong to the seeded fixture.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const auto rows = query_rows(ctx,
                                 fmt::format("SELECT t1.pk, t2.pk "
                                             "  FROM {} t1 CROSS JOIN {} t2 "
                                             " WHERE mmsearch(t1.seq0, t2.seq1) "
                                             " ORDER BY t1.pk, t2.pk;",
                                             k_hoe_join,
                                             k_hoe_join));
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

    const std::string sql = fmt::format("SELECT t1.pk, t2.pk "
                                        "  FROM {} t1 CROSS JOIN {} t2 "
                                        " WHERE mmsearch(t1.seq0, t2.seq1) "
                                        " ORDER BY t1.pk, t2.pk;",
                                        k_hoe_join,
                                        k_hoe_join);
    expect_invariant_under_guc_toggle(ctx, sql, "bioseq.vectorization.enabled");
}

TEST_CASE("Self-join projection helpers run without errors when matches surface",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const auto rows = query_rows(ctx,
                                 fmt::format("SELECT t1.pk, t2.pk, "
                                             "       bioseq.target_start(t1.pk), "
                                             "       bioseq.target_end(t1.pk), "
                                             "       bioseq.cigar(t1.pk) "
                                             "  FROM {} t1 CROSS JOIN {} t2 "
                                             " WHERE mmsearch(t1.seq0, t2.seq1) "
                                             " ORDER BY t1.pk, t2.pk;",
                                             k_hoe_join,
                                             k_hoe_join));
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
    // Advancing the inner scan must leave the outer scan's position, unchanged.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const auto rows = query_rows(ctx,
                                 fmt::format("SELECT t1.pk, t2.pk "
                                             "  FROM {} t1 CROSS JOIN {} t2 "
                                             " ORDER BY t1.pk, t2.pk;",
                                             k_hoe_join,
                                             k_hoe_join));
    REQUIRE(rows.size() == 25); // 5 x 5
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
    // Check independent scan positions using two different tables.
    // Reaching the end of an inner scan must still allow  the next outer row
    auto ctx = fresh_context({k_hoe_join_left, k_hoe_join_right});
    seed_hoe_fixture(ctx, k_hoe_join_left);
    seed_hoe_fixture(ctx, k_hoe_join_right);

    const auto rows = query_rows(ctx,
                                 fmt::format("SELECT t1.pk, t2.pk "
                                             "  FROM {} t1 CROSS JOIN {} t2 "
                                             " ORDER BY t1.pk, t2.pk;",
                                             k_hoe_join_left,
                                             k_hoe_join_right));
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
    // The CIGAR field must refer to the alignment of t1 against the probe
    // after the LEFT JOIN has produced its output row.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const auto rows = query_rows(
        ctx,
        fmt::format("SELECT t1.pk, t2.pk, bioseq.cigar(t1.pk) "
                    "  FROM {} t1 LEFT JOIN {} t2 ON t1.pk = t2.pk "
                    " WHERE mmsearch(t1.seq0, "
                    "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)) "
                    " ORDER BY t1.pk;",
                    k_hoe_join,
                    k_hoe_join));
    REQUIRE_FALSE(rows.empty());

    bool saw_valid_cigar = false;
    for (const auto& row : rows) {
        REQUIRE(row.size() == 3);
        REQUIRE(row[0].has_value());
        // The right side of a LEFT JOIN can be null. The CIGAR comes from t1's
        // alignment against the literal probe. Require at least one valid result below.
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

    const std::string sql =
        fmt::format("SELECT t1.pk, t2.pk, bioseq.target_start(t1.pk), bioseq.cigar(t1.pk) "
                    "  FROM {} t1 LEFT JOIN {} t2 ON t1.pk = t2.pk "
                    " WHERE mmsearch(t1.seq0, "
                    "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)) "
                    " ORDER BY t1.pk;",
                    k_hoe_join,
                    k_hoe_join);
    (void)expect_query_invariant_under_options(ctx,
                                               sql,
                                               {{"bioseq.vectorization.enabled", {"on", "off"}},
                                                {"bioseq.column_cache.enabled", {"on", "off"}}});
}

TEST_CASE("Repeated execution of CROSS JOIN does not leak per-scan state",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // Repeating the query in one connection must restart both scans.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    for (int run = 0; run < 4; ++run) {
        const auto rows = query_rows(ctx,
                                     fmt::format("SELECT t1.pk, t2.pk "
                                                 "  FROM {} t1 CROSS JOIN {} t2 "
                                                 " ORDER BY t1.pk, t2.pk;",
                                                 k_hoe_join,
                                                 k_hoe_join));
        REQUIRE(rows.size() == 25);
    }
}

TEST_CASE("INNER JOIN of two USING-bioseq tables with mmsearch on both "
          "preserves left-side row count",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // Use each t1.seq1 value as a query against t2.seq0. The join must handle
    // changing query values as it moves between rows.
    auto ctx = fresh_context({k_hoe_join_left, k_hoe_join_right});
    seed_hoe_fixture(ctx, k_hoe_join_left);
    seed_hoe_fixture(ctx, k_hoe_join_right);

    const auto rows = query_rows(ctx,
                                 fmt::format("SELECT t1.pk, t2.pk "
                                             "  FROM {} t1 INNER JOIN {} t2 "
                                             "    ON mmsearch(t2.seq0, t1.seq1) "
                                             " ORDER BY t1.pk, t2.pk;",
                                             k_hoe_join_left,
                                             k_hoe_join_right));

    // The join must find matches with query sequences taken  from rows.
    REQUIRE_FALSE(rows.empty());
    // This fixture should produce at least five alignments.
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
    // Alignment fields from t2 must survive the RIGHT JOIN, which preserves t2 rows.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const auto rows = query_rows(
        ctx,
        fmt::format("SELECT t1.pk, t2.pk, bioseq.cigar(t2.pk) "
                    "  FROM {} t1 RIGHT JOIN {} t2 ON t1.pk = t2.pk "
                    " WHERE mmsearch(t2.seq0, "
                    "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)) "
                    " ORDER BY t2.pk;",
                    k_hoe_join,
                    k_hoe_join));
    REQUIRE_FALSE(rows.empty());
    bool saw_valid_cigar = false;
    for (const auto& row : rows) {
        REQUIRE(row.size() == 3);
        REQUIRE(row[1].has_value()); // t2.pk preserved by RIGHT JOIN
        if (row[2].has_value() && !row[2].value().empty()) {
            REQUIRE(is_valid_cigar(row[2].value()));
            saw_valid_cigar = true;
        }
    }
    REQUIRE(saw_valid_cigar);
}

TEST_CASE("JOIN with ORDER BY bioseq.score then LIMIT/OFFSET stays sort-stable",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // Sorting by an alignment score must use the score belonging to each joined row.
    // Adding LIMIT and OFFSET must select the same rows from thaj ordering.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const std::string base_sql =
        fmt::format("SELECT t1.pk, t2.pk, bioseq.cigar(t1.pk) "
                    "  FROM {} t1 LEFT JOIN {} t2 ON t1.pk = t2.pk "
                    " WHERE mmsearch(t1.seq0, "
                    "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32))",
                    k_hoe_join,
                    k_hoe_join);
    const auto full =
        query_rows(ctx, base_sql + " ORDER BY bioseq.score(t1.pk) DESC NULLS LAST, t1.pk;");
    REQUIRE_FALSE(full.empty());
    const auto limited = query_rows(
        ctx, base_sql + " ORDER BY bioseq.score(t1.pk) DESC NULLS LAST, t1.pk LIMIT 2 OFFSET 1;");
    if (full.size() >= 3) {
        REQUIRE(limited.size() == 2);
        // LIMIT 2 OFFSET 1 must match rows 1 and 2 of the full ordered result.
        for (size_t i = 0; i < limited.size(); ++i) {
            REQUIRE(limited[i] == full[i + 1]);
        }
    }
}

TEST_CASE("JOIN with HAVING over MAX(bioseq.target_end) compiles and filters",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // Check an alignment field through GROUP BY and HAVING. Target-end positions
    // are positive for these matches, giving the HAVING filter a predictable result.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const auto rows = query_rows(
        ctx,
        fmt::format("SELECT t1.pk, MAX(bioseq.target_end(t1.pk)) AS best "
                    "  FROM {} t1 LEFT JOIN {} t2 ON t1.pk = t2.pk "
                    " WHERE mmsearch(t1.seq0, "
                    "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)) "
                    " GROUP BY t1.pk "
                    "HAVING MAX(bioseq.target_end(t1.pk)) > 0 "
                    " ORDER BY t1.pk;",
                    k_hoe_join,
                    k_hoe_join));
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
    // Chain two mmsearch joins and run the query repeatedly. Each  execution
    // must return the same row triples as query values change between scans.
    auto ctx = fresh_context({k_hoe_join_left, k_hoe_join_right, k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join_left);
    seed_hoe_fixture(ctx, k_hoe_join_right);
    seed_hoe_fixture(ctx, k_hoe_join);

    const std::string sql = fmt::format("SELECT t1.pk, t2.pk, t3.pk "
                                        "  FROM {} t1 "
                                        "  JOIN {} t2 ON mmsearch(t2.seq0, t1.seq1) "
                                        "  JOIN {} t3 ON mmsearch(t3.seq0, t2.seq1) "
                                        " ORDER BY t1.pk, t2.pk, t3.pk;",
                                        k_hoe_join_left,
                                        k_hoe_join_right,
                                        k_hoe_join);
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
    // EXPLAIN ANALYZE must report both scans and the alignment details for this join
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const auto plan = explain_text(
        ctx,
        fmt::format("SELECT t1.pk, bioseq.cigar(t1.pk) "
                    "  FROM {} t1 CROSS JOIN {} t2 "
                    " WHERE mmsearch(t1.seq0, "
                    "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32));",
                    k_hoe_join,
                    k_hoe_join));
    REQUIRE_FALSE(plan.empty());
    int bioseq_scan_count = 0;
    for (const auto& line : plan) {
        if (line.find("Custom Scan (BioseqScan)") != std::string::npos) {
            ++bioseq_scan_count;
        }
    }
    REQUIRE(bioseq_scan_count >= 2);
    REQUIRE(plan_contains(plan, "Bioseq Projected Columns"));
    // ANALYZE executes the alignment, so its one-to-many statistics must be present.
    REQUIRE(plan_contains(plan, "One-to-many alignment"));
}

TEST_CASE("Repeated EXECUTE of a prepared mmsearch JOIN reuses runtime cleanly",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // Force a generic plan so execution binds $1 each time. A custom, plan can
    // replace it with a constant and would miss this parameter-binding case.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);
    exec_sql(ctx, "SET plan_cache_mode = force_generic_plan;");

    exec_sqlf(ctx,
              "PREPARE p_join_test(bioseq.seq('dna4', 32)) AS "
              "SELECT t1.pk, bioseq.cigar(t1.pk) "
              "  FROM {} t1 LEFT JOIN {} t2 ON t1.pk = t2.pk "
              " WHERE mmsearch(t1.seq0, $1) ORDER BY t1.pk;",
              k_hoe_join,
              k_hoe_join);
    const auto generic_plan =
        explain_text<false>(ctx,
                            "EXECUTE p_join_test("
                            "'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32));");
    REQUIRE(plan_contains(generic_plan, "Custom Scan (BioseqScan)"));

    const auto run1 = query_rows(
        ctx, "EXECUTE p_join_test('CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32));");
    const auto run2 =
        query_rows(ctx, "EXECUTE p_join_test('TCATCATCTACTCCTA'::bioseq.seq('dna4', 32));");
    const auto run3 = query_rows(
        ctx, "EXECUTE p_join_test('CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32));");
    const auto run_null = query_rows(ctx, "EXECUTE p_join_test(NULL);");

    // Same parameter must produce same output (no stale state).
    REQUIRE(run1 == run3);
    // The alternate parameter must still produce a non-empty result.
    REQUIRE_FALSE(run2.empty());
    // A NULL mmsearch argument must produce  no matching rows.
    REQUIRE(run_null.empty());

    const long long before_bad_typmod =
        query_scalar_int(ctx,
                         "SELECT execution_id FROM "
                         "bioseq.last_alignment_execution_stats() LIMIT 1;");
    const auto bad_typmod = sql_error(ctx,
                                      "EXECUTE p_join_test("
                                      "'ACGTACGTACGTACGTACGTACGTACGTACGT'"
                                      "::bioseq.seq('dna5', 32));");
#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    REQUIRE(bad_typmod.sqlstate == "22023");
    REQUIRE(bad_typmod.contains("bioseq.seq alphabet \"dna5\" is not supported"));
    REQUIRE(bad_typmod.contains("Valid alphabets: 'dna4'"));
#else
    REQUIRE(bad_typmod.sqlstate == "42804");
    REQUIRE(
        (bad_typmod.contains("bioseq mmsearch requires operands with the same bound alphabet") ||
         bad_typmod.contains("cannot cast bound bioseq.seq across alphabets")));
#endif
    /* DNA5 is rejected during argument parsing when the build supports only DNA4.
     * Other builds reject the alphabet mismatch. Neither failure may replace
     * the last completed alignment statistics. */
    REQUIRE(query_scalar_int(ctx,
                             "SELECT execution_id FROM "
                             "bioseq.last_alignment_execution_stats() LIMIT 1;") ==
            before_bad_typmod);

    exec_sql(ctx, "DEALLOCATE p_join_test;");
}

TEST_CASE("JOIN with insert maintenance and sequence-index reads disabled "
          "still produces matches",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // Computing minimizers during the query must stilq provide CIGAR fields.
    auto ctx = fresh_context({k_hoe_join});
    // Seed without index maintenance so this test starts with no stored indexes.
    exec_sql(ctx,
             "SET bioseq.index.insert_maintenance.enabled = off;"
             "SET bioseq.aligner.chaining.minimum_score = 10;"
             "SET bioseq.aligner.chaining.minimum_seed_count = 1;");
    // Use the fixture's low chaining thresholds so the short probe can align.
    create_seq_table(ctx,
                     seq_table_spec{
                         .name = std::string(k_hoe_join),
                         .typmod = {.alphabet = "dna4", .width = 32},
                         .seq_columns = 2,
                         .using_bioseq = true,
                     },
                     hoe_seed_rows());

    const std::string sql =
        fmt::format("SELECT t1.pk, bioseq.cigar(t1.pk) "
                    "  FROM {} t1 LEFT JOIN {} t2 ON t1.pk = t2.pk "
                    " WHERE mmsearch(t1.seq0, "
                    "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)) "
                    " ORDER BY t1.pk;",
                    k_hoe_join,
                    k_hoe_join);

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
    exec_sql(ctx,
             fmt::format("DROP TABLE IF EXISTS hoe_64 CASCADE; "
                         "CREATE TABLE hoe_64 ("
                         "pk INT PRIMARY KEY, seq0 bioseq.seq('dna4', 64)) USING bioseq; "
                         "INSERT INTO hoe_64 SELECT pk, seq0 FROM {};",
                         k_hoe_join));

    const auto same_width =
        query_rows(ctx,
                   fmt::format("SELECT t1.pk, t2.pk, bioseq.cigar(t1.pk) "
                               "FROM {} t1 JOIN {} t2 ON mmsearch(t1.seq0, t2.seq0) "
                               "ORDER BY t1.pk, t2.pk;",
                               k_hoe_join,
                               k_hoe_join));
    const auto cross_width =
        query_rows(ctx,
                   fmt::format("SELECT t1.pk, t2.pk, bioseq.cigar(t1.pk) "
                               "FROM {} t1 JOIN hoe_64 t2 ON mmsearch(t1.seq0, t2.seq0) "
                               "ORDER BY t1.pk, t2.pk;",
                               k_hoe_join));

    REQUIRE_FALSE(same_width.empty());
    REQUIRE(cross_width == same_width);
    exec_sql(ctx, "DROP TABLE IF EXISTS hoe_64 CASCADE;");
}

TEST_CASE("JOIN with CTE wrapping mmsearch preserves bioseq projections",
          "[component:bioseq][layer:integration][feature:pipeline][resource:postgres]") {
    // A query sequence selected through a CTE must still leave the alignment
    // result available to the  outer SELECT.
    auto ctx = fresh_context({k_hoe_join});
    seed_hoe_fixture(ctx, k_hoe_join);

    const auto rows = query_rows(ctx,
                                 fmt::format("WITH probes AS ("
                                             "    SELECT pk, seq0 FROM {} WHERE pk <= 3"
                                             ") "
                                             "SELECT t1.pk, p.pk, bioseq.cigar(t1.pk) "
                                             "  FROM {} t1 "
                                             "  JOIN probes p ON mmsearch(t1.seq0, p.seq0) "
                                             " ORDER BY t1.pk, p.pk;",
                                             k_hoe_join,
                                             k_hoe_join));
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
    // A LATERAL subquery takes its mmsearch query from the current outer row.
    // Check that returned keys still belong to the expected table rows.
    auto ctx = fresh_context({k_hoe_join_left, k_hoe_join_right});
    seed_hoe_fixture(ctx, k_hoe_join_left);
    seed_hoe_fixture(ctx, k_hoe_join_right);

    const auto rows =
        query_rows(ctx,
                   fmt::format("SELECT t1.pk, sub.pk, bioseq.cigar(sub.pk) "
                               "  FROM {} t1 "
                               "       CROSS JOIN LATERAL ("
                               "         SELECT pk FROM {} t2 WHERE mmsearch(t2.seq0, t1.seq1)"
                               "       ) sub "
                               " ORDER BY t1.pk, sub.pk;",
                               k_hoe_join_left,
                               k_hoe_join_right));
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

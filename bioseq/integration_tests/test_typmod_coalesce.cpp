// Tests for same-alphabet typmod coalescing and cross-alphabet rejection.
//
// The previous bug class: a query like
//   mmsearch(seq_dna4_32, 'CTAG...'::bioseq.seq('dna4', 64))
// (column is dna4/32, literal is dna4/64) returned rows with NULL
// projections because BeginCustomScan rejected typmod mismatches.
// The user's expectation:
//   - Same alphabet, different width  → coalesce (re-encode literal
//                                       at column's width); same
//                                       results, sidecar usable.
//   - Different bound alphabet        → SQLSTATE 42804 before recoding.
//
// These tests pin the property "results are independent of the
// literal's storage width". Same-alphabet paths produce results identical to
// the query expressed at the column's typmod; alphabet is semantic and never
// implicitly promoted by mmsearch.
//
// EXPLAIN-output assertions are kept secondary to result-set
// assertions: the EXPLAIN line "(coalesce to seq('dna4', 32))"
// is the user-visible signal but the row set is the load-bearing part.

#include <tests/support/postgres.hpp>
#include <bioseq/pg_build_profile.h>

#include <array>
#include <set>
#include <string>
#include <string_view>

import bioseq.integration_tests.common;

using namespace bioseq_int_tests;

namespace {

// Sentinel for tests in this file. Every TEST_CASE creates its own
// fresh context against this name; the reset_extension hook drops
// it before each run so cross-test leakage is impossible.
constexpr std::string_view kCoalesceTable = "coalesce_test";

}  // namespace

TEST_CASE("Typmod coalesce keeps equality and hash support representation-independent",
          "[seq][typmod_coalesce][hash][regression]") {
    auto ctx = fresh_context();
    const auto row = query_one(
        ctx,
        "WITH v AS (SELECT "
        " 'ACGTACGT'::bioseq.seq('dna4',8) AS narrow,"
        " 'ACGTACGT'::bioseq.seq('dna4',64) AS wide) "
        "SELECT bioseq.seq_eq(narrow,wide),"
        "       bioseq.seq_hash(narrow)=bioseq.seq_hash(wide),"
        "       bioseq.seq_hash_extended(narrow,0)="
        "         bioseq.seq_hash_extended(wide,0),"
        "       (bioseq.seq_hash(narrow)::bigint & 4294967295)="
        "         ((bioseq.seq_hash_extended(narrow,0) # "
        "           (bioseq.seq_hash_extended(narrow,0) >> 32)) "
        "          & 4294967295) "
        "FROM v;");
    REQUIRE(row[0] == "t");
    REQUIRE(row[1] == "t");
    REQUIRE(row[2] == "t");
    REQUIRE(row[3] == "t");
}

TEST_CASE("Typmod coalesce: same alphabet, wider query than column "
          "yields the same rows as a width-matched query",
          "[mmsearch][typmod_coalesce][regression]") {
    // Pre-fix: BeginCustomScan rejected the qual when typmod
    // didn't match exactly, leaving bioseq.* projections NULL.
    // Post-fix: the literal is re-encoded at the column's typmod
    // (apply_typmod_coalesce path) and the alignment runs as if
    // it had been written at the column's typmod from the start.
    //
    // We compare two queries that differ ONLY in the literal's
    // typmod — the result rows MUST match. This is a strong
    // property: if the coalesce path silently corrupts the
    // literal, the row sets diverge and the test fails.
    auto ctx = fresh_context({kCoalesceTable});
    seed_hoe_fixture(ctx, kCoalesceTable);

    // Width-matched (32, identical to column).
    const auto rows_match = query_rows(
        ctx,
        fmt::format(
            "SELECT pk, bioseq.cigar(pk), bioseq.target_start(pk), bioseq.target_end(pk) "
            "  FROM {} "
            " WHERE mmsearch(seq0, "
            "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)) "
            " ORDER BY pk;",
            kCoalesceTable));

    // Width-mismatched (64, requires coalesce).
    const auto rows_coalesce = query_rows(
        ctx,
        fmt::format(
            "SELECT pk, bioseq.cigar(pk), bioseq.target_start(pk), bioseq.target_end(pk) "
            "  FROM {} "
            " WHERE mmsearch(seq0, "
            "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 64)) "
            " ORDER BY pk;",
            kCoalesceTable));

    REQUIRE_FALSE(rows_match.empty());
    REQUIRE_FALSE(rows_coalesce.empty());
    // The user's headline expectation: "The query should return the
    // same results". Strict equality.
    REQUIRE(rows_match == rows_coalesce);
}

TEST_CASE("EXPLAIN typmod-coalesce metadata follows the active storage-width "
          "profile",
          "[mmsearch][typmod_coalesce][explain]") {
    // Optimisation levels 0 and 1 deliberately normalise every accepted
    // SQL width to uint64_t.  Under those profiles the nominal dna4/32
    // column and dna4/64 literal have the same packed typmod, so there is
    // no coalesce decision to surface.  Level 2 retains the requested
    // widths and must expose the user-visible coalesce hint.
    auto ctx = fresh_context({kCoalesceTable});
    seed_hoe_fixture(ctx, kCoalesceTable);

    const auto plan = explain_text(
        ctx,
        fmt::format(
            "SELECT pk FROM {} "
            " WHERE mmsearch(seq0, "
            "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 64));",
            kCoalesceTable));
    REQUIRE_FALSE(plan.empty());
#if BIOSEQ_OPTTIMIZATION_LEVEL <= 1
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
    REQUIRE_FALSE(plan_contains(plan, "coalesce to seq"));
#else
    REQUIRE(plan_contains(plan, "coalesce to seq('dna4', 32)"));
#endif
}

#if BIOSEQ_OPTTIMIZATION_LEVEL >= 1
TEST_CASE("Cross-alphabet mmsearch rejects before generic recoding",
          "[mmsearch][cross_alphabet][typmod_coalesce][error]") {
    auto ctx = fresh_context({kCoalesceTable});
    seed_hoe_fixture(ctx, kCoalesceTable);
    const auto mismatch = sql_error(
        ctx,
        fmt::format(
            "SELECT pk FROM {} WHERE mmsearch("
            "seq0, 'CTAGTCACTGTAGCCTGATCGTAGCTAGT'"
            "::bioseq.seq('dna5', 32));",
            kCoalesceTable));
    REQUIRE(mismatch.sqlstate == "42804");
    REQUIRE(mismatch.contains(
        "bioseq mmsearch requires operands with the same bound alphabet"));
}
#endif

// Parameterised matrix: column at dna4/32 vs literal at every
// supported (alphabet, width) typmod. The contract:
//   - Same (alphabet, width) → direct dispatch, results identical
//     to the canonical dna4/32 baseline.
//   - Same alphabet (dna4) different width → coalesce; results
//     identical to the baseline (literal is re-encoded at
//     column's typmod, sidecar fast-path applies).
//   - Different bound alphabet (dna5) → SQLSTATE 42804; no recoder or
//     alignment pipeline is entered.
//
// The TEMPLATE_TEST_CASE_SIG matrix exercises every entry in the
// (column, literal) typmod table. The user explicitly asked for
// this in the prompt: "Can we have integration tests for all
// combinations of sequences? Can we parameterize it via Catch2
// TEMPLATE_TEST_CASE".
// DOC0261 §11.2 — Dna4Entry / Dna5Entry are sourced from
// integration_tests.common (re-exported there because integration
// tests cannot transitively import bioseq_sequences).

TEMPLATE_TEST_CASE_SIG(
    "Typmod matrix: column dna4/32 against literal at (alphabet, width) — "
    "same-alphabet rows coalesce and different alphabets reject",
    "[mmsearch][typmod_coalesce][typmod_matrix][matrix]",
    ((class Entry, int Width), Entry, Width),
    (Dna4Entry, 8),
    (Dna4Entry, 16),
    (Dna4Entry, 32),
    (Dna4Entry, 64),
    (Dna5Entry, 8),
    (Dna5Entry, 16),
    (Dna5Entry, 32),
    (Dna5Entry, 64)) {
    auto ctx = fresh_context({kCoalesceTable});
#if BIOSEQ_OPTTIMIZATION_LEVEL == 0
    if constexpr (std::is_same_v<typename Entry::alphabet_type,
                                 seq_entry_tag::dna5>) {
        const auto unsupported_alphabet = sql_error(
            ctx,
            fmt::format(
                "SELECT "
                "'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('{}', {});",
                Entry::alphabet_name_sv, Width));
        REQUIRE(unsupported_alphabet.sqlstate == "22023");
        REQUIRE(unsupported_alphabet.contains(
            "bioseq.seq alphabet \"dna5\" is not supported"));
        REQUIRE(unsupported_alphabet.contains("Valid alphabets: 'dna4'"));
    } else
#endif
    {
    seed_hoe_fixture(ctx, kCoalesceTable);

    // Baseline at the column's exact typmod.
    const auto rows_baseline = query_rows(
        ctx,
        fmt::format(
            "SELECT pk, bioseq.cigar(pk) "
            "  FROM {} "
            " WHERE mmsearch(seq0, "
            "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)) "
            " ORDER BY pk;",
            kCoalesceTable));
    REQUIRE_FALSE(rows_baseline.empty());

    if constexpr (std::is_same_v<typename Entry::alphabet_type,
                                  seq_entry_tag::dna4>) {
        const auto rows_param = query_rows(
            ctx,
            fmt::format(
                "SELECT pk, bioseq.cigar(pk) "
                "  FROM {} "
                " WHERE mmsearch(seq0, "
                "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('{}', {})) "
                " ORDER BY pk;",
                kCoalesceTable, Entry::alphabet_name_sv, Width));
        REQUIRE_FALSE(rows_param.empty());
        REQUIRE(rows_param == rows_baseline);
    } else {
        const auto mismatch = sql_error(
            ctx,
            fmt::format(
                "SELECT pk FROM {} WHERE mmsearch(seq0, "
                "'CTAGTCACTGTAGCCTGATCGTAGCTAGT'"
                "::bioseq.seq('{}', {}));",
                kCoalesceTable, Entry::alphabet_name_sv, Width));
        REQUIRE(mismatch.sqlstate == "42804");
        REQUIRE(mismatch.contains(
            "bioseq mmsearch requires operands with the same bound alphabet"));
    }
    }
}

TEST_CASE("Same-alphabet PARAM_EXTERN queries rewidth in both target-width "
          "directions and match flat literals",
          "[mmsearch][typmod_coalesce][param][prepared][rewidth]") {
    auto ctx = fresh_context({kCoalesceTable, "coalesce_wide"});
    seed_hoe_fixture(ctx, kCoalesceTable);
    exec_sql(
        ctx,
        "CREATE TABLE coalesce_wide ("
        "pk integer PRIMARY KEY, seq0 bioseq.seq('dna4', 64)) USING bioseq; "
        "INSERT INTO coalesce_wide SELECT pk, seq0 FROM coalesce_test; "
        "SET plan_cache_mode = force_generic_plan;");

    const auto narrow_flat = query_rows(
        ctx,
        "SELECT pk, bioseq.cigar(pk) FROM coalesce_test WHERE mmsearch("
        "seq0, 'CTAGTCACTGTAGCCTGATCGTAGCTAGT'"
        "::bioseq.seq('dna4', 32)) ORDER BY pk;");
    exec_sql(
        ctx,
        "PREPARE doc0377_param_wide(bioseq.seq('dna4', 64)) AS "
        "SELECT pk, bioseq.cigar(pk) FROM coalesce_test "
        "WHERE mmsearch(seq0, $1) ORDER BY pk;");
    const auto narrow_param = query_rows(
        ctx,
        "EXECUTE doc0377_param_wide("
        "'CTAGTCACTGTAGCCTGATCGTAGCTAGT'"
        "::bioseq.seq('dna4', 64));");
    REQUIRE_FALSE(narrow_flat.empty());
    REQUIRE(narrow_param == narrow_flat);

    const auto wide_flat = query_rows(
        ctx,
        "SELECT pk, bioseq.cigar(pk) FROM coalesce_wide WHERE mmsearch("
        "seq0, 'CTAGTCACTGTAGCCTGATCGTAGCTAGT'"
        "::bioseq.seq('dna4', 64)) ORDER BY pk;");
    exec_sql(
        ctx,
        "PREPARE doc0377_param_narrow(bioseq.seq('dna4', 8)) AS "
        "SELECT pk, bioseq.cigar(pk) FROM coalesce_wide "
        "WHERE mmsearch(seq0, $1) ORDER BY pk;");
    const auto wide_param = query_rows(
        ctx,
        "EXECUTE doc0377_param_narrow("
        "'CTAGTCACTGTAGCCTGATCGTAGCTAGT'"
        "::bioseq.seq('dna4', 8));");
    REQUIRE_FALSE(wide_flat.empty());
    REQUIRE(wide_param == wide_flat);
    exec_sql(ctx, "DEALLOCATE doc0377_param_wide; "
                  "DEALLOCATE doc0377_param_narrow;");
}

TEST_CASE("Same-alphabet cross-width raw and sliced joins rewidth in both "
          "target/query directions",
          "[mmsearch][typmod_coalesce][join][slice][rewidth]") {
    auto ctx = fresh_context({"doc0377_join_narrow", "doc0377_join_wide"});
    exec_sql(ctx,
        "SET bioseq.update_index_on_insert = on;"
        "SET bioseq.enable_vectorization = on;");
    const auto canonical_rows = hoe_seed_rows();
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = "doc0377_join_narrow",
            .typmod = {.alphabet = "dna4", .width = 8},
            .seq_columns = 2,
            .using_bioseq = true,
        },
        canonical_rows);
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = "doc0377_join_wide",
            .typmod = {.alphabet = "dna4", .width = 64},
            .seq_columns = 2,
            .using_bioseq = true,
        },
        canonical_rows);

    const std::string narrow_raw_oracle_sql =
        "SELECT target.pk, query.pk "
        "FROM doc0377_join_narrow AS target "
        "JOIN doc0377_join_narrow AS query "
        "ON mmsearch(target.seq0, query.seq1) "
        "ORDER BY target.pk, query.pk;";
    const std::string wide_raw_oracle_sql =
        "SELECT target.pk, query.pk "
        "FROM doc0377_join_wide AS target "
        "JOIN doc0377_join_wide AS query "
        "ON mmsearch(target.seq0, query.seq1) "
        "ORDER BY target.pk, query.pk;";
    const std::string narrow_slice_oracle_sql =
        "SELECT target.pk, query.pk "
        "FROM doc0377_join_narrow AS target "
        "JOIN doc0377_join_narrow AS query "
        "ON mmsearch(target.seq0[0:], query.seq1[0:]) "
        "ORDER BY target.pk, query.pk;";
    const std::string wide_slice_oracle_sql =
        "SELECT target.pk, query.pk "
        "FROM doc0377_join_wide AS target "
        "JOIN doc0377_join_wide AS query "
        "ON mmsearch(target.seq0[0:], query.seq1[0:]) "
        "ORDER BY target.pk, query.pk;";

    /* Raw index reads consume the persisted MPS k/w, while a transformed
     * operand owns an explicit require-absent recompute route whose short-read
     * heuristic may select another k/w.  Storage width is the variable under
     * test, so compare each cross-width result with an identical-width oracle
     * that has the same ownership/read-index route. */
    const auto require_canonical_diagonal = [](
            const auto& rows,
            const std::set<std::string>& expected_diagonal_pks) {
        REQUIRE_FALSE(rows.empty());
        std::set<std::string> diagonal_pks;
        for (const auto& row : rows) {
            REQUIRE(row.size() == 2u);
            REQUIRE(row[0].has_value());
            REQUIRE(row[1].has_value());
            if (row[0] == row[1]) {
                diagonal_pks.insert(*row[0]);
            }
        }
        REQUIRE(diagonal_pks == expected_diagonal_pks);
    };
    const std::set<std::string> eager_diagonal_pks{"1", "2", "3", "5"};
    const std::set<std::string> persisted_diagonal_pks{"1", "3", "5"};

    const std::string narrow_join_sql =
        "SELECT n.pk, w.pk FROM doc0377_join_narrow AS n "
        "JOIN doc0377_join_wide AS w ON mmsearch(n.seq0, w.seq1) "
        "ORDER BY n.pk, w.pk;";
    const std::string wide_join_sql =
        "SELECT w.pk, n.pk FROM doc0377_join_wide AS w "
        "JOIN doc0377_join_narrow AS n ON mmsearch(w.seq0, n.seq1) "
        "ORDER BY w.pk, n.pk;";
    const std::string narrow_slice_join_sql =
        "SELECT n.pk, w.pk FROM doc0377_join_narrow AS n "
        "JOIN doc0377_join_wide AS w "
        "ON mmsearch(n.seq0[0:], w.seq1[0:]) "
        "ORDER BY n.pk, w.pk;";
    const std::string wide_slice_join_sql =
        "SELECT w.pk, n.pk FROM doc0377_join_wide AS w "
        "JOIN doc0377_join_narrow AS n "
        "ON mmsearch(w.seq0[0:], n.seq1[0:]) "
        "ORDER BY w.pk, n.pk;";

    constexpr std::array<std::string_view, 2> read_index_modes{"off", "on"};
    for (const std::string_view read_index_mode : read_index_modes) {
        DYNAMIC_SECTION("read_sequence_index=" << read_index_mode) {
            guc_scope read_index(
                ctx, "bioseq.read_sequence_index", read_index_mode);
            const auto narrow_raw_oracle =
                query_rows(ctx, narrow_raw_oracle_sql);
            const auto wide_raw_oracle =
                query_rows(ctx, wide_raw_oracle_sql);
            const auto narrow_slice_oracle =
                query_rows(ctx, narrow_slice_oracle_sql);
            const auto wide_slice_oracle =
                query_rows(ctx, wide_slice_oracle_sql);
            REQUIRE(wide_raw_oracle == narrow_raw_oracle);
            REQUIRE(wide_slice_oracle == narrow_slice_oracle);
            require_canonical_diagonal(
                narrow_raw_oracle,
                read_index_mode == "on"
                    ? persisted_diagonal_pks : eager_diagonal_pks);
            require_canonical_diagonal(
                narrow_slice_oracle, eager_diagonal_pks);

            for (const std::string* raw_sql :
                 std::array{&narrow_raw_oracle_sql, &wide_raw_oracle_sql,
                            &narrow_join_sql, &wide_join_sql}) {
                const auto plan = explain_text<false>(ctx, *raw_sql);
                REQUIRE(plan_contains(
                    plan, "Custom Scan (BioseqBatchJoin)"));
                REQUIRE_FALSE(plan_contains(
                    plan, "Bioseq Mmsearch Ownership: per-row recipe"));
            }
            for (const std::string* sliced_sql :
                 std::array{&narrow_slice_oracle_sql, &wide_slice_oracle_sql,
                            &narrow_slice_join_sql, &wide_slice_join_sql}) {
                const auto plan = explain_text<false>(ctx, *sliced_sql);
                REQUIRE(plan_contains(
                    plan, "Custom Scan (BioseqBatchJoin)"));
                REQUIRE(plan_contains(
                    plan, "Bioseq Mmsearch Ownership: per-row recipe"));
                REQUIRE(plan_contains(
                    plan, "Custom Scan (BioseqSliceProject)"));
            }

            REQUIRE(query_rows(ctx, narrow_join_sql) == narrow_raw_oracle);
            REQUIRE(query_rows(ctx, wide_join_sql) == wide_raw_oracle);
            REQUIRE(query_rows(ctx, narrow_slice_join_sql)
                    == narrow_slice_oracle);
            REQUIRE(query_rows(ctx, wide_slice_join_sql)
                    == wide_slice_oracle);
        }
    }
}

#if BIOSEQ_OPTTIMIZATION_LEVEL >= 1
TEST_CASE("Cross-alphabet raw and sliced joins reject in both operand directions",
          "[mmsearch][cross_alphabet][join][slice][error]") {
    auto ctx = fresh_context({"doc0377_join_dna4", "doc0377_join_dna5"});
    exec_sql(
        ctx,
        "CREATE TABLE doc0377_join_dna4 ("
        "id integer PRIMARY KEY, seq bioseq.seq('dna4', 32)) USING bioseq; "
        "CREATE TABLE doc0377_join_dna5 ("
        "id integer PRIMARY KEY, seq bioseq.seq('dna5', 32)) USING bioseq; "
        "INSERT INTO doc0377_join_dna4 VALUES "
        "(1, repeat('ACGT', 400)::bioseq.seq('dna4', 32)); "
        "INSERT INTO doc0377_join_dna5 VALUES "
        "(1, repeat('ACGT', 400)::bioseq.seq('dna5', 32));");
    const std::array<std::string_view, 4> statements{
        "SELECT d4.id FROM doc0377_join_dna4 d4 "
        "JOIN doc0377_join_dna5 d5 ON mmsearch(d4.seq, d5.seq);",
        "SELECT d5.id FROM doc0377_join_dna5 d5 "
        "JOIN doc0377_join_dna4 d4 ON mmsearch(d5.seq, d4.seq);",
        "SELECT d4.id FROM doc0377_join_dna4 d4 "
        "JOIN doc0377_join_dna5 d5 ON mmsearch(d4.seq[0:], d5.seq[0:]);",
        "SELECT d5.id FROM doc0377_join_dna5 d5 "
        "JOIN doc0377_join_dna4 d4 ON mmsearch(d5.seq[0:], d4.seq[0:]);",
    };
    for (const std::string_view statement : statements) {
        const auto mismatch = sql_error(ctx, std::string{statement});
        REQUIRE(mismatch.sqlstate == "42804");
        REQUIRE(mismatch.contains(
            "bioseq mmsearch requires operands with the same bound alphabet"));
    }
}

TEST_CASE("EXPLAIN rejects cross-alphabet mmsearch before plan execution",
          "[mmsearch][cross_alphabet][typmod_coalesce][explain][error]") {
    auto ctx = fresh_context({kCoalesceTable});
    seed_hoe_fixture(ctx, kCoalesceTable);
    const auto mismatch = sql_error(
        ctx,
        fmt::format(
            "EXPLAIN SELECT pk FROM {} "
            " WHERE mmsearch(seq0, "
            "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna5', 32));",
            kCoalesceTable));
    REQUIRE(mismatch.sqlstate == "42804");
    REQUIRE(mismatch.contains(
        "bioseq mmsearch requires operands with the same bound alphabet"));
}
#endif

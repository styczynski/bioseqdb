/**
 * @file Tests for same-alphabet typmod coalescing and cross-alphabet rejection.
 *
 * Same-alphabet width mismatches re-encode at the column width without
 * changing results or sidecar eligibility. Cross-alphabet bindings fail with
 * SQLSTATE 42804 before recoding. Result rows remain the primary contract;
 * EXPLAIN assertions provide a secondary user-visible signal.
 */

#include <tests/support/postgres.hpp>
#include <bioseq/pg_build_profile.h>

#include <array>
#include <set>
#include <string>
#include <string_view>

import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {

// Every case creates a fresh context for this relation; reset_extension
// drops it before each run so state cannot leak between cases.
constexpr std::string_view k_coalesce_table = "typmod_coalesce";

}  // namespace

TEST_CASE("Typmod coalesce keeps equality and hash support representation-independent",
          "[component:bioseq][layer:integration][feature:alphabet][resource:postgres]") {
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
          "[component:bioseq][layer:integration][feature:alphabet][resource:postgres]") {
    // The literal is re-encoded at the column's typmod through
    // apply_typmod_coalesce, and alignment proceeds at the column width.
    //
    // We compare two queries that differ ONLY in the literal's
    // typmod — the result rows MUST match. This is a strong
    // property: if the coalesce path silently corrupts the
    // literal, the row sets diverge.
    auto ctx = fresh_context({k_coalesce_table});
    seed_hoe_fixture(ctx, k_coalesce_table);

    // p_width-matched (32, identical to column).
    const auto matched_width_rows = query_rows(
        ctx,
        fmt::format(
            "SELECT pk, bioseq.cigar(pk), bioseq.target_start(pk), bioseq.target_end(pk) "
            "  FROM {} "
            " WHERE mmsearch(seq0, "
            "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)) "
            " ORDER BY pk;",
            k_coalesce_table));

    // p_width-mismatched (64, requires coalesce).
    const auto coalesced_width_rows = query_rows(
        ctx,
        fmt::format(
            "SELECT pk, bioseq.cigar(pk), bioseq.target_start(pk), bioseq.target_end(pk) "
            "  FROM {} "
            " WHERE mmsearch(seq0, "
            "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 64)) "
            " ORDER BY pk;",
            k_coalesce_table));

    REQUIRE_FALSE(matched_width_rows.empty());
    REQUIRE_FALSE(coalesced_width_rows.empty());
    // The user's headline expectation: "The query should return the
    // same results". Strict equality.
    REQUIRE(matched_width_rows == coalesced_width_rows);
}

TEST_CASE("EXPLAIN typmod-coalesce metadata follows the active storage-width "
          "profile",
          "[component:bioseq][layer:integration][feature:alphabet][resource:postgres]") {
    // Optimisation levels 0 and 1 deliberately normalise every accepted
    // SQL width to uint64_t.  Under those profiles the nominal dna4/32
    // column and dna4/64 literal have the same packed typmod, so there is
    // no coalesce decision to surface. all_coordinates retains the requested
    // widths and must expose the user-visible coalesce hint.
    auto ctx = fresh_context({k_coalesce_table});
    seed_hoe_fixture(ctx, k_coalesce_table);

    const auto plan = explain_text(
        ctx,
        fmt::format(
            "SELECT pk FROM {} "
            " WHERE mmsearch(seq0, "
            "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 64));",
            k_coalesce_table));
    REQUIRE_FALSE(plan.empty());
#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_COORDINATES)
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
    REQUIRE_FALSE(plan_contains(plan, "coalesce to seq"));
#else
    REQUIRE(plan_contains(plan, "coalesce to seq('dna4', 32)"));
#endif
}

#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
TEST_CASE("Cross-alphabet mmsearch rejects before generic recoding",
          "[component:bioseq][layer:integration][feature:alphabet][resource:postgres]") {
    auto ctx = fresh_context({k_coalesce_table});
    seed_hoe_fixture(ctx, k_coalesce_table);
    const auto mismatch = sql_error(
        ctx,
        fmt::format(
            "SELECT pk FROM {} WHERE mmsearch("
            "seq0, 'CTAGTCACTGTAGCCTGATCGTAGCTAGT'"
            "::bioseq.seq('dna5', 32));",
            k_coalesce_table));
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
// column/literal typmod table.
// dna4_entry and dna5_entry are sourced from
// bioseq.tests.integration.support (re-exported there because this module
// cannot transitively import bioseq_sequences).

TEMPLATE_TEST_CASE_SIG(
    "Typmod matrix: column dna4/32 against literal at (alphabet, width) — "
    "same-alphabet rows coalesce and different alphabets reject",
    "[component:bioseq][layer:integration][feature:alphabet][resource:postgres]",
    ((class p_entry, int p_width), p_entry, p_width),
    (dna4_entry, 8),
    (dna4_entry, 16),
    (dna4_entry, 32),
    (dna4_entry, 64),
    (dna5_entry, 8),
    (dna5_entry, 16),
    (dna5_entry, 32),
    (dna5_entry, 64)) {
    auto ctx = fresh_context({k_coalesce_table});
#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    if constexpr (std::is_same_v<typename p_entry::alphabet_type,
                                 seq_entry_tag::dna5>) {
        const auto unsupported_alphabet = sql_error(
            ctx,
            fmt::format(
                "SELECT "
                "'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('{}', {});",
                p_entry::alphabet_name_sv, p_width));
        REQUIRE(unsupported_alphabet.sqlstate == "22023");
        REQUIRE(unsupported_alphabet.contains(
            "bioseq.seq alphabet \"dna5\" is not supported"));
        REQUIRE(unsupported_alphabet.contains("Valid alphabets: 'dna4'"));
    } else
#endif
    {
    seed_hoe_fixture(ctx, k_coalesce_table);

    // Baseline at the column's exact typmod.
    const auto rows_baseline = query_rows(
        ctx,
        fmt::format(
            "SELECT pk, bioseq.cigar(pk) "
            "  FROM {} "
            " WHERE mmsearch(seq0, "
            "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)) "
            " ORDER BY pk;",
            k_coalesce_table));
    REQUIRE_FALSE(rows_baseline.empty());

    if constexpr (std::is_same_v<typename p_entry::alphabet_type,
                                  seq_entry_tag::dna4>) {
        const auto rows_param = query_rows(
            ctx,
            fmt::format(
                "SELECT pk, bioseq.cigar(pk) "
                "  FROM {} "
                " WHERE mmsearch(seq0, "
                "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('{}', {})) "
                " ORDER BY pk;",
                k_coalesce_table, p_entry::alphabet_name_sv, p_width));
        REQUIRE_FALSE(rows_param.empty());
        REQUIRE(rows_param == rows_baseline);
    } else {
        const auto mismatch = sql_error(
            ctx,
            fmt::format(
                "SELECT pk FROM {} WHERE mmsearch(seq0, "
                "'CTAGTCACTGTAGCCTGATCGTAGCTAGT'"
                "::bioseq.seq('{}', {}));",
                k_coalesce_table, p_entry::alphabet_name_sv, p_width));
        REQUIRE(mismatch.sqlstate == "42804");
        REQUIRE(mismatch.contains(
            "bioseq mmsearch requires operands with the same bound alphabet"));
    }
    }
}

TEST_CASE("Same-alphabet PARAM_EXTERN queries rewidth in both target-width "
          "directions and match flat literals",
          "[component:bioseq][layer:integration][feature:alphabet][resource:postgres]") {
    auto ctx = fresh_context({k_coalesce_table, "typmod_coalesce_wide"});
    seed_hoe_fixture(ctx, k_coalesce_table);
    exec_sql(
        ctx,
        "CREATE TABLE typmod_coalesce_wide ("
        "pk integer PRIMARY KEY, seq0 bioseq.seq('dna4', 64)) USING bioseq; "
        "INSERT INTO typmod_coalesce_wide SELECT pk, seq0 FROM typmod_coalesce; "
        "SET plan_cache_mode = force_generic_plan;");

    const auto narrow_flat = query_rows(
        ctx,
        "SELECT pk, bioseq.cigar(pk) FROM typmod_coalesce WHERE mmsearch("
        "seq0, 'CTAGTCACTGTAGCCTGATCGTAGCTAGT'"
        "::bioseq.seq('dna4', 32)) ORDER BY pk;");
    exec_sql(
        ctx,
        "PREPARE typmod_param_wide(bioseq.seq('dna4', 64)) AS "
        "SELECT pk, bioseq.cigar(pk) FROM typmod_coalesce "
        "WHERE mmsearch(seq0, $1) ORDER BY pk;");
    const auto narrow_param = query_rows(
        ctx,
        "EXECUTE typmod_param_wide("
        "'CTAGTCACTGTAGCCTGATCGTAGCTAGT'"
        "::bioseq.seq('dna4', 64));");
    REQUIRE_FALSE(narrow_flat.empty());
    REQUIRE(narrow_param == narrow_flat);

    const auto wide_flat = query_rows(
        ctx,
        "SELECT pk, bioseq.cigar(pk) FROM typmod_coalesce_wide WHERE mmsearch("
        "seq0, 'CTAGTCACTGTAGCCTGATCGTAGCTAGT'"
        "::bioseq.seq('dna4', 64)) ORDER BY pk;");
    exec_sql(
        ctx,
        "PREPARE typmod_param_narrow(bioseq.seq('dna4', 8)) AS "
        "SELECT pk, bioseq.cigar(pk) FROM typmod_coalesce_wide "
        "WHERE mmsearch(seq0, $1) ORDER BY pk;");
    const auto wide_param = query_rows(
        ctx,
        "EXECUTE typmod_param_narrow("
        "'CTAGTCACTGTAGCCTGATCGTAGCTAGT'"
        "::bioseq.seq('dna4', 8));");
    REQUIRE_FALSE(wide_flat.empty());
    REQUIRE(wide_param == wide_flat);
    exec_sql(ctx, "DEALLOCATE typmod_param_wide; "
                  "DEALLOCATE typmod_param_narrow;");
}

TEST_CASE("Same-alphabet cross-width raw and sliced joins rewidth in both "
          "target/query directions",
          "[component:bioseq][layer:integration][feature:alphabet][resource:postgres]") {
    auto ctx = fresh_context({"typmod_join_narrow", "typmod_join_wide"});
    exec_sql(ctx,
        "SET bioseq.index.insert_maintenance.enabled = on;"
        "SET bioseq.vectorization.enabled = on;");
    const auto canonical_rows = hoe_seed_rows();
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = "typmod_join_narrow",
            .typmod = {.alphabet = "dna4", .width = 8},
            .seq_columns = 2,
            .using_bioseq = true,
        },
        canonical_rows);
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = "typmod_join_wide",
            .typmod = {.alphabet = "dna4", .width = 64},
            .seq_columns = 2,
            .using_bioseq = true,
        },
        canonical_rows);

    const std::string narrow_raw_oracle_sql =
        "SELECT target.pk, query.pk "
        "FROM typmod_join_narrow AS target "
        "JOIN typmod_join_narrow AS query "
        "ON mmsearch(target.seq0, query.seq1) "
        "ORDER BY target.pk, query.pk;";
    const std::string wide_raw_oracle_sql =
        "SELECT target.pk, query.pk "
        "FROM typmod_join_wide AS target "
        "JOIN typmod_join_wide AS query "
        "ON mmsearch(target.seq0, query.seq1) "
        "ORDER BY target.pk, query.pk;";
    const std::string narrow_slice_oracle_sql =
        "SELECT target.pk, query.pk "
        "FROM typmod_join_narrow AS target "
        "JOIN typmod_join_narrow AS query "
        "ON mmsearch(target.seq0[0:], query.seq1[0:]) "
        "ORDER BY target.pk, query.pk;";
    const std::string wide_slice_oracle_sql =
        "SELECT target.pk, query.pk "
        "FROM typmod_join_wide AS target "
        "JOIN typmod_join_wide AS query "
        "ON mmsearch(target.seq0[0:], query.seq1[0:]) "
        "ORDER BY target.pk, query.pk;";

    /* Raw index reads consume the persisted minimizers_per_sequence k/w,
     * while a transformed
     * operand owns an explicit require-absent recompute route whose short-read
     * heuristic may select another k/w.  Storage width is the variable under
     * case, so compare each cross-width result with an identical-width oracle
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
        "SELECT n.pk, w.pk FROM typmod_join_narrow AS n "
        "JOIN typmod_join_wide AS w ON mmsearch(n.seq0, w.seq1) "
        "ORDER BY n.pk, w.pk;";
    const std::string wide_join_sql =
        "SELECT w.pk, n.pk FROM typmod_join_wide AS w "
        "JOIN typmod_join_narrow AS n ON mmsearch(w.seq0, n.seq1) "
        "ORDER BY w.pk, n.pk;";
    const std::string narrow_slice_join_sql =
        "SELECT n.pk, w.pk FROM typmod_join_narrow AS n "
        "JOIN typmod_join_wide AS w "
        "ON mmsearch(n.seq0[0:], w.seq1[0:]) "
        "ORDER BY n.pk, w.pk;";
    const std::string wide_slice_join_sql =
        "SELECT w.pk, n.pk FROM typmod_join_wide AS w "
        "JOIN typmod_join_narrow AS n "
        "ON mmsearch(w.seq0[0:], n.seq1[0:]) "
        "ORDER BY w.pk, n.pk;";

    constexpr std::array<std::string_view, 2> read_index_modes{"off", "on"};
    for (const std::string_view read_index_mode : read_index_modes) {
        DYNAMIC_SECTION("index.sequence_read.enabled=" << read_index_mode) {
            guc_scope read_index(
                ctx, "bioseq.index.sequence_read.enabled", read_index_mode);
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

#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
TEST_CASE("Cross-alphabet raw and sliced joins reject in both operand directions",
          "[component:bioseq][layer:integration][feature:alphabet][resource:postgres]") {
    auto ctx = fresh_context({"cross_alphabet_join_dna4", "cross_alphabet_join_dna5"});
    exec_sql(
        ctx,
        "CREATE TABLE cross_alphabet_join_dna4 ("
        "id integer PRIMARY KEY, seq bioseq.seq('dna4', 32)) USING bioseq; "
        "CREATE TABLE cross_alphabet_join_dna5 ("
        "id integer PRIMARY KEY, seq bioseq.seq('dna5', 32)) USING bioseq; "
        "INSERT INTO cross_alphabet_join_dna4 VALUES "
        "(1, repeat('ACGT', 400)::bioseq.seq('dna4', 32)); "
        "INSERT INTO cross_alphabet_join_dna5 VALUES "
        "(1, repeat('ACGT', 400)::bioseq.seq('dna5', 32));");
    const std::array<std::string_view, 4> statements{
        "SELECT d4.id FROM cross_alphabet_join_dna4 d4 "
        "JOIN cross_alphabet_join_dna5 d5 ON mmsearch(d4.seq, d5.seq);",
        "SELECT d5.id FROM cross_alphabet_join_dna5 d5 "
        "JOIN cross_alphabet_join_dna4 d4 ON mmsearch(d5.seq, d4.seq);",
        "SELECT d4.id FROM cross_alphabet_join_dna4 d4 "
        "JOIN cross_alphabet_join_dna5 d5 ON mmsearch(d4.seq[0:], d5.seq[0:]);",
        "SELECT d5.id FROM cross_alphabet_join_dna5 d5 "
        "JOIN cross_alphabet_join_dna4 d4 ON mmsearch(d5.seq[0:], d4.seq[0:]);",
    };
    for (const std::string_view statement : statements) {
        const auto mismatch = sql_error(ctx, std::string{statement});
        REQUIRE(mismatch.sqlstate == "42804");
        REQUIRE(mismatch.contains(
            "bioseq mmsearch requires operands with the same bound alphabet"));
    }
}

TEST_CASE("EXPLAIN rejects cross-alphabet mmsearch before plan execution",
          "[component:bioseq][layer:integration][feature:alphabet][resource:postgres]") {
    auto ctx = fresh_context({k_coalesce_table});
    seed_hoe_fixture(ctx, k_coalesce_table);
    const auto mismatch = sql_error(
        ctx,
        fmt::format(
            "EXPLAIN SELECT pk FROM {} "
            " WHERE mmsearch(seq0, "
            "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna5', 32));",
            k_coalesce_table));
    REQUIRE(mismatch.sqlstate == "42804");
    REQUIRE(mismatch.contains(
        "bioseq mmsearch requires operands with the same bound alphabet"));
}
#endif

// Tests for typmod-coalesce and cross-alphabet recompute paths.
//
// The previous bug class: a query like
//   mmsearch(seq_dna4_32, 'CTAG...'::bioseq.seq('dna4', 64))
// (column is dna4/32, literal is dna4/64) returned rows with NULL
// projections because BeginCustomScan rejected typmod mismatches.
// The user's expectation:
//   - Same alphabet, different width  → coalesce (re-encode literal
//                                       at column's width); same
//                                       results, sidecar usable.
//   - Different alphabet              → coalesce + recompute index
//                                       (sidecar minimizers are
//                                       alphabet-incompatible).
//
// These tests pin the property "results are independent of the
// literal's width or alphabet, as long as the data fits the
// column's typmod". Both paths should produce results identical
// to the same query expressed at the column's typmod.
//
// EXPLAIN-output assertions are kept secondary to result-set
// assertions: the EXPLAIN line "(coalesce to seq('dna4', 32))"
// or "Bioseq Recomputed: ... Performance reduced: ..." is the
// user-visible signal but the row set is the load-bearing part.

#include "common_tests_integration.h"
#include "headers/pg_build_config.h"

#include <set>
#include <string>

import bioseq.integration_tests.common;

using namespace bioseq_int_tests;

namespace {

// Sentinel for tests in this file. Every TEST_CASE creates its own
// fresh context against this name; the reset_extension hook drops
// it before each run so cross-test leakage is impossible.
constexpr std::string_view kCoalesceTable = "coalesce_test";

}  // namespace

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

TEST_CASE("EXPLAIN surfaces \"coalesce to seq('dna4', 32)\" when the "
          "literal width differs from the column width",
          "[mmsearch][typmod_coalesce][explain]") {
    // The user explicitly asked for the EXPLAIN line so they can
    // see the coalesce decision in the plan. We only check for
    // the substring; the exact wording is in customscan's
    // ExplainCustomScan.
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
    // The EXPLAIN line should include both the index-fetch
    // reference and the coalesce hint. We check for "coalesce" so
    // the test isn't brittle against rewording.
    REQUIRE(plan_contains(plan, "coalesce to seq('dna4', 32)"));
}

#if BIOSEQ_OPTTIMIZATION_LEVEL >= 1
TEST_CASE("Cross-alphabet recompute: dna5 query against dna4 column "
          "still returns matches",
          "[mmsearch][cross_alphabet][typmod_coalesce][regression]") {
    // Pre-fix: a dna5-typmod literal vs a dna4 column was rejected.
    // Post-fix: the literal is re-encoded at the column's
    // (alphabet, width) AND the buffer is marked
    // recompute_due_to_alphabet_mismatch, so the sidecar's
    // dna4-minimizers are skipped and minimizers are re-sketched
    // per-row at the column's alphabet. The result MUST match the
    // dna4-vs-dna4 baseline.
    //
    // Note: dna5 → dna4 lossy cast may drop 'N'/ambiguous letters
    // to 'A'. The probe used here contains only A/C/G/T so the
    // round-trip is bit-identical.
    auto ctx = fresh_context({kCoalesceTable});
    seed_hoe_fixture(ctx, kCoalesceTable);

    const auto rows_dna4 = query_rows(
        ctx,
        fmt::format(
            "SELECT pk, bioseq.cigar(pk) "
            "  FROM {} "
            " WHERE mmsearch(seq0, "
            "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)) "
            " ORDER BY pk;",
            kCoalesceTable));

    const auto rows_dna5 = query_rows(
        ctx,
        fmt::format(
            "SELECT pk, bioseq.cigar(pk) "
            "  FROM {} "
            " WHERE mmsearch(seq0, "
            "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna5', 32)) "
            " ORDER BY pk;",
            kCoalesceTable));

    REQUIRE_FALSE(rows_dna4.empty());
    // Cross-alphabet recompute may legitimately find a SUPERSET of
    // dna4's matches. The recompute path uses apply_size_heuristic
    // which (for short inputs) lowers k/w from 15/10 to 10/5 — that
    // is more sensitive than the sidecar-pinned 15/10 used by the
    // dna4 baseline. Both are correct; the recompute path is just
    // strictly more permissive.
    //
    // The load-bearing properties are:
    //   - dna5 query MUST produce results (recompute path runs)
    //   - dna5 results MUST include every pk that the dna4 baseline
    //     surfaced (the alignments are a superset, not a different
    //     set)
    //   - every emitted dna5 row has populated bioseq.* projections
    //     (NULL projections would be the regression we're fixing)
    REQUIRE_FALSE(rows_dna5.empty());
    std::set<std::string> dna4_pks;
    for (const auto& row : rows_dna4) {
        if (row[0].has_value()) dna4_pks.insert(*row[0]);
    }
    std::set<std::string> dna5_pks;
    for (const auto& row : rows_dna5) {
        REQUIRE(row[0].has_value());
        REQUIRE(row[1].has_value());                           // cigar populated
        REQUIRE_FALSE(row[1].value().empty());                 // not the empty-cigar regression
        REQUIRE(is_valid_cigar(row[1].value()));               // structurally valid
        dna5_pks.insert(*row[0]);
    }
    for (const auto& key : dna4_pks) {
        REQUIRE(dna5_pks.count(key) > 0);
    }
}
#endif

// Parameterised matrix: column at dna4/32 vs literal at every
// supported (alphabet, width) typmod. The contract:
//   - Same (alphabet, width) → direct dispatch, results identical
//     to the canonical dna4/32 baseline.
//   - Same alphabet (dna4) different width → coalesce; results
//     identical to the baseline (literal is re-encoded at
//     column's typmod, sidecar fast-path applies).
//   - Different alphabet (dna5) → coalesce + recompute; results
//     are a SUPERSET of the baseline (per §9.2 limitation).
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
    "result set is consistent across coalesce / recompute paths",
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

    // Literal at the parameterised typmod — coalesce/recompute as
    // needed inside BeginCustomScan.
    const auto rows_param = query_rows(
        ctx,
        fmt::format(
            "SELECT pk, bioseq.cigar(pk) "
            "  FROM {} "
            " WHERE mmsearch(seq0, "
            "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('{}', {})) "
            " ORDER BY pk;",
            kCoalesceTable, Entry::alphabet_name_sv, Width));

    // The coalesce path (same alphabet) MUST produce results
    // identical to the baseline — the literal is re-encoded at
    // the column's exact typmod and the sidecar minimizers stay
    // valid. The recompute path (different alphabet) is allowed
    // to be a superset — the seed thresholds get lowered by
    // apply_size_heuristic and additional short matches surface.
    REQUIRE_FALSE(rows_param.empty());
    if constexpr (std::is_same_v<typename Entry::alphabet_type,
                                  seq_entry_tag::dna4>) {
        REQUIRE(rows_param == rows_baseline);
    } else {
        // Cross-alphabet recompute: every baseline pk must be
        // present in the param set, and every emitted CIGAR is
        // structurally valid.
        std::set<std::string> baseline_pks, param_pks;
        for (const auto& row : rows_baseline) {
            if (row[0].has_value()) baseline_pks.insert(*row[0]);
        }
        for (const auto& row : rows_param) {
            REQUIRE(row.size() == 2);
            REQUIRE(row[0].has_value());
            REQUIRE(row[1].has_value());
            REQUIRE_FALSE(row[1].value().empty());
            REQUIRE(is_valid_cigar(row[1].value()));
            param_pks.insert(*row[0]);
        }
        for (const auto& key : baseline_pks) {
            REQUIRE(param_pks.count(key) > 0);
        }
    }
    }
}

#if BIOSEQ_OPTTIMIZATION_LEVEL >= 1
TEST_CASE("EXPLAIN surfaces \"Bioseq Recomputed: minimizers 100%\" with "
          "\"Performance reduced\" hint when alphabets differ",
          "[mmsearch][cross_alphabet][typmod_coalesce][explain]") {
    // Documented user-facing signal: the user asked for the
    // explanation line so they understand WHY a previously-fast
    // query suddenly recomputes. The EXPLAIN block in customscan
    // emits both the recompute % and the hint when
    // recompute_due_to_alphabet_mismatch is set.
    auto ctx = fresh_context({kCoalesceTable});
    seed_hoe_fixture(ctx, kCoalesceTable);

    const auto plan = explain_text(
        ctx,
        fmt::format(
            "SELECT pk FROM {} "
            " WHERE mmsearch(seq0, "
            "                'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna5', 32));",
            kCoalesceTable));
    REQUIRE_FALSE(plan.empty());
    // The "Recomputed" line must appear and be 100% (a
    // cross-alphabet query can never reuse the sidecar). The
    // "Performance reduced" hint must also appear so users
    // understand the cause.
    REQUIRE(plan_contains(plan, "Bioseq Recomputed"));
    REQUIRE(plan_contains(plan, "Performance reduced"));
}
#endif

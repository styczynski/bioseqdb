// Substring properties for mmsearch require non-NULL projections and exact
// coordinates for short queries embedded in longer targets.
//
// Substring contract: mmsearch(A, B) where B is a contiguous substring of A
//   should produce (using the aligner's half-open interval convention,
//   matching BED-file semantics; the representative 29 bp probe yields
//   target_start=45 target_end=74 i.e. target_end-target_start ==
//   B.size()):
//     query_start  == 0
//     query_end    == B.size()                   (exclusive end)
//     target_start == A.find(B)
//     target_end   == A.find(B) + B.size()       (exclusive end)
//     cigar        == "{B.size()}M"   (single all-M run)
//
//   Our seed-based aligner (minimap2 port) requires k=15 / w=10 by
//   default, so very short B (under ~25 bp) will not chain unless we
//   relax the size heuristic. The dataset below uses a 30 bp probe
//   embedded in a 60 bp target so the default heuristic engages
//   (`apply_size_heuristic` in dispatch_buffer.cppm raises the
//   thresholds for short inputs to k=10/w=5/min_chain=20/min_seed=2).
//
//   The aligner uses half-open coordinates: target_end - target_start
//   equals B.size(). A 29 bp probe therefore has a span of 29.
//
// Self-alignment contract: mmsearch(A, A) — full-length self-alignment — should
//   produce target_start=0, target_end=A.size(), cigar "{N}M".
//   The full-match property is checked under five SQL shapes:
//     1. CROSS JOIN of the table with itself, WHERE t1.a = t2.a
//     2. INNER JOIN ON t1.a = t2.a (vs CROSS JOIN + WHERE)
//     3. mmsearch over the SAME column on a single relation
//     4. mmsearch over DIFFERENT columns that contain identical data
//     5. mmsearch over a (SELECT ...) subquery returning the same data
//   All five must produce identical rows.
//
// Three rows with predictable substring relationships establish exact start/end
// coordinates and require a non-empty CIGAR.

#include <tests/support/postgres.hpp>

#include <set>
#include <string>

import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view k_substring_table = "substring_alignment";
constexpr std::string_view k_substring_self_table = "self_alignment";

// Probe `B` is a 30 bp run that cleanly seeds under the size-heuristic
// (k=10 / w=5 path) when embedded in a target of 60 bp.
constexpr std::string_view k_probe_b =
    "CTATCATTCATACTTCTATCATTCATACTT";  // 30 bp

// `A` = run of 'A' characters + B + run of 'A' characters. The find
// position of B in A is exactly the leading-A length so we can
// compute expected target_start trivially. We pick lengths 23 and 8
// so total length stays within the seq('dna4', 32) bound width=32
// (32 storage units * 4 letters/unit = 128 bp ceiling).
constexpr std::string_view k_padded_a =
    "AAAAAAAAAAAAAAAAAAAAAAACTATCATTCATACTTCTATCATTCATACTTAAAAAAAA";
//   ^---- 23 A ----^^------ 30 bp probe B -----------^^-- 8 A --^

// expected_target_start is the index where B occurs in A (inclusive).
// expected_target_end is half-open (matches aligner output).
constexpr long long k_expected_target_start = 23;
constexpr long long k_expected_target_end =
    k_expected_target_start + static_cast<long long>(k_probe_b.size());

// Self-alignment dataset: a single sequence used as both query and
// target. Same 60 bp shape for parity; full self-alignment must
// produce CIGAR "60M".
constexpr std::string_view k_self_sequence = k_padded_a;

void seed_substring_table(const connection_test_context& ctx,
                          std::string_view name) {
    exec_sqlf(ctx, "SET bioseq.index.insert_maintenance.enabled = on;");
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(name),
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 2,  // seq0 stores A, seq1 stores B (probe)
            .using_bioseq = true,
        },
        {
            seq_row{.pk = 1, .sequences = {std::string(k_padded_a), std::string(k_probe_b)}},
        });
}

// Self-alignment dataset: a single column that holds the same
// sequence in both `seq0` and `seq1` so we can run mmsearch(seq0,
// seq1) and observe the FULL-match property.
void seed_self_alignment_table(const connection_test_context& ctx,
                               std::string_view name) {
    exec_sqlf(ctx, "SET bioseq.index.insert_maintenance.enabled = on;");
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(name),
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 2,  // seq0 == seq1 in every row
            .using_bioseq = true,
        },
        {
            seq_row{.pk = 1, .sequences = {std::string(k_self_sequence),
                                           std::string(k_self_sequence)}},
        });
}

}  // namespace

TEST_CASE("mmsearch(A, B) where B is a substring of A returns exact "
          "target_start / target_end / cigar coordinates",
          "[component:bioseq][layer:integration][feature:alignment][resource:postgres]") {
    // Require every projection so rows containing NULL alignment fields fail.
    auto ctx = fresh_context({k_substring_table});
    seed_substring_table(ctx, k_substring_table);

    const std::string sql = fmt::format(
        "SELECT pk, "
        "       bioseq.query_start(pk) AS qs, "
        "       bioseq.query_end(pk) AS qe, "
        "       bioseq.target_start(pk) AS ts, "
        "       bioseq.target_end(pk) AS te, "
        "       bioseq.cigar(pk) AS cig "
        "  FROM {} "
        " WHERE mmsearch(seq0, seq1) "
        " ORDER BY pk;",
        k_substring_table);

    const auto rows = query_rows(ctx, sql);
    REQUIRE(rows.size() == 1);
    const auto& row = rows[0];
    REQUIRE(row.size() == 6);
    // Every projection must be populated for an alignment result.
    for (size_t col = 0; col < row.size(); ++col) {
        REQUIRE(row[col].has_value());
    }
    REQUIRE(std::stoll(row[0].value()) == 1);
    // Substring property: the aligner-reported coordinates MUST
    // match the offset of the probe inside the target under the
    // half-open alignment coordinate contract.
    REQUIRE(std::stoll(row[1].value()) == 0);                       // query_start
    REQUIRE(std::stoll(row[2].value())
            == static_cast<long long>(k_probe_b.size()));             // query_end (half-open)
    REQUIRE(std::stoll(row[3].value()) == k_expected_target_start);    // target_start
    REQUIRE(std::stoll(row[4].value()) == k_expected_target_end);      // target_end (half-open)
    // CIGAR: a fuzzy aligner is allowed to return any CIGAR that
    // sums to a length covering the substring; the canonical
    // exact-match shape is "{B.size()}M". For the seeded data
    // the canonical answer is the only accepted result.
    REQUIRE(is_valid_cigar(row[5].value()));
    REQUIRE(is_exact_match_cigar(row[5].value(),
                                 static_cast<long long>(k_probe_b.size())));
}

TEST_CASE("mmsearch(A, A) — full self-alignment over a single column "
          "produces a full-length CIGAR",
          "[component:bioseq][layer:integration][feature:alignment][resource:postgres]") {
    // Self-alignment is the special case of the substring property
    // where B = A. The aligner must return target_start=0,
    // target_end=A.size()-1, cigar "{A.size()}M". The result must
    // include a non-empty CIGAR when both operands are Vars.
    //
    // The same-relation Var path uses per-row mode, so each row aligns
    // against its own column value.
    auto ctx = fresh_context({k_substring_self_table});
    seed_self_alignment_table(ctx, k_substring_self_table);

    const std::string sql = fmt::format(
        "SELECT pk, "
        "       bioseq.query_start(pk) AS qs, "
        "       bioseq.query_end(pk) AS qe, "
        "       bioseq.target_start(pk) AS ts, "
        "       bioseq.target_end(pk) AS te, "
        "       bioseq.cigar(pk) AS cig "
        "  FROM {} "
        " WHERE mmsearch(seq0, seq0) "    // SAME-VAR self-alignment
        " ORDER BY pk;",
        k_substring_self_table);

    const auto rows = query_rows(ctx, sql);
    REQUIRE(rows.size() == 1);
    const auto& row = rows[0];
    REQUIRE(row.size() == 6);
    for (size_t col = 0; col < row.size(); ++col) {
        REQUIRE(row[col].has_value());
    }
    const long long full_len = static_cast<long long>(k_self_sequence.size());
    REQUIRE(std::stoll(row[1].value()) == 0);
    REQUIRE(std::stoll(row[2].value()) == full_len);   // half-open
    REQUIRE(std::stoll(row[3].value()) == 0);
    REQUIRE(std::stoll(row[4].value()) == full_len);   // half-open
    REQUIRE(is_exact_match_cigar(row[5].value(), full_len));
}

TEST_CASE("nested mmsearch qualifier fails closed instead of executing the SQL stub",
          "[component:bioseq][layer:integration][feature:alignment][resource:postgres]") {
    auto ctx = fresh_context({k_substring_self_table});
    seed_self_alignment_table(ctx, k_substring_self_table);

    const auto err = sql_error(ctx, fmt::format(
        "SELECT pk, bioseq.cigar(pk) FROM {} "
        "WHERE NOT mmsearch(seq0, seq0);",
        k_substring_self_table));
    REQUIRE(err.sqlstate == "0A000");
    REQUIRE(err.contains("nested mmsearch qualifier shape is unsupported"));
    REQUIRE(err.contains("executing its SQL stub without alignment is forbidden"));
}

TEST_CASE("Self-alignment via different columns containing the same data",
          "[component:bioseq][layer:integration][feature:alignment][resource:postgres]") {
    // Here arg0 = seq0 and arg1 = seq1, while the
    // dataset writes IDENTICAL bytes to both columns. The
    // per-row mode in BeginCustomScan dispatches arg1's column
    // value at submit_scan time, so the alignment is still
    // X-vs-X — must produce the same full-length match.
    //
    // Per-row mode must extract both operand columns independently.
    auto ctx = fresh_context({k_substring_self_table});
    seed_self_alignment_table(ctx, k_substring_self_table);

    const std::string sql = fmt::format(
        "SELECT pk, "
        "       bioseq.target_start(pk) AS ts, "
        "       bioseq.target_end(pk) AS te, "
        "       bioseq.cigar(pk) AS cig "
        "  FROM {} "
        " WHERE mmsearch(seq0, seq1) "    // DIFFERENT vars, identical data
        " ORDER BY pk;",
        k_substring_self_table);

    const auto rows = query_rows(ctx, sql);
    REQUIRE(rows.size() == 1);
    const auto& row = rows[0];
    REQUIRE(row.size() == 4);
    for (size_t col = 0; col < row.size(); ++col) {
        REQUIRE(row[col].has_value());
    }
    const long long full_len = static_cast<long long>(k_self_sequence.size());
    REQUIRE(std::stoll(row[1].value()) == 0);
    REQUIRE(std::stoll(row[2].value()) == full_len);   // half-open
    REQUIRE(is_exact_match_cigar(row[3].value(), full_len));
}

TEST_CASE("Self-alignment via cross join with WHERE t1.pk = t2.pk",
          "[component:bioseq][layer:integration][feature:alignment][resource:postgres]") {
    // A cross join restricted to the diagonal via
    // WHERE t1.pk = t2.pk. The aligner takes mmsearch(t1.seq, t2.seq)
    // — but because t1 and t2 are both the same row (filtered by
    // pk equality), this is logically a self-alignment.
    // The result requires the exact full-match CIGAR.
    auto ctx = fresh_context({k_substring_self_table});
    seed_self_alignment_table(ctx, k_substring_self_table);

    const std::string sql = fmt::format(
        "SELECT t1.pk, "
        "       bioseq.target_start(t1.pk) AS ts, "
        "       bioseq.target_end(t1.pk) AS te, "
        "       bioseq.cigar(t1.pk) AS cig "
        "  FROM {} t1 CROSS JOIN {} t2 "
        " WHERE mmsearch(t1.seq0, t2.seq0) "
        "   AND t1.pk = t2.pk "
        " ORDER BY t1.pk;",
        k_substring_self_table, k_substring_self_table);

    const auto rows = query_rows(ctx, sql);
    REQUIRE(rows.size() == 1);
    const auto& row = rows[0];
    REQUIRE(row.size() == 4);
    for (size_t col = 0; col < row.size(); ++col) {
        REQUIRE(row[col].has_value());
    }
    const long long full_len = static_cast<long long>(k_self_sequence.size());
    REQUIRE(std::stoll(row[1].value()) == 0);
    REQUIRE(std::stoll(row[2].value()) == full_len);   // half-open
    REQUIRE(is_exact_match_cigar(row[3].value(), full_len));
}

TEST_CASE("Self-alignment via INNER JOIN on t1.pk = t2.pk",
          "[component:bioseq][layer:integration][feature:alignment][resource:postgres]") {
    // Flavor 2: same semantic content as the CROSS JOIN flavor
    // above, but expressed as INNER JOIN ... ON t1.pk = t2.pk.
    // The planner generates a different physical plan (parameterised
    // inner-side scan via Param-rebind), but the result rows
    // must be byte-identical.
    auto ctx = fresh_context({k_substring_self_table});
    seed_self_alignment_table(ctx, k_substring_self_table);

    const std::string sql = fmt::format(
        "SELECT t1.pk, "
        "       bioseq.target_start(t1.pk) AS ts, "
        "       bioseq.target_end(t1.pk) AS te, "
        "       bioseq.cigar(t1.pk) AS cig "
        "  FROM {} t1 INNER JOIN {} t2 ON t1.pk = t2.pk "
        " WHERE mmsearch(t1.seq0, t2.seq0) "
        " ORDER BY t1.pk;",
        k_substring_self_table, k_substring_self_table);

    const auto rows = query_rows(ctx, sql);
    REQUIRE(rows.size() == 1);
    const auto& row = rows[0];
    REQUIRE(row.size() == 4);
    for (size_t col = 0; col < row.size(); ++col) {
        REQUIRE(row[col].has_value());
    }
    const long long full_len = static_cast<long long>(k_self_sequence.size());
    REQUIRE(std::stoll(row[1].value()) == 0);
    REQUIRE(std::stoll(row[2].value()) == full_len);   // half-open
    REQUIRE(is_exact_match_cigar(row[3].value(), full_len));
}

TEST_CASE("Self-alignment via subquery returning identical data",
          "[component:bioseq][layer:integration][feature:alignment][resource:postgres]") {
    // Flavor 5: a (SELECT ...) subquery that returns the same data
    // as the outer relation. This stresses the planner path where
    // the inner side is a SubqueryScan above a BioseqScan; the
    // The mmsearch qualifier must still bind both columns to the underlying
    // bioseq.seq and produce the full-length match.
    auto ctx = fresh_context({k_substring_self_table});
    seed_self_alignment_table(ctx, k_substring_self_table);

    const std::string sql = fmt::format(
        "SELECT t1.pk, "
        "       bioseq.target_start(t1.pk) AS ts, "
        "       bioseq.target_end(t1.pk) AS te, "
        "       bioseq.cigar(t1.pk) AS cig "
        "  FROM {} t1 "
        "  JOIN (SELECT pk, seq0 FROM {}) sub ON t1.pk = sub.pk "
        " WHERE mmsearch(t1.seq0, sub.seq0) "
        " ORDER BY t1.pk;",
        k_substring_self_table, k_substring_self_table);

    const auto rows = query_rows(ctx, sql);
    REQUIRE(rows.size() == 1);
    const auto& row = rows[0];
    REQUIRE(row.size() == 4);
    for (size_t col = 0; col < row.size(); ++col) {
        REQUIRE(row[col].has_value());
    }
    const long long full_len = static_cast<long long>(k_self_sequence.size());
    REQUIRE(std::stoll(row[1].value()) == 0);
    REQUIRE(std::stoll(row[2].value()) == full_len);   // half-open
    REQUIRE(is_exact_match_cigar(row[3].value(), full_len));
}

TEST_CASE("Five self-alignment flavors return the same coordinates and CIGAR",
          "[component:bioseq][layer:integration][feature:alignment][resource:postgres]") {
    // Cross-check: all five self-alignment flavors must return the
    // same rows. This is the strongest version of the property:
    // the result set is invariant under the SQL surface used.
    auto ctx = fresh_context({k_substring_self_table});
    seed_self_alignment_table(ctx, k_substring_self_table);

    auto canonical_query = [&ctx](std::string_view sql_template) {
        return query_rows(
            ctx,
            fmt::format(
                fmt::runtime(sql_template),
                k_substring_self_table, k_substring_self_table));
    };

    // Flavor 1: CROSS JOIN with WHERE t1.pk = t2.pk
    const auto rows_cross_join = canonical_query(
        "SELECT t1.pk, bioseq.target_start(t1.pk), bioseq.target_end(t1.pk), "
        "       bioseq.cigar(t1.pk) "
        "  FROM {} t1 CROSS JOIN {} t2 "
        " WHERE mmsearch(t1.seq0, t2.seq0) AND t1.pk = t2.pk "
        " ORDER BY t1.pk;");

    // Flavor 2: INNER JOIN ON t1.pk = t2.pk
    const auto rows_inner_join = canonical_query(
        "SELECT t1.pk, bioseq.target_start(t1.pk), bioseq.target_end(t1.pk), "
        "       bioseq.cigar(t1.pk) "
        "  FROM {} t1 INNER JOIN {} t2 ON t1.pk = t2.pk "
        " WHERE mmsearch(t1.seq0, t2.seq0) "
        " ORDER BY t1.pk;");

    // Flavor 3: same column on a single relation
    auto canonical_single = [&ctx](std::string_view sql_template) {
        return query_rows(
            ctx,
            fmt::format(fmt::runtime(sql_template), k_substring_self_table));
    };
    const auto rows_same_column = canonical_single(
        "SELECT pk, bioseq.target_start(pk), bioseq.target_end(pk), "
        "       bioseq.cigar(pk) "
        "  FROM {} "
        " WHERE mmsearch(seq0, seq0) "
        " ORDER BY pk;");

    // Flavor 4: different columns with same data
    const auto rows_diff_columns = canonical_single(
        "SELECT pk, bioseq.target_start(pk), bioseq.target_end(pk), "
        "       bioseq.cigar(pk) "
        "  FROM {} "
        " WHERE mmsearch(seq0, seq1) "
        " ORDER BY pk;");

    // Flavor 5: subquery returning same data
    const auto rows_subquery = canonical_query(
        "SELECT t1.pk, bioseq.target_start(t1.pk), bioseq.target_end(t1.pk), "
        "       bioseq.cigar(t1.pk) "
        "  FROM {} t1 "
        "  JOIN (SELECT pk, seq0 FROM {}) sub ON t1.pk = sub.pk "
        " WHERE mmsearch(t1.seq0, sub.seq0) "
        " ORDER BY t1.pk;");

    // All five flavors must produce exactly the same rows.
    REQUIRE_FALSE(rows_cross_join.empty());
    REQUIRE(rows_cross_join == rows_inner_join);
    REQUIRE(rows_cross_join == rows_same_column);
    REQUIRE(rows_cross_join == rows_diff_columns);
    REQUIRE(rows_cross_join == rows_subquery);
}

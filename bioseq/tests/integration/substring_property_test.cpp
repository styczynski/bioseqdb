/**
 * @file substring_property_test.cpp
 * @brief Verifies half-open mmsearch coordinates and exact-match CIGARs.
 *
 * A contiguous probe must begin at query offset zero and keep its target
 * offset. Full  self-alignment must produce the same result across five SQL
 * shapes.
 */

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view k_substring_table = "substring_alignment";
constexpr std::string_view k_substring_self_table = "self_alignment";

/// A 30 bp probe accepted by the chaining limits set in this fixture.
constexpr std::string_view k_probe_sequence = "CTATCATTCATACTTCTATCATTCATACTT"; // 30 bp

/// A padded target whose probe begins after 23 bases.
constexpr std::string_view k_padded_target =
    "AAAAAAAAAAAAAAAAAAAAAAACTATCATTCATACTTCTATCATTCATACTTAAAAAAAA";
// 23 A bases, the 30 bp probe, then 8 A bases.

/// Zero-based starl of the probe within the target.
constexpr long long k_expected_target_start = 23;
/// Exclusive probe endpoint within the padded target.
constexpr long long k_expected_target_end =
    k_expected_target_start + static_cast<long long>(k_probe_sequence.size());

/// Sequence used as both operands for full self-alignment.
constexpr std::string_view k_self_sequence = k_padded_target;

/** @brief Seeds one target and its embedded probe. */
void seed_substring_table(const connection_test_context& ctx, std::string_view table_name) {
    exec_sqlf(ctx,
              "SET bioseq.index.insert_maintenance.enabled = on;"
              "SET bioseq.aligner.chaining.minimum_score = 10;"
              "SET bioseq.aligner.chaining.minimum_seed_count = 1;");
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(table_name),
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 2, // seq0 stores the target, seq1 the probe.
            .using_bioseq = true,
        },
        {
            seq_row{.pk = 1,
                    .sequences = {std::string(k_padded_target), std::string(k_probe_sequence)}},
        });
}

/** @brief Seeds equal query and target columns for self-alignment. */
void seed_self_alignment_table(const connection_test_context& ctx, std::string_view table_name) {
    exec_sqlf(ctx, "SET bioseq.index.insert_maintenance.enabled = on;");
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string(table_name),
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 2, // seq0 == seq1 in every row
            .using_bioseq = true,
        },
        {
            seq_row{.pk = 1,
                    .sequences = {std::string(k_self_sequence), std::string(k_self_sequence)}},
        });
}

} // namespace

TEST_CASE("mmsearch(A, B) where B is a substring of A returns exact "
          "target_start / target_end / cigar coordinates",
          "[component:bioseq][layer:integration][feature:alignment][resource:postgres]") {
    // Request every alignment field so, missing values fail the test.
    auto ctx = fresh_context({k_substring_table});
    seed_substring_table(ctx, k_substring_table);

    const std::string sql = fmt::format("SELECT pk, "
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
    for (std::size_t column_index = 0; column_index < row.size(); ++column_index) {
        REQUIRE(row[column_index].has_value());
    }
    REQUIRE(std::stoll(row[0].value()) == 1);
    // Query coordinates cover the whole probe. Target coordinates include its
    // padding offset, and both end positions are exclusive
    REQUIRE(std::stoll(row[1].value()) == 0); // query_start
    REQUIRE(std::stoll(row[2].value()) ==
            static_cast<long long>(k_probe_sequence.size()));       // query_end (half-open)
    REQUIRE(std::stoll(row[3].value()) == k_expected_target_start); // target_start
    REQUIRE(std::stoll(row[4].value()) == k_expected_target_end);   // target_end (half-open)
    // This exact substring must produce one full-length match  in the CIGAR.
    REQUIRE(is_valid_cigar(row[5].value()));
    REQUIRE(is_exact_match_cigar(row[5].value(), static_cast<long long>(k_probe_sequence.size())));
}

TEST_CASE("mmsearch(A, A) — full self-alignment over a single column "
          "produces a full-length CIGAR",
          "[component:bioseq][layer:integration][feature:alignment][resource:postgres]") {
    // Align each row's seq0 value with itself. Both intervals must cover
    // [0, sequence length), and the CIGAR must describe a full-length match.
    auto ctx = fresh_context({k_substring_self_table});
    seed_self_alignment_table(ctx, k_substring_self_table);

    const std::string sql = fmt::format("SELECT pk, "
                                        "       bioseq.query_start(pk) AS qs, "
                                        "       bioseq.query_end(pk) AS qe, "
                                        "       bioseq.target_start(pk) AS ts, "
                                        "       bioseq.target_end(pk) AS te, "
                                        "       bioseq.cigar(pk) AS cig "
                                        "  FROM {} "
                                        " WHERE mmsearch(seq0, seq0) " // Both operands refer to seq0.
                                        " ORDER BY pk;",
                                        k_substring_self_table);

    const auto rows = query_rows(ctx, sql);
    REQUIRE(rows.size() == 1);
    const auto& row = rows[0];
    REQUIRE(row.size() == 6);
    for (std::size_t column_index = 0; column_index < row.size(); ++column_index) {
        REQUIRE(row[column_index].has_value());
    }
    const long long full_length = static_cast<long long>(k_self_sequence.size());
    REQUIRE(std::stoll(row[1].value()) == 0);
    REQUIRE(std::stoll(row[2].value()) == full_length); // half-open
    REQUIRE(std::stoll(row[3].value()) == 0);
    REQUIRE(std::stoll(row[4].value()) == full_length); // half-open
    REQUIRE(is_exact_match_cigar(row[5].value(), full_length));
}

TEST_CASE("nested mmsearch qualifier fails closed instead of executing the SQL stub",
          "[component:bioseq][layer:integration][feature:alignment][resource:postgres]") {
    auto ctx = fresh_context({k_substring_self_table});
    seed_self_alignment_table(ctx, k_substring_self_table);

    const auto error = sql_error(ctx,
                                 fmt::format("SELECT pk, bioseq.cigar(pk) FROM {} "
                                             "WHERE NOT mmsearch(seq0, seq0);",
                                             k_substring_self_table));
    REQUIRE(error.sqlstate == "0A000");
    REQUIRE(error.contains("nested mmsearch qualifier shape is unsupported"));
    REQUIRE(error.contains("executing its SQL stub without alignment is forbidden"));
}

TEST_CASE("Self-alignment via different columns containing the same data",
          "[component:bioseq][layer:integration][feature:alignment][resource:postgres]") {
    // Read the two operand columns independently. They contain identical bytes,
    // so aligning them must still return a full-length match.
    auto ctx = fresh_context({k_substring_self_table});
    seed_self_alignment_table(ctx, k_substring_self_table);

    const std::string sql =
        fmt::format("SELECT pk, "
                    "       bioseq.target_start(pk) AS ts, "
                    "       bioseq.target_end(pk) AS te, "
                    "       bioseq.cigar(pk) AS cig "
                    "  FROM {} "
                    " WHERE mmsearch(seq0, seq1) " // Separate columns with identical data.
                    " ORDER BY pk;",
                    k_substring_self_table);

    const auto rows = query_rows(ctx, sql);
    REQUIRE(rows.size() == 1);
    const auto& row = rows[0];
    REQUIRE(row.size() == 4);
    for (std::size_t column_index = 0; column_index < row.size(); ++column_index) {
        REQUIRE(row[column_index].has_value());
    }
    const long long full_length = static_cast<long long>(k_self_sequence.size());
    REQUIRE(std::stoll(row[1].value()) == 0);
    REQUIRE(std::stoll(row[2].value()) == full_length); // half-open
    REQUIRE(is_exact_match_cigar(row[3].value(), full_length));
}

TEST_CASE("Self-alignment via cross join with WHERE t1.pk = t2.pk",
          "[component:bioseq][layer:integration][feature:alignment][resource:postgres]") {
    // Restrict the cross join to rows with equal primary keys. Each sequence
    // then aligns with  itself and must produce a full-length match.
    auto ctx = fresh_context({k_substring_self_table});
    seed_self_alignment_table(ctx, k_substring_self_table);

    const std::string sql = fmt::format("SELECT t1.pk, "
                                        "       bioseq.target_start(t1.pk) AS ts, "
                                        "       bioseq.target_end(t1.pk) AS te, "
                                        "       bioseq.cigar(t1.pk) AS cig "
                                        "  FROM {} t1 CROSS JOIN {} t2 "
                                        " WHERE mmsearch(t1.seq0, t2.seq0) "
                                        "   AND t1.pk = t2.pk "
                                        " ORDER BY t1.pk;",
                                        k_substring_self_table,
                                        k_substring_self_table);

    const auto rows = query_rows(ctx, sql);
    REQUIRE(rows.size() == 1);
    const auto& row = rows[0];
    REQUIRE(row.size() == 4);
    for (std::size_t column_index = 0; column_index < row.size(); ++column_index) {
        REQUIRE(row[column_index].has_value());
    }
    const long long full_length = static_cast<long long>(k_self_sequence.size());
    REQUIRE(std::stoll(row[1].value()) == 0);
    REQUIRE(std::stoll(row[2].value()) == full_length); // half-open
    REQUIRE(is_exact_match_cigar(row[3].value(), full_length));
}

TEST_CASE("Self-alignment via INNER JOIN on t1.pk = t2.pk",
          "[component:bioseq][layer:integration][feature:alignment][resource:postgres]") {
    // Express the same row pairing with INNER JOIN. The coordinates and CIGAR
    // must match the self-alignment result.
    auto ctx = fresh_context({k_substring_self_table});
    seed_self_alignment_table(ctx, k_substring_self_table);

    const std::string sql = fmt::format("SELECT t1.pk, "
                                        "       bioseq.target_start(t1.pk) AS ts, "
                                        "       bioseq.target_end(t1.pk) AS te, "
                                        "       bioseq.cigar(t1.pk) AS cig "
                                        "  FROM {} t1 INNER JOIN {} t2 ON t1.pk = t2.pk "
                                        " WHERE mmsearch(t1.seq0, t2.seq0) "
                                        " ORDER BY t1.pk;",
                                        k_substring_self_table,
                                        k_substring_self_table);

    const auto rows = query_rows(ctx, sql);
    REQUIRE(rows.size() == 1);
    const auto& row = rows[0];
    REQUIRE(row.size() == 4);
    for (std::size_t column_index = 0; column_index < row.size(); ++column_index) {
        REQUIRE(row[column_index].has_value());
    }
    const long long full_length = static_cast<long long>(k_self_sequence.size());
    REQUIRE(std::stoll(row[1].value()) == 0);
    REQUIRE(std::stoll(row[2].value()) == full_length); // half-open
    REQUIRE(is_exact_match_cigar(row[3].value(), full_length));
}

TEST_CASE("Self-alignment via subquery returning identical data",
          "[component:bioseq][layer:integration][feature:alignment][resource:postgres]") {
    // Read one operand through a subquery. mmsearch must still use the sequence
    // values from both sides and return a full-length katch.
    auto ctx = fresh_context({k_substring_self_table});
    seed_self_alignment_table(ctx, k_substring_self_table);

    const std::string sql = fmt::format("SELECT t1.pk, "
                                        "       bioseq.target_start(t1.pk) AS ts, "
                                        "       bioseq.target_end(t1.pk) AS te, "
                                        "       bioseq.cigar(t1.pk) AS cig "
                                        "  FROM {} t1 "
                                        "  JOIN (SELECT pk, seq0 FROM {}) sub ON t1.pk = sub.pk "
                                        " WHERE mmsearch(t1.seq0, sub.seq0) "
                                        " ORDER BY t1.pk;",
                                        k_substring_self_table,
                                        k_substring_self_table);

    const auto rows = query_rows(ctx, sql);
    REQUIRE(rows.size() == 1);
    const auto& row = rows[0];
    REQUIRE(row.size() == 4);
    for (std::size_t column_index = 0; column_index < row.size(); ++column_index) {
        REQUIRE(row[column_index].has_value());
    }
    const long long full_length = static_cast<long long>(k_self_sequence.size());
    REQUIRE(std::stoll(row[1].value()) == 0);
    REQUIRE(std::stoll(row[2].value()) == full_length); // half-open
    REQUIRE(is_exact_match_cigar(row[3].value(), full_length));
}

TEST_CASE("Five self-alignment flavors return the same coordinates and CIGAR",
          "[component:bioseq][layer:integration][feature:alignment][resource:postgres]") {
    // Compare the returned rows across all five ways of expressing self-alignment.
    auto ctx = fresh_context({k_substring_self_table});
    seed_self_alignment_table(ctx, k_substring_self_table);

    auto canonical_query = [&ctx](std::string_view sql_template) {
        return query_rows(ctx,
                          fmt::format(fmt::runtime(sql_template),
                                      k_substring_self_table,
                                      k_substring_self_table));
    };

    // CROSS JOIN restricted to equal primary keys.
    const auto rows_cross_join =
        canonical_query("SELECT t1.pk, bioseq.target_start(t1.pk), bioseq.target_end(t1.pk), "
                        "       bioseq.cigar(t1.pk) "
                        "  FROM {} t1 CROSS JOIN {} t2 "
                        " WHERE mmsearch(t1.seq0, t2.seq0) AND t1.pk = t2.pk "
                        " ORDER BY t1.pk;");

    // INNER JOIN on equal primary keys.
    const auto rows_inner_join =
        canonical_query("SELECT t1.pk, bioseq.target_start(t1.pk), bioseq.target_end(t1.pk), "
                        "       bioseq.cigar(t1.pk) "
                        "  FROM {} t1 INNER JOIN {} t2 ON t1.pk = t2.pk "
                        " WHERE mmsearch(t1.seq0, t2.seq0) "
                        " ORDER BY t1.pk;");

    // The same column supplies both operands.
    auto canonical_single = [&ctx](std::string_view sql_template) {
        return query_rows(ctx, fmt::format(fmt::runtime(sql_template), k_substring_self_table));
    };
    const auto rows_same_column =
        canonical_single("SELECT pk, bioseq.target_start(pk), bioseq.target_end(pk), "
                         "       bioseq.cigar(pk) "
                         "  FROM {} "
                         " WHERE mmsearch(seq0, seq0) "
                         " ORDER BY pk;");

    // Separate columns contain  the same sequence.
    const auto rows_diff_columns =
        canonical_single("SELECT pk, bioseq.target_start(pk), bioseq.target_end(pk), "
                         "       bioseq.cigar(pk) "
                         "  FROM {} "
                         " WHERE mmsearch(seq0, seq1) "
                         " ORDER BY pk;");

    // A subquery supplies the matching row
    const auto rows_subquery =
        canonical_query("SELECT t1.pk, bioseq.target_start(t1.pk), bioseq.target_end(t1.pk), "
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

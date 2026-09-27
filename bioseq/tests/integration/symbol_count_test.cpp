/**
 * @file Tests for bioseq.symbol_count on literals and stored sequences.
 *
 * Checks base counts with vectorization on and off, stored index reads, and
 * recomputation when those reads are disabled. Error cases also check that
 * the same PostgreSQL connection  remains usable.
 */

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view k_bioseq_column = "symbol_count_columnar";

}  // namespace

// Counts from sequence literals.

TEST_CASE("scalar literal symbol_count returns the real tally (dna4)",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context();

    // 'ACGTACGTACGTACGT' has each of A/C/G/T exactly four times.
    REQUIRE(query_scalar_int(
                ctx, "SELECT bioseq.symbol_count("
                     "    'ACGTACGTACGTACGT'::bioseq.seq('dna4', 32), 'A');") == 4);
    REQUIRE(query_scalar_int(
                ctx, "SELECT bioseq.symbol_count("
                     "    'ACGTACGTACGTACGT'::bioseq.seq('dna4', 32), 'C');") == 4);
    REQUIRE(query_scalar_int(
                ctx, "SELECT bioseq.symbol_count("
                     "    'ACGTACGTACGTACGT'::bioseq.seq('dna4', 32), 'G');") == 4);
    REQUIRE(query_scalar_int(
                ctx, "SELECT bioseq.symbol_count("
                     "    'ACGTACGTACGTACGT'::bioseq.seq('dna4', 32), 'T');") == 4);

    // 'AAAAAAAA' is eight A's and nothing else.
    REQUIRE(query_scalar_int(
                ctx, "SELECT bioseq.symbol_count("
                     "    'AAAAAAAA'::bioseq.seq('dna4', 32), 'A');") == 8);
    REQUIRE(query_scalar_int(
                ctx, "SELECT bioseq.symbol_count("
                     "    'AAAAAAAA'::bioseq.seq('dna4', 32), 'C');") == 0);
}

TEST_CASE("scalar literal symbol_count returns the real tally (dna5)",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context();

#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    const auto error = sql_error(
        ctx, "SELECT bioseq.symbol_count("
             "    'NNNNN'::bioseq.seq('dna5', 32), 'N');");
    REQUIRE(error.sqlstate == "22023");
    REQUIRE(error.contains("bioseq.seq alphabet \"dna5\" is not supported"));
    REQUIRE(error.contains("Valid alphabets: 'dna4'"));
#else
    // DNA5 must count five N symbols and zero A symbols in NNNNN.
    REQUIRE(query_scalar_int(
                ctx, "SELECT bioseq.symbol_count("
                     "    'NNNNN'::bioseq.seq('dna5', 32), 'N');") == 5);
    REQUIRE(query_scalar_int(
                ctx, "SELECT bioseq.symbol_count("
                     "    'NNNNN'::bioseq.seq('dna5', 32), 'A');") == 0);

    // 'ACGTN' is one of each letter.
    REQUIRE(query_scalar_int(
                ctx, "SELECT bioseq.symbol_count("
                     "    'ACGTN'::bioseq.seq('dna5', 32), 'A');") == 1);
    REQUIRE(query_scalar_int(
                ctx, "SELECT bioseq.symbol_count("
                     "    'ACGTN'::bioseq.seq('dna5', 32), 'C');") == 1);
    REQUIRE(query_scalar_int(
                ctx, "SELECT bioseq.symbol_count("
                     "    'ACGTN'::bioseq.seq('dna5', 32), 'G');") == 1);
    REQUIRE(query_scalar_int(
                ctx, "SELECT bioseq.symbol_count("
                     "    'ACGTN'::bioseq.seq('dna5', 32), 'T');") == 1);
    REQUIRE(query_scalar_int(
                ctx, "SELECT bioseq.symbol_count("
                     "    'ACGTN'::bioseq.seq('dna5', 32), 'N');") == 1);
#endif
}

// Compare counts with vectorization on and off.

TEST_CASE("vectorization on/off agree on the scalar tally",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context();
    const auto rows = expect_invariant_under_guc_toggle(
        ctx,
        "SELECT bioseq.symbol_count("
        "  'ACGTACGTACGTACGT'::bioseq.seq('dna4', 32), 'A');",
        "bioseq.vectorization.enabled");
    REQUIRE(rows.size() == 1);
    REQUIRE(rows.front().size() == 1);
    REQUIRE(std::stoll(rows.front().front().value()) == 4);
}

// Counts from tables using the Bioseq access method.

TEST_CASE("columnar fast-path symbol_count returns the real tally + recompute fallback",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context({k_bioseq_column});

    SECTION("stored read path (bioseq.index.insert_maintenance.enabled = on)") {
        // The fixture stores A counts of 4, 4, and 16 for its three rows.
        // Insert maintenance builds the inline bymbol-count index.
        seed_small_bioseq_column(ctx, k_bioseq_column);

        // pk=1 'ACGTACGTACGTACGT' -> A=4.
        REQUIRE(query_scalar_int(
                    ctx,
                    fmt::format("SELECT bioseq.symbol_count(seq0, 'A') FROM {} "
                                "WHERE pk = 1;",
                                k_bioseq_column)) == 4);
        // pk=3 'AAAAAAAAAAAAAAAA' -> A=16.
        REQUIRE(query_scalar_int(
                    ctx,
                    fmt::format("SELECT bioseq.symbol_count(seq0, 'A') FROM {} "
                                "WHERE pk = 3;",
                                k_bioseq_column)) == 16);

        // Every stored row must return a non-NULL, nonnegative A count.
        const auto rows = query_rows(
            ctx,
            fmt::format("SELECT pk, bioseq.symbol_count(seq0, 'A') AS ca FROM {} "
                        "ORDER BY pk;",
                        k_bioseq_column));
        REQUIRE(rows.size() == 3);
        for (const auto& row : rows) {
            REQUIRE(row.size() == 2);
            REQUIRE(row[1].has_value());
            REQUIRE(std::stoll(row[1].value()) >= 0);
        }
    }

    SECTION("recompute fallback (bioseq.index.insert_maintenance.enabled = off, "
            "index.sequence_read.enabled = off)") {
        seed_small_bioseq_column(ctx, k_bioseq_column);

        // Insert a fourth row with symbol-count index maintenance disabled.
        {
            guc_scope insert_maintenance_disabled(
                ctx, "bioseq.index.insert_maintenance.enabled", "off");
            exec_sqlf(ctx,
                      "INSERT INTO {} VALUES "
                      "(4, 'ACGTACGTACGTACGT'::bioseq.seq('dna4', 32));",
                      k_bioseq_column);
        }

        // Disable stored index reads so this row's A count must  be recomputed.
        {
            guc_scope sequence_reads_disabled(
                ctx, "bioseq.index.sequence_read.enabled", "off");
            ctx.clear_notices();
            const auto row = query_one(
                ctx,
                fmt::format("SELECT bioseq.symbol_count(seq0, 'A') FROM {} "
                            "WHERE pk = 4;",
                            k_bioseq_column));
            REQUIRE(row.size() == 1);
            REQUIRE(row[0].has_value());  // This sequence must produce a count.
            REQUIRE(std::stoll(row[0].value()) == 4);
            REQUIRE_FALSE(ctx.notice_contains("resource was not closed"));

            // Raise a projection error after computing the count. The backend must
            // remain usable and report no unclosed resources.
            ctx.clear_notices();
            const auto projection_error = sql_error(
                ctx,
                fmt::format(
                    "SELECT bioseq.symbol_count(seq0, 'A') + 1 / (pk - pk) "
                    "FROM {} WHERE pk = 4;",
                    k_bioseq_column));
            REQUIRE(projection_error.sqlstate == "22012");
            REQUIRE_FALSE(ctx.notice_contains("resource was not closed"));
            REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
        }
    }
}

TEST_CASE("inline Sequence PostgreSQL leaves preserve exact error and same-backend recovery",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context({k_bioseq_column});
    seed_small_bioseq_column(ctx, k_bioseq_column);
    // Disable, primary-key index scans so this query uses the stored symbol-count
    // path where the PostgreSQL failures are injected.
    guc_scope index_scans_disabled(ctx, "enable_indexscan", "off");
    guc_scope sequence_index_reads_enabled(
        ctx, "bioseq.index.sequence_read.enabled", "on");
    const std::string read_sql = fmt::format(
        "SELECT bioseq.symbol_count(seq0, 'A') FROM {} WHERE pk = 1;",
        k_bioseq_column);
    REQUIRE(query_scalar_int(ctx, read_sql) == 4);

    for (const std::string_view failure_point : {
            std::string_view{"relation_lookup"},
            std::string_view{"tuple_read"},
            std::string_view{"detoast"},
            std::string_view{"close"}}) {
        exec_sqlf(ctx,
            "SELECT bioseq_internal.arm_inline_sequence_pg_failure('{}');",
            failure_point);
        ctx.clear_notices();
        const auto error = sql_error(ctx, read_sql);
        REQUIRE(error.sqlstate == "58030");
        REQUIRE(error.contains(
            "injected inline sequence storage PostgreSQL leaf failure"));
        REQUIRE(error.contains(
            fmt::format("failure_point={}", failure_point)));
        REQUIRE_FALSE(ctx.notice_contains("resource was not closed"));

        // Each injected failure fires once A trivial statement and another stored
        // count read must then succeed on the same connection.
        REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
        REQUIRE(query_scalar_int(ctx, read_sql) == 4);
        REQUIRE_FALSE(ctx.notice_contains("resource was not closed"));
    }
}

/**
 * @file Integration scenarios for `bioseq.symbol_count`.
 *
 * Literal tallies cover every enabled alphabet and vectorization parity.
 * Columnar cases verify both the stored INLINE symbol-count chunk and the
 * unconditional recompute fallback when index maintenance and reads are off.
 */

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view k_bioseq_column = "symbol_count_columnar";

}  // namespace

// ---- 1. Literal scalar path -------------------------------------------------

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
    // 'NNNNN' is five N's; in dna5 the rank of N is 3, so N=5 and every other
    // letter is 0. The typmod packs dna5 in 3 bits/char (width 32 is fine).
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

// ---- 2. Vectorization parity ------------------------------------------------

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

// ---- 3. BioseqColumn table (`USING bioseq`) -------------------------------------

TEST_CASE("columnar fast-path symbol_count returns the real tally + recompute fallback",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context({k_bioseq_column});

    SECTION("stored read path (bioseq.index.insert_maintenance.enabled = on)") {
        // seed_small_bioseq_column inserts pk=1 'ACGTACGTACGTACGT' (A=4),
        // pk=2 'TTTTAAAAGGGGCCCC' (A=4), pk=3 'AAAAAAAAAAAAAAAA' (A=16),
        // all under bioseq.index.insert_maintenance.enabled = on so the INLINE
        // symbol_counts chunk is populated.
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

        // Every stored row's A-count must be a non-NULL real tally.
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

        // Insert a fourth row WITHOUT populating the stored INLINE index.
        {
            guc_scope insert_maintenance_disabled(
                ctx, "bioseq.index.insert_maintenance.enabled", "off");
            exec_sqlf(ctx,
                      "INSERT INTO {} VALUES "
                      "(4, 'ACGTACGTACGTACGT'::bioseq.seq('dna4', 32));",
                      k_bioseq_column);
        }

        // With the stored read disabled, the unconditional recompute fallback
        // must still return the correct NON-NULL real tally (A=4) — never NULL.
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
            REQUIRE(row[0].has_value());  // recompute never yields NULL
            REQUIRE(std::stoll(row[0].value()) == 4);
            REQUIRE_FALSE(ctx.notice_contains("resource was not closed"));

            // Abort the executor after BioseqScan has populated the placeholder.
            // The helper has already performed its normal cleanup at this
            // point; this verifies that it leaves no ResourceOwner debt for a
            // later projection error. The helper's internal PG_CATCH path is
            // established by the heap-resident state and per-cleanup
            // saved-ErrorData guards.
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
    // Keep the access method from hydrating the locator through a primary-key
    // IndexScan, while enabling the stored symbol-count path that traverses
    // the injected PostgreSQL sequence leaves.
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

        // The hook is one-shot. Both SPI/executor use in a trivial statement
        // and a second real inline-index read must succeed in this backend.
        REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
        REQUIRE(query_scalar_int(ctx, read_sql) == 4);
        REQUIRE_FALSE(ctx.notice_contains("resource was not closed"));
    }
}

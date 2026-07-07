// Integration scenarios for bioseq.symbol_count (DOC0279 §12.5):
//   * The deleted POC 49-stub is gone: bioseq.symbol_count(LITERAL, 'L') now
//     returns the REAL per-rank tally (tally_symbol_counts) for both dna4 and
//     dna5 literals on the scalar path (no row context).
//   * The scalar result is identical whether bioseq.enable_vectorization is on
//     or off — the SIMD path must not diverge from the scalar path.
//   * On a USING bioseq columnar table the custom-scan VarInjector rewrites
//     symbol_count(Var, 'L') into a per-row placeholder Const; with
//     bioseq.read_sequence_index = on the stored INLINE symbol_counts chunk is
//     preferred, but the unconditional recompute fallback guarantees a
//     non-NULL real tally for every present row even with the index disabled
//     and read_sequence_index = off.
//
// Real-tally goldens (counted directly from the literals):
//   'ACGTACGTACGTACGT'::dna4 -> A=4, C=4, G=4, T=4
//   'AAAAAAAA'::dna4         -> A=8, C=G=T=0
//   'NNNNN'::dna5            -> N=5 (rank of N in dna5 is 4), A=C=G=T=0
//   'ACGTN'::dna5            -> A=1, C=1, G=1, T=1, N=1

#include "common_tests_integration.h"
import bioseq.integration_tests.common;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view kBioseqColumn = "test_symcount_cx";

}  // namespace

// ---- 1. Literal scalar path -------------------------------------------------

TEST_CASE("scalar literal symbol_count returns the real tally (dna4)",
          "[symbol_count][literal]") {
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
          "[symbol_count][literal][dna5]") {
    auto ctx = fresh_context();

    // 'NNNNN' is five N's; in dna5 the rank of N is 4, so N=5 and every other
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
}

// ---- 2. Vectorization parity ------------------------------------------------

TEST_CASE("vectorization on/off agree on the scalar tally",
          "[symbol_count][literal][parity]") {
    auto ctx = fresh_context();
    const auto rows = expect_invariant_under_guc_toggle(
        ctx,
        "SELECT bioseq.symbol_count("
        "  'ACGTACGTACGTACGT'::bioseq.seq('dna4', 32), 'A');",
        "bioseq.enable_vectorization");
    REQUIRE(rows.size() == 1);
    REQUIRE(rows.front().size() == 1);
    REQUIRE(std::stoll(rows.front().front().value()) == 4);
}

// ---- 3. BioseqColumn table (`USING bioseq`) -------------------------------------

TEST_CASE("columnar fast-path symbol_count returns the real tally + recompute fallback",
          "[symbol_count][columnar]") {
    auto ctx = fresh_context({kBioseqColumn});

    SECTION("stored read path (update_index_on_insert = on)") {
        // seed_small_bioseq_column inserts pk=1 'ACGTACGTACGTACGT' (A=4),
        // pk=2 'TTTTAAAAGGGGCCCC' (A=4), pk=3 'AAAAAAAAAAAAAAAA' (A=16),
        // all under bioseq.update_index_on_insert = on so the INLINE
        // symbol_counts chunk is populated.
        seed_small_bioseq_column(ctx, kBioseqColumn);

        // pk=1 'ACGTACGTACGTACGT' -> A=4.
        REQUIRE(query_scalar_int(
                    ctx,
                    fmt::format("SELECT bioseq.symbol_count(seq0, 'A') FROM {} "
                                "WHERE pk = 1;",
                                kBioseqColumn)) == 4);
        // pk=3 'AAAAAAAAAAAAAAAA' -> A=16.
        REQUIRE(query_scalar_int(
                    ctx,
                    fmt::format("SELECT bioseq.symbol_count(seq0, 'A') FROM {} "
                                "WHERE pk = 3;",
                                kBioseqColumn)) == 16);

        // Every stored row's A-count must be a non-NULL real tally.
        const auto rows = query_rows(
            ctx,
            fmt::format("SELECT pk, bioseq.symbol_count(seq0, 'A') AS ca FROM {} "
                        "ORDER BY pk;",
                        kBioseqColumn));
        REQUIRE(rows.size() == 3);
        for (const auto& row : rows) {
            REQUIRE(row.size() == 2);
            REQUIRE(row[1].has_value());
            REQUIRE(std::stoll(row[1].value()) >= 0);
        }
    }

    SECTION("recompute fallback (update_index_on_insert = off, "
            "read_sequence_index = off)") {
        seed_small_bioseq_column(ctx, kBioseqColumn);

        // Insert a fourth row WITHOUT populating the stored INLINE index.
        {
            guc_scope no_insert(ctx, "bioseq.update_index_on_insert", "off");
            exec_sqlf(ctx,
                      "INSERT INTO {} VALUES "
                      "(4, 'ACGTACGTACGTACGT'::bioseq.seq('dna4', 32));",
                      kBioseqColumn);
        }

        // With the stored read disabled, the unconditional recompute fallback
        // must still return the correct NON-NULL real tally (A=4) — never NULL.
        {
            guc_scope no_read(ctx, "bioseq.read_sequence_index", "off");
            const auto row = query_one(
                ctx,
                fmt::format("SELECT bioseq.symbol_count(seq0, 'A') FROM {} "
                            "WHERE pk = 4;",
                            kBioseqColumn));
            REQUIRE(row.size() == 1);
            REQUIRE(row[0].has_value());  // recompute never yields NULL
            REQUIRE(std::stoll(row[0].value()) == 4);
        }
    }
}

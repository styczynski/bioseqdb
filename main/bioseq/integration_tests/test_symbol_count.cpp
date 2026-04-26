// Integration scenarios:
//   * `bioseq.symbol_count(LITERAL, 'A')` returns the POC-hardcoded
//     `kSymbolCountPocFor_A` on the scalar tier-3 path (no row context),
//     and `kSymbolCountPocOther` for non-rank-0 letters in this POC.
//   * The result is identical whether `bioseq.enable_vectorization` is on or
//     off — the SIMD path must not diverge from the scalar path.
//   * The POC stub also fires for `dna5` rank-0 letter ('A'), and for `'N'`
//     on dna5 (rank 3) returns the "non-rank-0" value.
//   * On a `USING bioseq` columnar table with the meta_sidecar populated,
//     the custom-scan VarInjector rewrites symbol_count into a per-row Const
//     that comes from the sidecar; rows inserted while
//     `bioseq.update_index_on_insert = off` lack a sidecar entry and yield
//     NULL for that placeholder.
//
// We use TEMPLATED_TEST_CASE_SIG over (letter, expected) pairs to express
// the dna4 rank-membership matrix in one declarative table.

#include "common_tests_integration.h"
import integration_tests.common;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view kColumnar = "test_symcount_cx";

}  // namespace

// ---- 1. Literal scalar path -------------------------------------------------

TEST_CASE("bioseq.symbol_count literal returns 49 for rank-0 letter (POC)",
          "[symbol_count][literal]") {
    auto ctx = fresh_context();

    // 'A' on a sequence whose visible 'A' count is much smaller than 49
    // proves the POC value is hardcoded (rather than computed from the
    // sequence).
    const long long c_arich = query_scalar_int(
        ctx,
        "SELECT bioseq.symbol_count("
        "    'AAAAAAAAAAAAAAAAAAAA'::bioseq.seq('dna4', 32), 'A');");
    const long long c_apoor = query_scalar_int(
        ctx,
        "SELECT bioseq.symbol_count("
        "    'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32), 'A');");
    REQUIRE(c_arich == kSymbolCountPocFor_A);
    REQUIRE(c_apoor == kSymbolCountPocFor_A);
}

TEMPLATE_TEST_CASE_SIG(
    "bioseq.symbol_count literal: only rank-0 letter returns the POC value",
    "[symbol_count][literal][matrix]",
    ((char Letter, long long Expected), Letter, Expected),
    ('A', kSymbolCountPocFor_A),
    ('C', kSymbolCountPocOther),
    ('G', kSymbolCountPocOther),
    ('T', kSymbolCountPocOther)) {
    auto ctx = fresh_context();
    const long long actual = query_scalar_int(
        ctx,
        fmt::format(
            "SELECT bioseq.symbol_count("
            "    'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32), '{}');",
            Letter));
    REQUIRE(actual == Expected);
}

TEST_CASE("bioseq.symbol_count literal returns the POC value for dna5 'A' too",
          "[symbol_count][literal][dna5]") {
    auto ctx = fresh_context();
    REQUIRE(query_scalar_int(
                ctx, "SELECT bioseq.symbol_count('NNNNN'::bioseq.seq('dna5', 32),"
                     " 'A');") == kSymbolCountPocFor_A);
    REQUIRE(query_scalar_int(
                ctx, "SELECT bioseq.symbol_count('NNNNN'::bioseq.seq('dna5', 32),"
                     " 'N');") == kSymbolCountPocOther);
}

// ---- 2. Vectorization parity ------------------------------------------------

TEST_CASE("bioseq.symbol_count literal: vectorization on/off agree",
          "[symbol_count][literal][parity]") {
    auto ctx = fresh_context();
    const auto rows = expect_invariant_under_guc_toggle(
        ctx,
        "SELECT bioseq.symbol_count("
        "  'AAAAAAAAAAAAAAAAAAAA'::bioseq.seq('dna4', 32), 'A');",
        "bioseq.enable_vectorization");
    REQUIRE(rows.size() == 1);
    REQUIRE(rows.front().size() == 1);
    REQUIRE(std::stoll(rows.front().front().value()) ==
            kSymbolCountPocFor_A);
}

// ---- 3. Columnar table (`USING bioseq`) with meta_sidecar -------------------

TEST_CASE("bioseq.symbol_count on columnar table returns POC value when matches surface",
          "[symbol_count][columnar]") {
    auto ctx = fresh_context({kColumnar});
    seed_small_columnar(ctx, kColumnar);

    const auto rows = query_rows(
        ctx, fmt::format(
                 "SELECT pk, bioseq.symbol_count(seq0, 'A') AS ca FROM {} "
                 "WHERE mmsearch(seq0, 'AAAA'::bioseq.seq('dna4', 32)) "
                 "ORDER BY pk;",
                 kColumnar));
    for (const auto& row : rows) {
        REQUIRE(row.size() == 2);
        if (row[1].has_value()) {
            REQUIRE(std::stoll(row[1].value()) == kSymbolCountPocFor_A);
        }
    }
}

TEST_CASE("symbol_count produces identical rows in vector and scalar paths",
          "[symbol_count][columnar][parity]") {
    auto ctx = fresh_context({kColumnar});
    seed_small_columnar(ctx, kColumnar);

    const std::string sql = fmt::format(
        "SELECT pk, bioseq.symbol_count(seq0, 'A') AS ca FROM {} "
        "WHERE mmsearch(seq0, 'AAAA'::bioseq.seq('dna4', 32)) "
        "ORDER BY pk;",
        kColumnar);
    expect_invariant_under_guc_toggle(ctx, sql,
                                      "bioseq.enable_vectorization");
}

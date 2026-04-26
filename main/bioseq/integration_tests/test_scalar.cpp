// Integration scenarios:
//   * Calling bioseq SQL functions directly on literals (no table) hits the
//     scalar path of every projection helper.
//   * `bioseq.symbol_count` on a literal returns the POC-hardcoded value
//     for rank-0 letters and the "other" value for everything else, with
//     vectorisation toggled either way (parity check against the SIMD path).
//   * `bioseq.len` / `bioseq.unit_len` on the empty sequence return 0 and
//     do not error out under any width.

#include "common_tests_integration.h"
import integration_tests.common;

using namespace bioseq_int_tests;

TEST_CASE("bioseq.symbol_count over a literal stays at the POC value across vectorization",
          "[scalar][symbol_count]") {
    auto ctx = fresh_context();
    const auto reference = expect_invariant_under_guc_toggle(
        ctx,
        "SELECT bioseq.symbol_count("
        "    'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32), 'A');",
        "bioseq.enable_vectorization");
    REQUIRE(reference.size() == 1);
    REQUIRE(reference.front().size() == 1);
    REQUIRE(std::stoll(reference.front().front().value()) ==
            kSymbolCountPocFor_A);
}

TEMPLATE_TEST_CASE_SIG(
    "bioseq.len / bioseq.unit_len agree across widths on the empty sequence",
    "[scalar][len][matrix]",
    ((int Width), Width),
    8, 16, 32, 64) {
    auto ctx = fresh_context();
    const seq_typmod typmod{.alphabet = "dna4", .width = Width};
    const auto row = query_one(
        ctx,
        fmt::format("SELECT bioseq.len({}), bioseq.unit_len({});",
                    format_literal("", typmod), format_literal("", typmod)));
    REQUIRE(row[0].value() == "0");
    REQUIRE(row[1].value() == "0");
}

TEST_CASE("bioseq.symbol_count returns 0 for non-rank-0 letters in this POC",
          "[scalar][symbol_count][poc]") {
    auto ctx = fresh_context();
    const auto row = query_one(
        ctx,
        "SELECT bioseq.symbol_count("
        "    'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32), 'C'), "
        "       bioseq.symbol_count("
        "    'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32), 'G'), "
        "       bioseq.symbol_count("
        "    'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32), 'T');");
    REQUIRE(std::stoll(row[0].value()) == kSymbolCountPocOther);
    REQUIRE(std::stoll(row[1].value()) == kSymbolCountPocOther);
    REQUIRE(std::stoll(row[2].value()) == kSymbolCountPocOther);
}

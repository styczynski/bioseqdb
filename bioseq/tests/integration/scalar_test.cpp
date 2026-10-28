/**
 * @file
 * @brief Tests SQL sequence lengths and base counts.
 */

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

#include <catch2/catch_template_test_macros.hpp>

import std;
import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

// The 29-base fixture contains A=6, C=7, G=7, and T=9.
TEST_CASE("bioseq.symbol_count over a literal returns the real tally across vectorization",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context();
    const auto reference = expect_invariant_under_guc_toggle(
        ctx,
        "SELECT bioseq.symbol_count("
        "    'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 64), 'A');",
        "bioseq.vectorization.enabled");
    REQUIRE(reference.size() == 1);
    REQUIRE(reference.front().size() == 1);
    REQUIRE(std::stoll(reference.front().front().value()) == 6); // A count
}

TEMPLATE_TEST_CASE_SIG("bioseq.len / bioseq.unit_len agree across widths on the empty sequence",
                       "[component:bioseq][layer:integration][feature:sequence][resource:postgres]",
                       ((int p_width), p_width),
                       8,
                       16,
                       32,
                       64) {
    auto ctx = fresh_context();
    const seq_typmod typmod{.alphabet = "dna4", .width = p_width};
#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_COORDINATES)
    if constexpr (p_width != 64) {
        const auto disabled_width = sql_error(
            ctx, fmt::format("SELECT {}::text;", format_literal("", typmod)));
        REQUIRE(disabled_width.sqlstate == "22023");
        REQUIRE(disabled_width.contains(fmt::format(
            "bioseq.seq width {} is not enabled in this build", p_width)));
        return;
    }
#endif
    const auto row = query_one(ctx,
                               fmt::format("SELECT bioseq.len({}), bioseq.unit_len({});",
                                           format_literal("", typmod),
                                           format_literal("", typmod)));
    REQUIRE(row[0].value() == "0");
    REQUIRE(row[1].value() == "0");
}

TEST_CASE("bioseq.symbol_count returns the real per-letter tally for C/G/T",
          "[component:bioseq][layer:integration][feature:sequence][resource:postgres]") {
    auto ctx = fresh_context();
    const auto row =
        query_one(ctx,
                  "SELECT bioseq.symbol_count("
                  "    'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 64), 'C'), "
                  "       bioseq.symbol_count("
                  "    'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 64), 'G'), "
                  "       bioseq.symbol_count("
                  "    'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 64), 'T');");
    REQUIRE(std::stoll(row[0].value()) == 7); // C
    REQUIRE(std::stoll(row[1].value()) == 7); // G
    REQUIRE(std::stoll(row[2].value()) == 9); // T
}

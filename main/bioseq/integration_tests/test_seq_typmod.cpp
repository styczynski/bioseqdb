// Integration scenarios:
//   * Encoding / decoding roundtrip across the full (alphabet, width) matrix
//     to exercise each dispatch arm in pg_compact_sequence_in / _out.
//   * Long-input SIMD encoder coverage (>=512 chars) so the batched encoder
//     paths run with `complete_units >= L_out` for both narrow and wide
//     storage units.
//   * Cross-typmod casting `'X'::bioseq.seq('dna4', W)` for every width.
//
// We use TEMPLATE_TEST_CASE_SIG over (alphabet, width) instead of repeating
// eight near-identical cases; the alphabet is encoded as a non-type template
// parameter via a small enum.

#include "common_tests_integration.h"
import integration_tests.common;

using namespace bioseq_int_tests;

namespace {

enum class alphabet_id : int { dna4 = 0, dna5 = 1 };

constexpr std::string_view alphabet_name(alphabet_id a) {
    return a == alphabet_id::dna4 ? "dna4" : "dna5";
}

constexpr std::string_view sample_for(alphabet_id a) {
    return a == alphabet_id::dna4 ? "ACGTACGTACGTACGT" : "ACGTNACGTNACGTNACGTNACGTN";
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "bioseq.seq encode/decode roundtrip across full typmod matrix",
    "[seq][typmod][matrix]",
    ((alphabet_id Alpha, int Width), Alpha, Width),
    (alphabet_id::dna4, 8),
    (alphabet_id::dna4, 16),
    (alphabet_id::dna4, 32),
    (alphabet_id::dna4, 64),
    (alphabet_id::dna5, 8),
    (alphabet_id::dna5, 16),
    (alphabet_id::dna5, 32),
    (alphabet_id::dna5, 64)) {
    auto ctx = fresh_context();

    constexpr seq_typmod typmod{.alphabet = alphabet_name(Alpha),
                                .width = Width};
    const std::string sample(sample_for(Alpha));
    const auto literal = format_literal(sample, typmod);

    const auto row = query_one(
        ctx, fmt::format("SELECT {0}::text AS roundtrip, "
                         "       bioseq.len({0}) AS bp, "
                         "       bioseq.unit_len({0}) AS units;",
                         literal));
    REQUIRE(row[0].value() == sample);
    REQUIRE(std::stoll(row[1].value()) ==
            static_cast<long long>(sample.size()));
    REQUIRE(std::stoll(row[2].value()) > 0);
}

TEMPLATE_TEST_CASE_SIG(
    "bioseq.seq SIMD encoder roundtrips long payloads",
    "[seq][typmod][simd]",
    ((int Width), Width),
    8, 16, 32, 64) {
    auto ctx = fresh_context();
    const seq_typmod typmod{.alphabet = "dna4", .width = Width};
    const std::string typmod_sql = typmod.sql();

    const auto row = query_one(
        ctx, fmt::format(
                 "WITH long_input AS (SELECT repeat('ACGT', 128) AS s) "
                 "SELECT length(s) AS in_len, "
                 "       bioseq.len(s::{0}) AS bp, "
                 "       (s::{0}::text = s) AS roundtrip "
                 "FROM long_input;",
                 typmod_sql));
    REQUIRE(row[0].value() == "512");
    REQUIRE(row[1].value() == "512");
    REQUIRE(row[2].value() == "t");
}

TEMPLATE_TEST_CASE_SIG(
    "bioseq.seq cross-typmod cast: dna4 strips N, dna5 keeps N",
    "[seq][typmod][cast]",
    ((int Width), Width),
    8, 16, 32, 64) {
    auto ctx = fresh_context();
    const auto row = query_one(
        ctx,
        fmt::format("SELECT 'ACGTN'::bioseq.seq('dna4', {})::text, "
                    "       'ACGTN'::bioseq.seq('dna5', {})::text;",
                    Width, Width));
    REQUIRE(row[0].value() == "ACGTA");
    REQUIRE(row[1].value() == "ACGTN");
}

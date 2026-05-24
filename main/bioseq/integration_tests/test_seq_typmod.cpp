// Integration scenarios:
//   * Encoding / decoding roundtrip across the full (alphabet, width) matrix
//     to exercise each dispatch arm in pg_compact_sequence_in / _out.
//   * Long-input SIMD encoder coverage (>=512 chars) so the batched encoder
//     paths run with `complete_units >= L_out` for both narrow and wide
//     storage units.
//   * Cross-typmod casting `'X'::bioseq.seq('dna4', W)` for every width.
//
// We use TEMPLATE_TEST_CASE_SIG over (alphabet_entry, width) instead of
// repeating eight near-identical cases; DOC0261 reparameterises the matrix
// to pin a `pgseq_supported_alphabets::entry_for<seq::dnaX>` brand directly
// so the alphabet name string is sourced from the production pack.

#include "common_tests_integration.h"
import integration_tests.common;

using namespace bioseq_int_tests;

namespace {

// DOC0261 §11.2 — exhaustive sample picker. Adding a new alphabet to
// the pack forces a build break here so the test fixture is updated in
// lockstep with R1's "pack is the single source of truth".
template <class> inline constexpr bool always_false_v = false;

template <class Entry>
constexpr std::string_view sample_for() {
    using A = typename Entry::alphabet_type;
    if constexpr (std::is_same_v<A, seq_entry_tag::dna4>) {
        return "ACGTACGTACGTACGT";
    } else if constexpr (std::is_same_v<A, seq_entry_tag::dna5>) {
        return "ACGTNACGTNACGTNACGTNACGTN";
    } else {
        static_assert(always_false_v<Entry>,
            "sample_for: no sample defined for this alphabet — "
            "add a clause when the pack grows");
    }
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "bioseq.seq encode/decode roundtrip across full typmod matrix",
    "[seq][typmod][matrix]",
    ((class Entry, int Width), Entry, Width),
    (Dna4Entry, 8),
    (Dna4Entry, 16),
    (Dna4Entry, 32),
    (Dna4Entry, 64),
    (Dna5Entry, 8),
    (Dna5Entry, 16),
    (Dna5Entry, 32),
    (Dna5Entry, 64)) {
    auto ctx = fresh_context();

    constexpr seq_typmod typmod = make_seq_typmod<Entry>(Width);
    const std::string sample(sample_for<Entry>());
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
    const seq_typmod typmod = make_seq_typmod<Dna4Entry>(Width);
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

/**
 * @file sequence_typmod_test.cpp
 * @brief Verifies sequence typmods across supported alphabets and widths.
 *
 * The cases cover text round trips, long SIMD encoding, and cross-typmod
 * casts. The template matrix takes alphabet names from the production pack.
 */

#include <tests/support/postgres.hpp>
import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {

/// Fails compilation when the alphabet fixture has no representative sample.
template <class> inline constexpr bool always_false_v = false;

/** @brief Returns a representative sequence for the selected production alphabet. */
template <class p_entry> constexpr std::string_view sample_for() {
    using alphabet_type = typename p_entry::alphabet_type;
    if constexpr (std::is_same_v<alphabet_type, seq_entry_tag::dna4>) {
        return "ACGTACGTACGTACGT";
    } else if constexpr (std::is_same_v<alphabet_type, seq_entry_tag::dna5>) {
        return "ACGTNACGTNACGTNACGTNACGTN";
    } else {
        static_assert(always_false_v<p_entry>,
                      "sample_for: no sample defined for this alphabet — "
                      "add a clause when the pack grows");
    }
}

} // namespace

TEMPLATE_TEST_CASE_SIG("bioseq.seq encode/decode roundtrip across full typmod matrix",
                       "[component:bioseq][layer:integration][feature:alphabet][resource:postgres]",
                       ((class p_entry, int p_width), p_entry, p_width),
                       (dna4_entry, 8),
                       (dna4_entry, 16),
                       (dna4_entry, 32),
                       (dna4_entry, 64),
                       (dna5_entry, 8),
                       (dna5_entry, 16),
                       (dna5_entry, 32),
                       (dna5_entry, 64)) {
    auto ctx = fresh_context();

    constexpr seq_typmod typmod = make_seq_typmod<p_entry>(p_width);
    const std::string sample(sample_for<p_entry>());
    const auto literal = format_literal(sample, typmod);

#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    if constexpr (std::is_same_v<typename p_entry::alphabet_type, seq_entry_tag::dna5>) {
        const auto unsupported_alphabet = sql_error(ctx, fmt::format("SELECT {}::text;", literal));
        REQUIRE(unsupported_alphabet.sqlstate == "22023");
        REQUIRE(unsupported_alphabet.contains("bioseq.seq alphabet \"dna5\" is not supported"));
        REQUIRE(unsupported_alphabet.contains("Valid alphabets: 'dna4'"));
    } else
#endif
    {
        const auto row = query_one(ctx,
                                   fmt::format("SELECT {0}::text AS roundtrip, "
                                               "       bioseq.len({0}) AS bp, "
                                               "       bioseq.unit_len({0}) AS units;",
                                               literal));
        REQUIRE(row[0].value() == sample);
        REQUIRE(std::stoll(row[1].value()) == static_cast<long long>(sample.size()));
        REQUIRE(std::stoll(row[2].value()) > 0);
    }
}

TEMPLATE_TEST_CASE_SIG(
    "bioseq.seq SIMD encoder roundtrips long payloads",
    "[component:bioseq][layer:integration][feature:alphabet][resource:postgres][resource:highway]",
    ((int p_width), p_width),
    8,
    16,
    32,
    64) {
    auto ctx = fresh_context();
    const seq_typmod typmod = make_seq_typmod<dna4_entry>(p_width);
    const std::string typmod_sql = typmod.sql();

    const auto row = query_one(ctx,
                               fmt::format("WITH long_input AS (SELECT repeat('ACGT', 128) AS s) "
                                           "SELECT length(s) AS in_len, "
                                           "       bioseq.len(s::{0}) AS bp, "
                                           "       (s::{0}::text = s) AS roundtrip "
                                           "FROM long_input;",
                                           typmod_sql));
    REQUIRE(row[0].value() == "512");
    REQUIRE(row[1].value() == "512");
    REQUIRE(row[2].value() == "t");
}

TEMPLATE_TEST_CASE_SIG("bioseq.seq cross-typmod cast follows the active alphabet profile",
                       "[component:bioseq][layer:integration][feature:alphabet][resource:postgres]",
                       ((int p_width), p_width),
                       8,
                       16,
                       32,
                       64) {
    auto ctx = fresh_context();
    constexpr int source_width = p_width == 8 ? 16 : 8;
    REQUIRE(query_scalar(ctx,
                         fmt::format("SELECT ('ACGTA'::bioseq.seq('dna4', {}))"
                                     "       ::bioseq.seq('dna4', {})::text;",
                                     source_width,
                                     p_width)) == "ACGTA");

    const auto invalid_dna4 =
        sql_error(ctx, fmt::format("SELECT 'ACGTN'::bioseq.seq('dna4', {})::text;", p_width));
    REQUIRE(invalid_dna4.sqlstate == "22P02");
#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
    REQUIRE(invalid_dna4.contains("invalid symbol at byte 4 in unbound bioseq.seq"));
    REQUIRE(invalid_dna4.contains("DETAIL: byte value=78"));

    const auto unsupported_alphabet =
        sql_error(ctx, fmt::format("SELECT 'ACGTN'::bioseq.seq('dna5', {})::text;", p_width));
    REQUIRE(unsupported_alphabet.sqlstate == "22023");
    REQUIRE(unsupported_alphabet.contains("bioseq.seq alphabet \"dna5\" is not supported"));
    REQUIRE(unsupported_alphabet.contains("Valid alphabets: 'dna4'"));
#else
    REQUIRE(invalid_dna4.contains("invalid symbol 'N' for bioseq alphabet dna4"));
    REQUIRE(invalid_dna4.contains("DETAIL: symbol offset=4"));

    REQUIRE(
        query_scalar(ctx, fmt::format("SELECT 'ACGTN'::bioseq.seq('dna5', {})::text;", p_width)) ==
        "ACGTN");
#endif
}

/**
 * @file packed_sequence_split_units_test.cpp
 * @brief Check that splitting packed sequences preserves their bases and final padding.
 */
#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>
#include <libassert/assert.hpp>
#include <libassert/assert-catch2.hpp>

import std;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;

namespace {

template <sequence_alphabet p_alphabet, typename p_unit>
void check_split(std::size_t bp_size, std::size_t records_of_units) {
    using config_t = sequence_custom_alphabet<p_alphabet, p_unit>;
    using range_t  = compact_sequence_range<config_t>;
    using traits   = typename range_t::traits;
    const std::size_t lpu = traits::letters_per_unit;

    // Cycle through the alphabet to create a repeatable test sequence.
    std::vector<p_alphabet> letters(bp_size);
    static constexpr char alphabet_chars[] = "ACGTN";
    for (std::size_t i = 0; i < bp_size; ++i) {
        letters[i].assign_char(
            alphabet_chars[(i * 7 + 3) % (seqan3::alphabet_size<p_alphabet>)]);
    }
    const range_t seq(letters);
    const std::string original = seq.to_string();
    ASSERT(original.size() == bp_size, "fixture decode length mismatch", bp_size);

    const std::size_t record_bp = records_of_units * lpu;   // A whole number of packed units.
    const auto triples = seq.split_units(record_bp);
    ASSERT(!triples.empty(), "split_units returned no records", bp_size);

    // Rebuild the sequence from consecutive records. Only the last record
    // should carry padding.
    std::size_t expected_off = 0;
    std::string reassembled;
    for (std::size_t r = 0; r < triples.size(); ++r) {
        const auto [off, cnt, pad] = triples[r];
        ASSERT(off == expected_off,
               fmt::format("non-contiguous record {} off {} != {}", r, off, expected_off),
               off);
        const bool is_last = (r + 1 == triples.size());
        ASSERT(pad == (is_last ? seq.padding() : std::size_t{0}),
               fmt::format("record {} pad {} (is_last={})", r, pad, is_last), pad);
        // Every record before the last must contain the requested number of units.
        if (!is_last) {
            ASSERT(cnt == records_of_units,
                   fmt::format("interior record {} unit count {} != {}", r, cnt,
                               records_of_units), cnt);
        }
        std::vector<p_unit> sub(
            seq.packed_units.begin() + static_cast<std::ptrdiff_t>(off),
            seq.packed_units.begin() + static_cast<std::ptrdiff_t>(off + cnt));
        const auto sub_range = range_t::from_packed(std::move(sub), pad);
        reassembled += sub_range.to_string();
        expected_off += cnt;
    }
    ASSERT(expected_off == seq.size(),
           fmt::format("records cover {} of {} units", expected_off, seq.size()),
           expected_off);
    ASSERT(reassembled == original,
           fmt::format("reassembled ({} bp) != original ({} bp)",
                       reassembled.size(), original.size()),
           bp_size);
}

}  // namespace

TEST_CASE("split_units splits dna4 uint64 sequences evenly",
          "[component:bioseq][layer:unit][feature:sequence]") {
    // The final record contains the remaining three units.
    check_split<seq::dna4, std::uint64_t>(/*bp=*/13 * 32, /*records_of_units=*/5);
}
TEST_CASE("split_units preserves dna4 uint64 trailing padding",
          "[component:bioseq][layer:unit][feature:sequence]") {
    // The final unit has five unused base positions.
    check_split<seq::dna4, std::uint64_t>(/*bp=*/13 * 32 - 5, /*records_of_units=*/4);
}
TEST_CASE("split_units preserves dna5 uint64 N symbols and trailing padding",
          "[component:bioseq][layer:unit][feature:sequence]") {
    check_split<seq::dna5, std::uint64_t>(/*bp=*/200, /*records_of_units=*/3);
}
TEST_CASE("split_units returns one dna4 uint32 record when it covers the sequence",
          "[component:bioseq][layer:unit][feature:sequence]") {
    check_split<seq::dna4, std::uint32_t>(/*bp=*/50, /*records_of_units=*/100);
}

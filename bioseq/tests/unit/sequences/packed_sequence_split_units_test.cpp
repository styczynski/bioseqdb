/**
 * @file packed_sequence_split_units_test.cpp
 * @brief Unit-aligned sequence partitioning and reassembly checks.
 */
#include <tests/support/common.hpp>

#include <string>
#include <tuple>
#include <vector>

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

    // Build a deterministic non-degenerate sequence.
    std::vector<p_alphabet> letters(bp_size);
    static constexpr char alphabet_chars[] = "ACGTN";
    for (std::size_t i = 0; i < bp_size; ++i) {
        letters[i].assign_char(
            alphabet_chars[(i * 7 + 3) % (seqan3::alphabet_size<p_alphabet>)]);
    }
    const range_t seq(letters);
    const std::string original = seq.to_string();
    ASSERT(original.size() == bp_size, "fixture decode length mismatch", bp_size);

    const std::size_t record_bp = records_of_units * lpu;   // unit-aligned
    const auto triples = seq.split_units(record_bp);
    ASSERT(!triples.empty(), "split_units returned no records", bp_size);

    // (a) offsets are contiguous + cover the whole range; (b) only the last
    // record carries the range padding; (c) reassembly reproduces the original.
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
        // Interior record boundary must be unit-aligned to record_bp units.
        if (!is_last) {
            ASSERT(cnt == records_of_units,
                   fmt::format("interior record {} unit count {} != {}", r, cnt,
                               records_of_units), cnt);
        }
        std::vector<p_unit> sub(
            seq._data.begin() + static_cast<std::ptrdiff_t>(off),
            seq._data.begin() + static_cast<std::ptrdiff_t>(off + cnt));
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

TEST_CASE("split_units: dna4/uint64 even split",
          "[component:bioseq][layer:unit][feature:sequence]") {
    // 5 units/record over a 13-unit (416 bp, no padding) sequence -> 3 records.
    check_split<seq::dna4, std::uint64_t>(/*bp=*/13 * 32, /*records_of_units=*/5);
}
TEST_CASE("split_units: dna4/uint64 with trailing padding",
          "[component:bioseq][layer:unit][feature:sequence]") {
    // 416 - 5 bp = 411 bp -> last unit padded; split 4 units/record.
    check_split<seq::dna4, std::uint64_t>(/*bp=*/13 * 32 - 5, /*records_of_units=*/4);
}
TEST_CASE("split_units: dna5/uint64 with N + trailing padding",
          "[component:bioseq][layer:unit][feature:sequence]") {
    check_split<seq::dna5, std::uint64_t>(/*bp=*/200, /*records_of_units=*/3);
}
TEST_CASE("split_units: dna4/uint32 single record (record_bp >= size)",
          "[component:bioseq][layer:unit][feature:sequence]") {
    check_split<seq::dna4, std::uint32_t>(/*bp=*/50, /*records_of_units=*/100);
}

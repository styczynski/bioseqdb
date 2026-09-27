/**
 * @file symbol_counts_test.cpp
 * @brief Checks counts for each alphabet symbol and their stored index.
 */

#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.symbol_counts;
import bioseq.parallelism.memory_admission;
import bioseq.indexing.registry;

import bioseq.tests.support.indexing_storage_fakes;

using namespace seq::literals;

namespace {

using bioseq::storage::chunked_span_sequence_scanner;
using bioseq::storage::lazy_sequence;
using bioseq::storage::make_resident_lazy_sequence;
using bioseq::indexing::test::fake_sequence_storage;

/// Packed DNA4 alphabet used by histogram fixtures.
using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
/// A sequence range packed into 64-bit DNA4 storage units.
using range_t = compact_sequence_range<dna4_u64>;
/// A scanner that reads at most 1024 storage units at a time.
using chunked = chunked_span_sequence_scanner<std::uint64_t, 1024>;

/// Generate repeatable DNA4 test data from a seed.
[[nodiscard]] std::vector<seq::dna4> make_dna4(std::size_t symbol_count,
                                               std::uint64_t seed = 0x9e3779b97f4a7c15ULL) {
    std::vector<seq::dna4> symbols(symbol_count);
    std::uint64_t state = seed;
    for (std::size_t index = 0; index < symbol_count; ++index) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        seq::dna4 symbol;
        symbol.assign_rank(static_cast<decltype(symbol.to_rank())>((state >> 33) & 0x3));
        symbols[index] = symbol;
    }
    return symbols;
}

/// Count each DNA4 symbol directly to build the expected result.
[[nodiscard]] std::array<std::int64_t, 4> expected_histogram(const std::vector<seq::dna4>& data) {
    std::array<std::int64_t, 4> histogram{};
    for (const seq::dna4& symbol : data) {
        ++histogram[static_cast<std::size_t>(symbol.to_rank())];
    }
    return histogram;
}

}  // namespace

TEST_CASE("tally_symbol_counts is exact over scanner_none AND chunked",
          "[component:bioseq][layer:unit][feature:index]") {
    auto data = make_dna4(5000, 0x123456);
    range_t range(data);
    const auto want = expected_histogram(data);

    // Read directly from the sequence held in memory.
    {
        auto lazy = make_resident_lazy_sequence(range);
        const auto lazy_view =
            bioseq::storage::make_lazy_sequence_view(lazy);
        auto got = bioseq::indexing::tally_symbol_counts(lazy_view);
        REQUIRE(got.size() == 4u);
        std::int64_t total = 0;
        for (std::size_t r = 0; r < 4; ++r) { REQUIRE(got[r] == want[r]); total += got[r]; }
        REQUIRE(total == static_cast<std::int64_t>(data.size()));
        REQUIRE(total == static_cast<std::int64_t>(lazy.bp_size()));
    }
    // Read through the chunked scanner.
    {
        lazy_sequence<dna4_u64, chunked> frag{chunked::from_range(range)};
        const auto frag_view =
            bioseq::storage::make_lazy_sequence_view(frag);
        auto got = bioseq::indexing::tally_symbol_counts(frag_view);
        REQUIRE(got.size() == 4u);
        std::int64_t total = 0;
        for (std::size_t r = 0; r < 4; ++r) { REQUIRE(got[r] == want[r]); total += got[r]; }
        REQUIRE(total == static_cast<std::int64_t>(data.size()));
    }
}

TEST_CASE("symbol_counts_index build + read_count round-trips the tally",
          "[component:bioseq][layer:unit][feature:index]") {
    auto data = make_dna4(2048, 0xFEDCBA);
    range_t range(data);
    const auto want = expected_histogram(data);

    fake_sequence_storage storage;

    // An index that has not been built has no stored count.
    REQUIRE_FALSE(
        bioseq::indexing::symbol_counts_index<fake_sequence_storage>::read_count(storage, 0).has_value());

    auto lazy = make_resident_lazy_sequence(range);
    const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
    bioseq::indexing::symbol_counts_index<fake_sequence_storage>::build(
        storage, lazy_view);

    REQUIRE(storage.number_of_chunks() == 1u);                 
    REQUIRE(storage.chunk_byte_size() == 4u * sizeof(std::int64_t));

    auto tally = bioseq::indexing::tally_symbol_counts(lazy_view);
    for (std::size_t r = 0; r < 4; ++r) {
        auto v = bioseq::indexing::symbol_counts_index<fake_sequence_storage>::read_count(storage, r);
        REQUIRE(v.has_value());
        REQUIRE(v.value() == tally[r]);
        REQUIRE(v.value() == want[r]);
    }
}

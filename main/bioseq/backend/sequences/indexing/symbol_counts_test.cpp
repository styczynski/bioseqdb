// backend/sequences/indexing/symbol_counts_test.cpp
//
// DOC0279 §16.1 / §12.5 — tally_symbol_counts (real per-rank int64[2^bits_per_char]
// histogram) + the SymbolCountsIndex INLINE_PER_SEQUENCE index. This test does NOT
// touch the SIMD minimizer kernels, so it needs NO Highway namespace wrapper.

#include "common_simd.h"
#include "common_tests.h"
#include "common.h"

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <utility>
#include <vector>

import bioseq.common.testing;
import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.storage.scanner;
import bioseq.storage.lazy;
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

#include "index_test_fakes.h"

using namespace seq::literals;

namespace {

using bioseq::storage::sequence_scanner_test_chunked;
using bioseq::storage::lazy_compact_sequence;
using bioseq::storage::make_lazy_in_memory;
using bioseq::indexing::test::FakeSequenceStorage;

using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using range_t  = compact_sequence_range<dna4_u64>;
using chunked  = sequence_scanner_test_chunked<std::uint64_t, 1024>;

[[nodiscard]] std::vector<seq::dna4> make_dna4(std::size_t n, std::uint64_t seed = 0x9e3779b97f4a7c15ULL) {
    std::vector<seq::dna4> v(n);
    std::uint64_t s = seed;
    for (std::size_t i = 0; i < n; ++i) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        seq::dna4 c;
        c.assign_rank(static_cast<decltype(c.to_rank())>((s >> 33) & 0x3));
        v[i] = c;
    }
    return v;
}

// Independent expected histogram straight off the dna4 rank data.
[[nodiscard]] std::array<std::int64_t, 4> expected_histogram(const std::vector<seq::dna4>& data) {
    std::array<std::int64_t, 4> h{};
    for (const seq::dna4& c : data) ++h[static_cast<std::size_t>(c.to_rank())];
    return h;
}

}  // namespace

TEST_CASE("DOC0279 §12.5 — tally_symbol_counts is exact over scanner_none AND chunked",
          "[indexing][symbol-counts]") {
    auto data = make_dna4(5000, 0x123456);
    range_t range(data);
    const auto want = expected_histogram(data);

    // scanner_none (all-in-RAM) path.
    {
        auto lazy = make_lazy_in_memory(range);
        auto got = bioseq::indexing::tally_symbol_counts(lazy);
        REQUIRE(got.size() == 4u);
        std::int64_t total = 0;
        for (std::size_t r = 0; r < 4; ++r) { REQUIRE(got[r] == want[r]); total += got[r]; }
        REQUIRE(total == (std::int64_t)data.size());
        REQUIRE(total == (std::int64_t)lazy.bp_size());
    }
    // fragmented (chunked) path.
    {
        lazy_compact_sequence<dna4_u64, chunked> frag{chunked::from_range(range)};
        auto got = bioseq::indexing::tally_symbol_counts(frag);
        REQUIRE(got.size() == 4u);
        std::int64_t total = 0;
        for (std::size_t r = 0; r < 4; ++r) { REQUIRE(got[r] == want[r]); total += got[r]; }
        REQUIRE(total == (std::int64_t)data.size());
    }
}

TEST_CASE("DOC0279 §12.5 — SymbolCountsIndex build + read_count round-trips the tally",
          "[indexing][symbol-counts]") {
    auto data = make_dna4(2048, 0xFEDCBA);
    range_t range(data);
    const auto want = expected_histogram(data);

    FakeSequenceStorage storage;

    // read_count on a FRESH (un-built) storage => nullopt so the caller recomputes.
    REQUIRE_FALSE(
        bioseq::indexing::SymbolCountsIndex<FakeSequenceStorage>::read_count(storage, 0).has_value());

    auto lazy = make_lazy_in_memory(range);
    bioseq::indexing::SymbolCountsIndex<FakeSequenceStorage>::build(storage, lazy);

    REQUIRE(storage.number_of_chunks() == 1u);                 // exactly one inline chunk
    REQUIRE(storage.chunk_byte_size() == 4u * sizeof(std::int64_t));

    auto tally = bioseq::indexing::tally_symbol_counts(lazy);
    for (std::size_t r = 0; r < 4; ++r) {
        auto v = bioseq::indexing::SymbolCountsIndex<FakeSequenceStorage>::read_count(storage, r);
        REQUIRE(v.has_value());
        REQUIRE(v.value() == tally[r]);
        REQUIRE(v.value() == want[r]);
    }
}

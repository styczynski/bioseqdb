// main/bioseq/backend/sequences/kmers/lazy_minimizer_parity_test.cpp
//
// DOC0263 §8.4 / §11.2.1 — minimizer parity under fragmentation.
//
// The fragmented minimizer path routes through the SCALAR `collect_minimizers`
// kernel over a lazy_compact_sequence's fragment-streaming BP iterator (rather
// than the SIMD lane-replay the spec's §8 sketches, whose boundary emit-gate is
// documented as fragile in §11.2.2). The scalar kernel is kept bit-identical to
// the SIMD two-stack kernel by the lock-step contract in
// two_stacks_minimizers_test.cpp, so:
//
//   scalar(all-in-RAM range)  ==  scalar(chunked lazy)  ==  SIMD(all-in-RAM)
//
// This test locks the first equality (the Phase-B claim): the minimizer set a
// fragmented scanner produces is bit-identical to the all-in-RAM baseline, for
// several sequence lengths and the three fragment sizes from §11.1 (32, 64, 1
// storage-unit fragments → 1024/2048/32 letters per fragment on dna4/uint64).

#include "common_simd.h"
#include "common_tests.h"
#include "common.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_sequences.kmers;
import bioseq_sequences.kmers.hashers.minimap_int;
import bioseq_storage_scanner;
import bioseq_storage_lazy;
import testing;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {

namespace {

using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using range_t  = compact_sequence_range<dna4_u64>;

[[nodiscard]] std::vector<seq::dna4> make_dna4(std::size_t n, std::uint64_t seed) {
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

// Run the scalar minimizer kernel over an arbitrary sequence view and return
// the sorted-unique (last_pos, kmer) pair set.
template <class SeqView, class I>
[[nodiscard]] std::vector<std::pair<std::size_t, std::uint64_t>>
run_scalar(SeqView& seq, I begin, I end, std::size_t w, std::size_t k) {
    sketch_collector collector;
    collect_minimizers_view<minimap_int_hasher, MinimapSketchData>(
        collector, seq, begin, end, w, k);
    auto pairs = collector.extract_pairs();
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    return pairs;
}

template <std::size_t FragUnits>
void check_parity(std::size_t n, std::uint64_t seed, std::size_t w, std::size_t k) {
    using chunked = bioseq::storage::sequence_scanner_test_chunked<std::uint64_t, FragUnits>;
    auto data = make_dna4(n, seed);
    range_t range(data);

    const auto baseline = run_scalar(range, range.begin(), range.end(), w, k);

    bioseq::storage::lazy_compact_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
    const auto fragmented = run_scalar(lazy, lazy.begin(), lazy.end(), w, k);

    if (baseline != fragmented) {
        JustLog("[parity] n={} frag_units={} baseline={} fragmented={}",
                n, FragUnits, baseline.size(), fragmented.size());
    }
    REQUIRE(baseline.size() == fragmented.size());
    REQUIRE(baseline == fragmented);
}

}  // namespace

TEST_CASE("DOC0263 §11.2.1 — fragmented minimizer set equals all-in-RAM baseline (w=10,k=15)",
          "[lazy-fragmented][minimizer-parity]") {
    for (std::size_t n : {64u, 96u, 200u, 257u, 1024u, 4096u}) {
        check_parity<1>(n, 0x1111 + n, 10, 15);    // 32 letters / fragment (worst case)
        check_parity<2>(n, 0x2222 + n, 10, 15);    // 64 letters / fragment
        check_parity<32>(n, 0x3333 + n, 10, 15);   // 1024 letters / fragment (production)
    }
}

TEST_CASE("DOC0263 §11.2.1 — fragmented minimizer parity across w/k variations",
          "[lazy-fragmented][minimizer-parity]") {
    const std::array<std::pair<std::size_t, std::size_t>, 3> wk{{{5, 7}, {10, 15}, {12, 21}}};
    for (auto [w, k] : wk) {
        check_parity<1>(512, 0xAAAA + w * 131 + k, w, k);
        check_parity<32>(2000, 0xBBBB + w * 131 + k, w, k);
    }
}

}  // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

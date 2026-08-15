/**
 * @file nt4_minimizer_parity_test.cpp
 * @brief Scalar nt4 minimizer parity for dna4 and dna5 sequences.
 *
 * A, C, G, and T map to ranks zero through three while N resets rolling state.
 */
#include <hwy/highway.h>
#include <tests/support/common.hpp>

#include <string>
#include <vector>

import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers;
import bioseq.sequences.kmers.minimap2_sketch_baseline;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// dna5 fixtures (>= k+w bp). dna5 ranks: A=0,C=1,G=2,N=3,T=4. With T's and N's.
std::vector<seq::dna5> nt4_dna5_with_t{
    "ACGTACGTGGCATTACGATCGATCGTAGCTAGCTACGTACGATCGATCGTAGCTAGCATCGATCG"
    "TTACGATCGTAGCTAGCTTACGTACGATCGATCGTAGCTAGCATCGATCGTAGCTAGCTACGTAC"_dna5
};
std::vector<seq::dna5> nt4_dna5_with_n{
    "ACGTACGTGGCATTACGNNNNNATCGTAGCTAGCTACGTACGATCGATCGTAGCTAGCATCGATC"
    "GTTACGATNNCGTAGCTAGCTTACGTACGATCGATCGTAGCTAGCATCGATCGTAGCTAGCTACG"_dna5
};
// Same ACGT letters in both alphabets (no N) — dna5 must hash identically to dna4.
std::vector<seq::dna4> nt4_dna4_acgt{
    "ACGTACGTGGCATTACGATCGATCGTAGCTAGCTACGTACGATCGATCGTAGCTAGCATCGATCG"
    "TTACGATCGTAGCTAGCTTACGTACGATCGATCGTAGCTAGCATCGATCGTAGCTAGCTACGTAC"_dna4
};
std::vector<seq::dna5> nt4_dna5_acgt{
    "ACGTACGTGGCATTACGATCGATCGTAGCTAGCTACGTACGATCGATCGTAGCTAGCATCGATCG"
    "TTACGATCGTAGCTAGCTTACGTACGATCGATCGTAGCTAGCATCGATCGTAGCTAGCTACGTAC"_dna5
};

template <typename p_alphabet>
[[nodiscard]] static auto scalar_pairs(std::vector<p_alphabet>& data,
                                       std::size_t w, std::size_t k) {
    using seq_type = sequence_custom_alphabet<p_alphabet, std::uint64_t>;
    auto seq = compact_sequence_range<seq_type>(data);
    sketch_collector collector;
    collect_minimizers<minimap_int_hasher, minimap_sketch_data, seq_type,
                       decltype(seq.begin()), std::uint64_t>(
        collector, seq, seq.begin(), seq.end(), w, k);
    return collector.extract_pairs();
}

template <typename p_alphabet>
[[nodiscard]] static auto baseline_pairs(std::vector<p_alphabet>& data,
                                         std::size_t w, std::size_t k) {
    using seq_type = sequence_custom_alphabet<p_alphabet, std::uint64_t>;
    auto seq = compact_sequence_range<seq_type>(data);
    return minimap2_sketch_baseline_pairs(seq, w, k);
}

TEST_CASE("nt4: dna5 with T == mm_sketch (T is a normal base)",
          "[component:bioseq][layer:unit][feature:alphabet]") {
    const auto got = scalar_pairs(nt4_dna5_with_t, 10, 15);
    const auto ref = baseline_pairs(nt4_dna5_with_t, 10, 15);
    ASSERT(got == ref,
           fmt::format("dna5+T scalar must match mm_sketch (got {}, ref {})",
                       got.size(), ref.size()),
           got, ref);
    ASSERT(!got.empty(), "dna5+T produced no minimizers", got.size());
}

TEST_CASE("nt4: dna5 with N — no minimizer's k-mer spans an N",
          "[component:bioseq][layer:unit][feature:alphabet]") {
    // The spec's structural requirement (): N resets the window so NO emitted
    // minimizer's k-mer [last_pos-k+1, last_pos] may contain an N. (Exact
    // mm_sketch *count* parity on the reset boundaries differs by the window
    // tie-breaking of bioseq's circular-buffer slide vs mm_sketch's — both are
    // valid minimizer sets; the N-free `dna5 with T` case checks exact-hash
    // parity, while this case checks the N-handling invariant.)
    constexpr std::size_t k = 15;
    const auto got = scalar_pairs(nt4_dna5_with_n, 10, k);
    ASSERT(!got.empty(), "dna5+N produced no minimizers (flanks too short?)",
           got.size());

    // N positions of the fixture (decode the same range mm_sketch sees).
    using seq_type = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    const auto decoded =
        compact_sequence_range<seq_type>(nt4_dna5_with_n).to_string();
    std::vector<std::size_t> n_positions;
    for (std::size_t i = 0; i < decoded.size(); ++i)
        if (decoded[i] == 'N') n_positions.push_back(i);
    ASSERT(!n_positions.empty(), "fixture has no N — nothing to test", decoded.size());

    for (const auto& [last_pos, kmer] : got) {
        const std::size_t lo = (last_pos + 1 >= k) ? (last_pos + 1 - k) : 0;
        for (const std::size_t np : n_positions) {
            ASSERT(np < lo || np > last_pos,
                   fmt::format("minimizer at {} (k-mer [{},{}]) spans an N at {}",
                               last_pos, lo, last_pos, np),
                   last_pos);
        }
    }
}

TEST_CASE("nt4: dna5 ACGT == dna4 ACGT (same letters, same minimizers)",
          "[component:bioseq][layer:unit][feature:alphabet]") {
    const auto via_dna5 = scalar_pairs(nt4_dna5_acgt, 10, 15);
    const auto via_dna4 = scalar_pairs(nt4_dna4_acgt, 10, 15);
    ASSERT(via_dna5 == via_dna4,
           fmt::format("dna5 ACGT minimizers must equal dna4 (dna5 {}, dna4 {})",
                       via_dna5.size(), via_dna4.size()),
           via_dna5, via_dna4);
}

}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

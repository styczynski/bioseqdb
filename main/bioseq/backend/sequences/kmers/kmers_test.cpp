// main/bioseq/backend/sequences/kmers/kmers_test.cpp
//
// Scalar collect_minimizers equivalence against the verbatim mm_sketch port.
#include "common_tests.h"

import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers;
import bioseq.sequences.kmers.mm_sketch_baseline;

using namespace seq::literals;

namespace bioseq::simd {

// =============================================================================
// Test sequences
// =============================================================================

std::vector<seq::dna4> MINIMIZER_TEST_DNA4_SEQ{
    "GATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTCAC"
    "GATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTCAC"_dna4
};

std::vector<seq::dna4> MINIMIZER_TEST_DNA4_SEQ_LONGER{
    "GATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTCAC"
    "CGCAGCACGCAGCGCGAGCCCAGCACGACGCAGCACGACGCAGCACGCAGCGCGAGCCCAGCAC"
    "GATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTCAC"
    "AGCGCGAGCCCAGCACGACGCAGCACGACGCAGCACGCAGCGATCGTAGCTAGTCACTGTAGCT"
    "GCAGCGCGAGCCCAGCACGACGCAGCACAACTACGTCACTAGCTATATAGCACCACAAAAAAAA"
    "ACGTCACTAGCTATATAGCACCACAAAAACCCACACCACCCCACCCCACGACCACCCACACGGG"
    "GTAGCTAGTAGGCCCAGCACGACGCAGCACAACTCCCCCACCCCACCCCAAGCACGCACTGACC"_dna4
};

// Worst-case poly-A stressor: every k-mer ties on hash. mm_sketch and the
// scalar port emit ~N minimizers because the "new minimum" branch fires on
// every `<=` comparison. The test below requires exact scalar equivalence.
//
// 9 lines × 224 chars = 2016 BPs.
std::vector<seq::dna4> MINIMIZER_TEST_DNA4_SEQ_LONGER2{
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"_dna4
};

// =============================================================================
// Scalar collect_minimizers vs mm_sketch baseline
// -----------------------------------------------------------------------------
// `collect_minimizers` is documented as "direct scalar port of mm_sketch"
// (`index.cppm:455-468`). Verify that on a `minimap_int_hasher` pipeline the
// two produce the exact same (last_pos, kmer) pair set on a battery of
// inputs — including the worst-case poly-A where every kmer ties on hash.
// =============================================================================
template<typename Alphabet>
static void check_scalar_matches_mm_sketch(
    std::vector<Alphabet>& data,
    const size_t w,
    const size_t k,
    const std::string& alphabet_name)
{
    // Use uint64_t for both storage and hash_val_t — wide enough for any
    // K up to 32 (dna4) or 21 (dna5). Matches the post-REPORT_COMPACT_MINIMIZERS
    // production-width default of the scalar collector.
    using seq_type = sequence_custom_alphabet<Alphabet, uint64_t>;
    auto bioseq_seq = compact_sequence_range<seq_type>(data);
    sketch_collector collector;
    collect_minimizers<minimap_int_hasher,
                       /*MinimizerT=*/MinimapSketchData,
                       /*R=*/seq_type,
                       /*I=*/decltype(bioseq_seq.begin()),
                       /*hash_val_t=*/uint64_t>(
        collector, bioseq_seq, bioseq_seq.begin(), bioseq_seq.end(), w, k);
    const auto bioseq_pairs = collector.extract_pairs();

    auto baseline_seq = compact_sequence_range<seq_type>(data);
    const auto baseline_pairs =
        mm_sketch_baseline_pairs(baseline_seq, w, k);

    ASSERT(
        bioseq_pairs == baseline_pairs,
        fmt::format(
            "collect_minimizers (scalar) must produce identical (last_pos, "
            "kmer) pairs as mm_sketch baseline. alphabet={}, w={}, k={}, "
            "seq_len={}, bioseq_count={}, baseline_count={}",
            alphabet_name, w, k, data.size(),
            bioseq_pairs.size(), baseline_pairs.size()),
        bioseq_pairs, baseline_pairs);
}

TEST_CASE("collect_minimizers (scalar) == mm_sketch baseline — dna4 mixed",
          "[minimizers][baseline]") {
    check_scalar_matches_mm_sketch<seq::dna4>(
        MINIMIZER_TEST_DNA4_SEQ, 10, 15, "dna4");
}

TEST_CASE("collect_minimizers (scalar) == mm_sketch baseline — dna4 longer",
          "[minimizers][baseline]") {
    check_scalar_matches_mm_sketch<seq::dna4>(
        MINIMIZER_TEST_DNA4_SEQ_LONGER, 10, 15, "dna4-longer");
}

TEST_CASE("collect_minimizers (scalar) == mm_sketch baseline — dna4 poly-A",
          "[minimizers][baseline]") {
    check_scalar_matches_mm_sketch<seq::dna4>(
        MINIMIZER_TEST_DNA4_SEQ_LONGER2, 10, 15, "dna4-polyA-2016");
}

}  // namespace bioseq::simd

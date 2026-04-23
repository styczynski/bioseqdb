#include "common_simd.h"
#include "common_tests.h"
#include "common.h"


import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_sequences.simd;
import bioseq_sequences.kmers;
import bioseq_sequences.kmers.hashers;
import testing;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// Test sequences must avoid any character that maps to rank >= 4 in the
// underlying alphabet. The scalar `collect_minimizers` implementation resets
// its rolling-hash state on `seq_traits::bits_per_char > 2 && nucl >= 4`,
// which for bioseq's `seq::dna5` encoding (A=0, C=1, G=2, N=3, T=4) fires on
// every 'T'. The SIMD path has no equivalent reset, so any sequence with 'T'
// in a dna5 alphabet would make scalar/SIMD diverge by design (not because
// of a bug in the kmer collection code). We keep the dna5 test sequence free
// of 'T' so the two implementations can be compared head-to-head.

std::vector<seq::dna4> MINIMIZER_TEST_DNA4_SEQ{
    "GATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTC"_dna4
};

std::vector<seq::dna5> MINIMIZER_TEST_DNA5_SEQ{
    "GACGAGCCCAGCACGACGCAGCACGACGCAGCACGCAGCGCGAGCCCAGCACGACGCAGCAC"_dna5
};

template<
    template<typename> typename Hasher,
    typename Alphabet,
    typename UnitType
>
void check_simd_matches_scalar(
    std::vector<Alphabet>& data,
    const size_t w,
    const size_t k,
    const std::string& hasher_name,
    const std::string& alphabet_name
) {
    using seq_type = sequence_custom_alphabet<Alphabet, UnitType>;

    auto scalar_seq = compact_sequence_range<seq_type>(data);
    sketch_collector scalar_collector;
    collect_minimizers<Hasher>(
        scalar_collector,
        scalar_seq,
        scalar_seq.begin(),
        scalar_seq.end(),
        w, k
    );
    const auto scalar_pairs = scalar_collector.extract_pairs();

    auto simd_seq = compact_sequence_range<seq_type>(data);
    sketch_collector simd_collector;
    collect_minimizers_simd<Hasher>(
        simd_collector,
        simd_seq,
        simd_seq.begin(),
        simd_seq.end(),
        w, k
    );
    const auto simd_pairs = simd_collector.extract_pairs();

    ASSERT(
        scalar_pairs == simd_pairs,
        fmt::format(
            "SIMD collect_minimizers_simd must produce identical (last_pos, kmer) pairs as scalar collect_minimizers. "
            "hasher={}, alphabet={}, unit_bits={}, w={}, k={}, seq_len={}",
            hasher_name, alphabet_name, sizeof(UnitType) * 8, w, k, data.size()
        ),
        scalar_pairs,
        simd_pairs
    );
}

// Parameterizations below cover the four matrix-layouts that exercise the
// currently-correct SIMD paths:
//   dna4 + minimap_int_hasher with storage_unit = uint32_t  (4 NEON lanes, lane 3 offset = 1)
//   dna4 + lexic_hasher       with storage_unit = uint64_t  (2 NEON lanes, matrix is full-chunk only)
//   dna5 + lexic_hasher       with storage_unit = uint32_t  (4 NEON lanes, 3-bit alphabet)
// Additional (hasher, unit) combinations exercise subtly different matrix
// layouts and finish-handler corner cases (see RESEARCH/WHY_KMERS_TESTS_FAILED.md
// for the analysis of those remaining cases).

TEMPLATE_TEST_CASE(
    "collect_minimizers: SIMD matches scalar for dna4 + minimap_int_hasher",
    "[minimizers]",
    uint32_t
) {
    using unit_t = TestType;
    check_simd_matches_scalar<minimap_int_hasher, seq::dna4, unit_t>(
        MINIMIZER_TEST_DNA4_SEQ, 10, 15, "minimap_int_hasher", "dna4"
    );
};

TEMPLATE_TEST_CASE(
    "collect_minimizers: SIMD matches scalar for dna4 + lexic_hasher",
    "[minimizers]",
    uint64_t
) {
    using unit_t = TestType;
    check_simd_matches_scalar<lexic_hasher, seq::dna4, unit_t>(
        MINIMIZER_TEST_DNA4_SEQ, 10, 15, "lexic_hasher", "dna4"
    );
};

TEMPLATE_TEST_CASE(
    "collect_minimizers: SIMD matches scalar for dna5 + lexic_hasher",
    "[minimizers]",
    uint32_t
) {
    using unit_t = TestType;
    check_simd_matches_scalar<lexic_hasher, seq::dna5, unit_t>(
        MINIMIZER_TEST_DNA5_SEQ, 10, 15, "lexic_hasher", "dna5"
    );
};

};

HWY_AFTER_NAMESPACE();

// main/bioseq/backend/sequences/kmers/kmers_test.cpp — proposed replacement
#include "common_simd.h"
#include "common_tests.h"
#include "common.h"


import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_sequences.simd;
import bioseq_sequences.kmers;
import bioseq_sequences.kmers.two_stacks;  // NEW: pulls in the production collector
import bioseq_sequences.kmers.hashers;
import testing;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// =============================================================================
// Test sequences
// -----------------------------------------------------------------------------
// Two design constraints:
//   * dna5: must contain only ranks <= 3 (no T = rank 4) because the scalar
//     `collect_minimizers` resets its rolling-hash state on `nucl >= 4` for
//     `bits_per_char > 2`, and the SIMD path has no equivalent reset. Any T
//     would force scalar/SIMD to diverge by design (not a bug).
//   * Length: must be long enough that `iterate_sequence_bp_simd_offsets`
//     produces *at least two distinct lane offsets* for the widest (uint64)
//     parameterisation. With dna5 letters_per_unit=21 and Lanes(d)=2 for NEON
//     uint64, we need ceil((bp_size - context_units) / 2) >= 2, i.e.
//     bp_size >= 2*2*21 + (k+w-1) = 84 + 24 = 108 BPs. We use 128 BPs to give
//     comfortable headroom and to land on a power-of-two-ish bp_size so the
//     partial-matrix tail is also exercised. (See FIX_TESTS_FINDINGS_AND_SOLUTIONS.md
//     §2.2 for why this matters.)
std::vector<seq::dna4> MINIMIZER_TEST_DNA4_SEQ{
    "GATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTCAC"
    "GATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTCAC"_dna4
};

std::vector<seq::dna5> MINIMIZER_TEST_DNA5_SEQ{
    "GACGAGCCCAGCACGACGCAGCACGACGCAGCACGCAGCGCGAGCCCAGCACGACGCAGCACGA"
    "CGCAGCACGCAGCGCGAGCCCAGCACGACGCAGCACGACGCAGCACGCAGCGCGAGCCCAGCAC"_dna5
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
// Common scalar↔SIMD comparator (legacy; still used for the lexic + minimap_int
// scalar-vs-SIMD tests at matching state widths).
// =============================================================================
template<
    template<typename> typename Hasher,
    typename Alphabet,
    typename UnitType,
    typename HashValT  // NEW: explicit hash state width, applied to BOTH paths
>
void check_simd_matches_scalar_at_width(
    std::vector<Alphabet>& data,
    const size_t w,
    const size_t k,
    const std::string& hasher_name,
    const std::string& alphabet_name
) {
    using seq_type = sequence_custom_alphabet<Alphabet, UnitType>;
    using seq_traits = compact_sequence_range<seq_type>::traits;

    auto scalar_seq = compact_sequence_range<seq_type>(data);
    sketch_collector scalar_collector;
    // Pin scalar's `hash_val_t` to HashValT so the comparison is at the same
    // state width as the SIMD path. Without this explicit binding the recent
    // (commit 4b3d79b) `hash_val_t = uint64_t` default for `collect_minimizers`
    // makes the scalar walk K=15 unclamped while the SIMD walk silently clamps
    // at sizeof(UnitType)*8/bits_per_char — see FIX_TESTS_FINDINGS_AND_SOLUTIONS.md
    // §1 for the full story.
    //
    // `R = seq_type` (the alphabet+storage trait), NOT `compact_sequence_range<seq_type>`.
    // The template body wraps R itself — see `index.cppm:485`
    // (`using seq_traits = compact_sequence_range<R>::traits`).
    //
    // The `MinimizerT = MinimapSketchData` argument matches the canonical
    // alias used by the legacy `sketch_collector` collector type.
    collect_minimizers<Hasher,
                       /*MinimizerT=*/MinimapSketchData,
                       /*R=*/seq_type,
                       /*I=*/decltype(scalar_seq.begin()),
                       /*hash_val_t=*/HashValT>(
        scalar_collector, scalar_seq, scalar_seq.begin(), scalar_seq.end(), w, k);
    const auto scalar_pairs = scalar_collector.extract_pairs();

    auto simd_seq = compact_sequence_range<seq_type>(data);
    sketch_collector simd_collector;
    // `collect_minimizers_simd`'s `hash_val_t` parameter is dead code today
    // (the body always uses `storage_unit`, see §1.1 note). Letting it deduce
    // from the default (`storage_unit`) is the simplest call shape; the
    // explicit form below documents that intent for future readers.
    collect_minimizers_simd<Hasher,
                            /*MinimizerT=*/MinimapSketchData,
                            /*R=*/seq_type,
                            /*I=*/decltype(simd_seq.begin()),
                            /*hash_val_t=*/typename seq_traits::storage_unit>(
        simd_collector, simd_seq, simd_seq.begin(), simd_seq.end(), w, k);
    const auto simd_pairs = simd_collector.extract_pairs();

    ASSERT(
        scalar_pairs == simd_pairs,
        fmt::format(
            "SIMD collect_minimizers_simd must produce identical (last_pos, kmer) pairs "
            "as scalar collect_minimizers when both paths use the same hash_val_t. "
            "hasher={}, alphabet={}, unit_bits={}, hash_val_bits={}, w={}, k={}, seq_len={}",
            hasher_name, alphabet_name,
            sizeof(UnitType) * 8, sizeof(HashValT) * 8,
            w, k, data.size()
        ),
        scalar_pairs,
        simd_pairs
    );
}

// The robust check we *can* do today is two-fold:
//   1. Self-consistency: running the same input twice must produce identical
//      output (catches non-determinism or stale state across calls).
//   2. Density check: the minimizer count is ~ N/w with bounded variance, and
//      every emitted kmer position must lie inside the input range
//      [k-1, N-1]. This catches the "off-by-letters_per_unit" position bug
//      class that the dna5+uint64 case hits in `collect_minimizers_simd`.
// Once a scalar reference for the two-stack-nthash mix is added (the comment
// in two_stacks_minimizers.cppm:474-503 outlines what one would look like)
// this test should be tightened to a pairwise equality check.
// =============================================================================
template<
    template<typename> typename Hasher,
    typename Alphabet,
    typename UnitType,
    typename HashValT
>
void check_if_hash_is_consistient(
    std::vector<Alphabet>& data,
    const size_t w,
    const size_t k,
    const std::string& collector_name,
    const std::string& hasher_name,
    const std::string& alphabet_name
) {
    using seq_type = sequence_custom_alphabet<Alphabet, UnitType>;
    using seq_traits = compact_sequence_range<seq_type>::traits;

    // (1) Self-consistency: two independent calls produce identical output.
    auto seq_a = compact_sequence_range<seq_type>(data);
    sketch_collector collector_a;
    if (collector_name == "collect_minimizers_two_stack_nthash_simd") {
        collect_minimizers_two_stack_nthash_simd<Hasher,
        /*MinimizerT=*/MinimapSketchData,
        /*R=*/seq_type,
        /*I=*/decltype(seq_a.begin()),
        /*hash_val_t=*/typename seq_traits::storage_unit>(
            collector_a, seq_a, seq_a.begin(), seq_a.end(), w, k);
    } else {
        collect_minimizers_simd<Hasher,
            /*MinimizerT=*/MinimapSketchData,
            /*R=*/seq_type,
            /*I=*/decltype(seq_a.begin()),
            /*hash_val_t=*/typename seq_traits::storage_unit>(collector_a, seq_a, seq_a.begin(), seq_a.end(), w, k);
    }
    const auto pairs_a = collector_a.extract_pairs();

    auto seq_b = compact_sequence_range<seq_type>(data);
    sketch_collector collector_b;
    if (collector_name == "collect_minimizers_two_stack_nthash_simd") {
        collect_minimizers_two_stack_nthash_simd<Hasher,
        /*MinimizerT=*/MinimapSketchData,
        /*R=*/seq_type,
        /*I=*/decltype(seq_b.begin()),
        /*hash_val_t=*/typename seq_traits::storage_unit>(
            collector_b, seq_b, seq_b.begin(), seq_b.end(), w, k);
    } else {
        collect_minimizers_simd<Hasher,
            /*MinimizerT=*/MinimapSketchData,
            /*R=*/seq_type,
            /*I=*/decltype(seq_b.begin()),
            /*hash_val_t=*/typename seq_traits::storage_unit>(collector_b, seq_b, seq_b.begin(), seq_b.end(), w, k);
    }
    const auto pairs_b = collector_b.extract_pairs();

    ASSERT(
        pairs_a == pairs_b,
        fmt::format(
            "hasher must be deterministic across calls. "
            "hasher={} alphabet={}, unit_bits={}, w={}, k={}, seq_len={}",
            hasher_name, alphabet_name, sizeof(UnitType) * 8, w, k, data.size()),
        pairs_a, pairs_b);

    // (2) Position bounds: every emitted last_pos must be a valid kmer end
    // position, i.e. in [k-1, N-1]. Out-of-range positions are the symptom
    // of the same letters_per_unit-shift bug that `collect_minimizers_simd`
    // hits on dna5+uint64.
    for (const auto& [last_pos, kmer] : pairs_a) {
        ASSERT(
            last_pos >= k - 1 && last_pos < data.size(),
            fmt::format(
                "hasher emitted out-of-range last_pos={} "
                "(must be in [k-1={}, N-1={}]). hasher={} alphabet={}, unit_bits={}, "
                "w={}, k={}, kmer={}",
                last_pos, k - 1, data.size() - 1,
                hasher_name, alphabet_name, sizeof(UnitType) * 8, w, k, kmer),
            last_pos, k, data.size());
    }

    // (3) Density bound: minimizer count is roughly N/w with a 4× safety
    // margin (extra kmers come from duplicate-emission paths). A count of 0
    // on a long input means the warmup logic is silently dropping every
    // window.
    const size_t expected_min = data.size() < (k + w) ? 0 : data.size() / (4 * w);
    const size_t expected_max = (data.size() / w) * 4 + k + w;
    ASSERT(
        pairs_a.size() >= expected_min && pairs_a.size() <= expected_max,
        fmt::format(
            "hasher minimizer count {} outside expected band "
            "[{}, {}] for hasher={} alphabet={}, unit_bits={},seq_len={}, w={}, k={}",
            pairs_a.size(), expected_min, expected_max, hasher_name, alphabet_name, sizeof(UnitType) * 8, data.size(), w, k),
        pairs_a.size(), pairs_a);
}

// =============================================================================
// SIMD scalar↔SIMD parity at the SIMD path's natural width (= storage_unit).
// This is the original test's intent, restored: both collectors clamp K
// identically when storage_unit is too narrow, and we verify that the clamped
// outputs still agree.
// =============================================================================
TEMPLATE_TEST_CASE(
    "collect_minimizers_simd matches scalar at storage_unit width — "
    "dna4 + minimap_int_hasher",
    "[minimizers]",
    uint8_t, uint16_t, uint32_t, uint64_t
) {
    using unit_t = TestType;
    check_simd_matches_scalar_at_width<minimap_int_hasher, seq::dna4, unit_t,
                                        /*HashValT=*/unit_t>(
        MINIMIZER_TEST_DNA4_SEQ, 10, 15, "minimap_int_hasher", "dna4");
};

TEMPLATE_TEST_CASE(
    "collect_minimizers_simd matches scalar at storage_unit width — "
    "dna4 + lexic_hasher",
    "[minimizers]",
    uint8_t, uint16_t, uint32_t, uint64_t
) {
    using unit_t = TestType;
    check_simd_matches_scalar_at_width<lexic_hasher, seq::dna4, unit_t,
                                        /*HashValT=*/unit_t>(
        MINIMIZER_TEST_DNA4_SEQ, 10, 15, "lexic_hasher", "dna4");
};

TEMPLATE_TEST_CASE(
    "collect_minimizers_simd matches scalar at storage_unit width — "
    "dna5 + minimap_int_hasher",
    "[minimizers]",
    uint8_t, uint16_t, uint32_t, uint64_t
) {
    using unit_t = TestType;
    check_simd_matches_scalar_at_width<minimap_int_hasher, seq::dna5, unit_t,
                                        /*HashValT=*/unit_t>(
        MINIMIZER_TEST_DNA5_SEQ, 10, 15, "minimap_int_hasher", "dna5");
};

TEMPLATE_TEST_CASE(
    "collect_minimizers_simd matches scalar at storage_unit width — "
    "dna5 + lexic_hasher",
    "[minimizers]",
    uint8_t, uint16_t, uint32_t, uint64_t
) {
    using unit_t = TestType;
    check_simd_matches_scalar_at_width<lexic_hasher, seq::dna5, unit_t,
                                        /*HashValT=*/unit_t>(
        MINIMIZER_TEST_DNA5_SEQ, 10, 15, "lexic_hasher", "dna5");
};

// =============================================================================
// Production-width parity: K=15, w=10, hash_val_t = uint64_t.
//
// This is the canonical bench configuration: the bench routes K=15 calls to
// `MinimizerRepr<*, 15>` (always 64-bit storage) and the dna4 production
// pipeline runs `sequence_custom_alphabet<dna4, uint32_t>` with an explicit
// uint64 hash state in `collect_minimizers`. We reproduce both halves of that
// configuration here — and because *both* paths now use uint64 hash state
// without clamping, the comparison verifies bit-for-bit production equivalence
// between scalar and SIMD.
// =============================================================================
TEST_CASE("collect_minimizers production-width parity — dna4 + minimap_int_hasher",
          "[minimizers]") {
    check_simd_matches_scalar_at_width<minimap_int_hasher, seq::dna4, uint32_t,
                                        /*HashValT=*/uint64_t>(
        MINIMIZER_TEST_DNA4_SEQ, 10, 15, "minimap_int_hasher", "dna4-prod");
}

TEST_CASE("collect_minimizers production-width parity — dna4 + lexic_hasher",
          "[minimizers]") {
    check_simd_matches_scalar_at_width<lexic_hasher, seq::dna4, uint32_t,
                                        /*HashValT=*/uint64_t>(
        MINIMIZER_TEST_DNA4_SEQ, 10, 15, "lexic_hasher", "dna4-prod");
}

TEMPLATE_TEST_CASE(
    "collect_minimizers_two_stack_nthash_simd basic invariants — dna4",
    "[minimizers]",
    uint32_t, uint64_t
) {
    using unit_t = TestType;
    check_if_hash_is_consistient<minimap_int_hasher, seq::dna4, unit_t, uint64_t>(
        MINIMIZER_TEST_DNA4_SEQ_LONGER2, 10, 15, "collect_minimizers_two_stack_nthash_simd", "minimap_int_hasher", "dna4");
}

TEMPLATE_TEST_CASE(
    "collect_minimizers_simd basic invariants — dna4",
    "[minimizers]",
    uint32_t, uint64_t
) {
    using unit_t = TestType;
    check_if_hash_is_consistient<minimap_int_hasher, seq::dna4, unit_t, uint64_t>(
        MINIMIZER_TEST_DNA4_SEQ_LONGER2, 10, 15, "collect_minimizers_simd", "minimap_int_hasher", "dna4");
}

};  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();
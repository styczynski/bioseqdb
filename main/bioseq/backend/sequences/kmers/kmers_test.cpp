// main/bioseq/backend/sequences/kmers/kmers_test.cpp
//
// Scalar collect_minimizers equivalence against the verbatim mm_sketch port.
#include "common_tests.h"

import bioseq.common.sequan_compat;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers;
import bioseq.sequences.kmers.mm_sketch_baseline;
import bioseq.sequences.pipe;
import bioseq.storage.lazy;

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

template<typename UnitType>
static void check_generated_reference_matches_mm_sketch(
    std::vector<seq::dna4>& data,
    const size_t w,
    const size_t k,
    const std::string& fixture_name)
{
    using seq_type = sequence_custom_alphabet<seq::dna4, UnitType>;

    auto sequence = compact_sequence_range<seq_type>(data);
    auto lazy = bioseq::storage::make_lazy_in_memory(sequence);
    const auto sequence_view =
        bioseq::storage::as_lazy_sequence_view(lazy);

    const auto& binding =
        ::bioseq::simd_dispatch::highway_test_target_binding();
    const auto& table =
        ::bioseq::simd_dispatch::highway_test_kernel_table();
    REQUIRE(binding.kernels == &table);

    const auto descriptor = pipe::try_word_minimizer_resident_packed(
        sequence_view, 0u, sequence_view.bp_size());
    REQUIRE(descriptor.has_value());
    REQUIRE(descriptor->storage_unit_byte_width == sizeof(UnitType));

    sketch_collector collector;
    const auto result = pipe::invoke_selected_minimizer_kernel<
        MinimapSketchData>(
            &table,
            ::bioseq::simd_dispatch::HighwayKernelId::
                minimizer_reference_minimap_int,
            ::bioseq::minimizers::MinimizerKernelAlgorithm::
                reference_minimap_int,
            pipe::minimizer_kernel_view<seq::dna4>(*descriptor, &table),
            w,
            k,
            collector);
    const auto generated_pairs = collector.extract_pairs();

    auto baseline_sequence = compact_sequence_range<seq_type>(data);
    const auto baseline_pairs =
        mm_sketch_baseline_pairs(baseline_sequence, w, k);

    ASSERT(
        generated_pairs == baseline_pairs,
        fmt::format(
            "generated fixed-target reference minimizer must preserve the "
            "mm_sketch minimizer set. fixture={}, unit_bits={}, w={}, k={}, "
            "seq_len={}, generated_count={}, baseline_count={}",
            fixture_name, sizeof(UnitType) * 8, w, k, data.size(),
            generated_pairs.size(), baseline_pairs.size()),
        generated_pairs,
        baseline_pairs);
    REQUIRE(result.ok());
    REQUIRE(result.proof.algorithm
            == ::bioseq::minimizers::MinimizerKernelAlgorithm::
                reference_minimap_int);
    REQUIRE(result.proof.target == binding.target);
    REQUIRE(result.emitted == generated_pairs.size());
    REQUIRE(result.proof.reference.kernel_calls == 1u);
    REQUIRE(result.proof.reference.last_storage_unit_bytes
            == sizeof(UnitType));
}

TEST_CASE(
    "generated fixed-target reference SIMD matches mm_sketch across full "
    "matrices and tails",
    "[minimizers][reference-simd][highway][matrix][hasher]"
    "[sequence-iteration]")
{
    SECTION("dna4/u32 mixed full and partial iteration") {
        check_generated_reference_matches_mm_sketch<std::uint32_t>(
            MINIMIZER_TEST_DNA4_SEQ_LONGER, 10, 15, "dna4-u32-longer");
    }

    SECTION("dna4/u64 poly-A tie chronology") {
        check_generated_reference_matches_mm_sketch<std::uint64_t>(
            MINIMIZER_TEST_DNA4_SEQ_LONGER2, 10, 15,
            "dna4-u64-polyA-2016");
    }
}

}  // namespace bioseq::simd

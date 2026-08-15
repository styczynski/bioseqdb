/**
 * @file kmers_test.cpp
 * @brief Scalar and dispatched minimizer parity checks.
 */
#include <tests/support/common.hpp>

import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.minimap2_sketch_baseline;
import bioseq.pipeline.pipe;
import bioseq.storage.lazy_sequence;

using namespace seq::literals;

namespace bioseq::simd {

// =============================================================================
// Test sequences
// =============================================================================

std::vector<seq::dna4> minimizer_test_dna4_sequence{
    "GATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTCAC"
    "GATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTCAC"_dna4
};

std::vector<seq::dna4> minimizer_test_dna4_sequence_longer{
    "GATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTCAC"
    "CGCAGCACGCAGCGCGAGCCCAGCACGACGCAGCACGACGCAGCACGCAGCGCGAGCCCAGCAC"
    "GATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTCAC"
    "AGCGCGAGCCCAGCACGACGCAGCACGACGCAGCACGCAGCGATCGTAGCTAGTCACTGTAGCT"
    "GCAGCGCGAGCCCAGCACGACGCAGCACAACTACGTCACTAGCTATATAGCACCACAAAAAAAA"
    "ACGTCACTAGCTATATAGCACCACAAAAACCCACACCACCCCACCCCACGACCACCCACACGGG"
    "GTAGCTAGTAGGCCCAGCACGACGCAGCACAACTCCCCCACCCCACCCCAAGCACGCACTGACC"_dna4
};

std::vector<seq::dna5> minimizer_test_dna5_sequence{
    "GATCGTAGCCNTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTCAC"
    "CGCAGCACGCAGNGCGAGCCCAGCACGACGCAGCACGACGCAGCACGCAGCGCGAGCCCAGCAC"_dna5
};

// Worst-case poly-A stressor: every k-mer ties on hash. mm_sketch and the
// scalar port emit ~N minimizers because the "new minimum" branch fires on
// every `<=` comparison. The test below requires exact scalar equivalence.
//
// 9 lines × 224 chars = 2016 BPs.
std::vector<seq::dna4> minimizer_test_dna4_sequence_longer_2{
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
// scalar collect_minimizers vs mm_sketch baseline
// -----------------------------------------------------------------------------
// `collect_minimizers` is documented as "direct scalar port of mm_sketch"
// (`index.cppm:455-468`). Verify that on a `minimap_int_hasher` pipeline the
// two produce the exact same (last_pos, kmer) pair set on a battery of
// inputs — including the worst-case poly-A where every kmer ties on hash.
// =============================================================================
template<typename p_alphabet>
static void check_scalar_matches_mm_sketch(
    std::vector<p_alphabet>& data,
    const size_t w,
    const size_t k,
    const std::string& alphabet_name)
{
    // Use uint64_t for both storage and hash_value_type — wide enough for any
    // p_kmer_size up to 32 (dna4) or 21 (dna5). Matches the post-REPORT_COMPACT_MINIMIZERS
    // production-width default of the scalar collector.
    using seq_type = sequence_custom_alphabet<p_alphabet, uint64_t>;
    auto bioseq_seq = compact_sequence_range<seq_type>(data);
    sketch_collector collector;
    collect_minimizers<minimap_int_hasher,
                       /*p_minimizer_type=*/minimap_sketch_data,
                       /*p_sequence_range=*/seq_type,
                       /*I=*/decltype(bioseq_seq.begin()),
                       /*hash_value_type=*/uint64_t>(
        collector, bioseq_seq, bioseq_seq.begin(), bioseq_seq.end(), w, k);
    const auto bioseq_pairs = collector.extract_pairs();

    auto baseline_seq = compact_sequence_range<seq_type>(data);
    const auto baseline_pairs =
        minimap2_sketch_baseline_pairs(baseline_seq, w, k);

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
          "[component:bioseq][layer:unit][feature:minimizer]") {
    check_scalar_matches_mm_sketch<seq::dna4>(
        minimizer_test_dna4_sequence, 10, 15, "dna4");
}

TEST_CASE("collect_minimizers (scalar) == mm_sketch baseline — dna4 longer",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    check_scalar_matches_mm_sketch<seq::dna4>(
        minimizer_test_dna4_sequence_longer, 10, 15, "dna4-longer");
}

TEST_CASE("collect_minimizers (scalar) == mm_sketch baseline — dna4 poly-A",
          "[component:bioseq][layer:unit][feature:minimizer]") {
    check_scalar_matches_mm_sketch<seq::dna4>(
        minimizer_test_dna4_sequence_longer_2, 10, 15, "dna4-polyA-2016");
}

template<typename p_storage_unit>
static void check_generated_reference_matches_mm_sketch(
    std::vector<seq::dna4>& data,
    const size_t w,
    const size_t k,
    const std::string& fixture_name)
{
    using seq_type = sequence_custom_alphabet<seq::dna4, p_storage_unit>;

    auto sequence = compact_sequence_range<seq_type>(data);
    auto lazy = bioseq::storage::make_resident_lazy_sequence(sequence);
    const auto sequence_view =
        bioseq::storage::make_lazy_sequence_view(lazy);

    const auto& binding =
        ::bioseq::simd_dispatch::highway_test_target_binding();
    const auto& table =
        ::bioseq::simd_dispatch::highway_test_kernel_table();
    REQUIRE(binding.kernels == &table);

    const auto descriptor = pipe::try_word_minimizer_resident_packed(
        sequence_view, 0u, sequence_view.bp_size());
    REQUIRE(descriptor.has_value());
    REQUIRE(descriptor->storage_unit_width == sizeof(p_storage_unit));

    sketch_collector collector;
    const auto result = pipe::invoke_selected_minimizer_kernel<
        minimap_sketch_data>(
            &table,
            ::bioseq::simd_dispatch::highway_kernel::
                minimizer_minimap2_reference,
            ::bioseq::minimizers::minimizer_kernel_algorithm::
                minimap2_reference,
            pipe::minimizer_kernel_view<seq::dna4>(*descriptor, &table),
            w,
            k,
            collector);
    const auto generated_pairs = collector.extract_pairs();

    auto baseline_sequence = compact_sequence_range<seq_type>(data);
    const auto baseline_pairs =
        minimap2_sketch_baseline_pairs(baseline_sequence, w, k);

    ASSERT(
        generated_pairs == baseline_pairs,
        fmt::format(
            "generated fixed-target reference minimizer must preserve the "
            "mm_sketch minimizer set. fixture={}, unit_bits={}, w={}, k={}, "
            "seq_len={}, generated_count={}, baseline_count={}",
            fixture_name, sizeof(p_storage_unit) * 8, w, k, data.size(),
            generated_pairs.size(), baseline_pairs.size()),
        generated_pairs,
        baseline_pairs);
    REQUIRE(result.is_ok());
    REQUIRE(result.stats.algorithm
            == ::bioseq::minimizers::minimizer_kernel_algorithm::
                minimap2_reference);
    REQUIRE(result.stats.target == binding.target);
    REQUIRE(result.emitted == collector.extract_sketches_data().size());
    REQUIRE(result.stats.reference.kernel_calls == 1u);
    REQUIRE(result.stats.reference.last_storage_unit_bytes
            == sizeof(p_storage_unit));
}

TEST_CASE(
    "generated fixed-target reference SIMD matches mm_sketch on supported "
    "coordinates and tails",
    "[component:bioseq][layer:unit][feature:minimizer][resource:highway]")
{
    SECTION("dna4/u32 mixed full and partial iteration") {
        check_generated_reference_matches_mm_sketch<std::uint32_t>(
            minimizer_test_dna4_sequence_longer, 10, 15, "dna4-u32-longer");
    }

    SECTION("dna4/u64 poly-A tie chronology") {
        check_generated_reference_matches_mm_sketch<std::uint64_t>(
            minimizer_test_dna4_sequence_longer_2, 10, 15,
            "dna4-u64-polyA-2016");
    }
}

template<typename p_alphabet, typename p_storage_unit>
static ::bioseq::minimizers::minimizer_kernel_result
invoke_generated_minimizer_coordinate(
    std::vector<p_alphabet>& data,
    const ::bioseq::simd_dispatch::highway_kernel kernel,
    const ::bioseq::minimizers::minimizer_kernel_algorithm algorithm,
    sketch_collector& collector)
{
    using seq_type = sequence_custom_alphabet<p_alphabet, p_storage_unit>;

    auto sequence = compact_sequence_range<seq_type>(data);
    auto lazy = bioseq::storage::make_resident_lazy_sequence(sequence);
    const auto sequence_view =
        bioseq::storage::make_lazy_sequence_view(lazy);

    const auto& binding =
        ::bioseq::simd_dispatch::highway_test_target_binding();
    const auto& table =
        ::bioseq::simd_dispatch::highway_test_kernel_table();
    REQUIRE(binding.kernels == &table);

    const auto descriptor = pipe::try_minimizer_resident_packed(
        sequence_view, 0u, sequence_view.bp_size());
    REQUIRE(descriptor.has_value());
    REQUIRE(descriptor->storage_unit_width == sizeof(p_storage_unit));

    return pipe::invoke_selected_minimizer_kernel<minimap_sketch_data>(
        &table,
        kernel,
        algorithm,
        pipe::minimizer_kernel_view<p_alphabet>(*descriptor, &table),
        10u,
        15u,
        collector);
}

template<typename p_alphabet, typename p_storage_unit>
static void check_generated_reference_rejects_coordinate(
    std::vector<p_alphabet>& data,
    const std::string& coordinate)
{
    sketch_collector collector;
    const auto result = invoke_generated_minimizer_coordinate<
        p_alphabet, p_storage_unit>(
            data,
            ::bioseq::simd_dispatch::highway_kernel::
                minimizer_minimap2_reference,
            ::bioseq::minimizers::minimizer_kernel_algorithm::
                minimap2_reference,
            collector);
    const auto pairs = collector.extract_pairs();

    CAPTURE(coordinate);
    REQUIRE(result.status
            == ::bioseq::minimizers::minimizer_kernel_status::
                invalid_configuration);
    REQUIRE(result.stats.algorithm
            == ::bioseq::minimizers::minimizer_kernel_algorithm::
                minimap2_reference);
    REQUIRE(result.stats.target
            == ::bioseq::simd_dispatch::highway_test_target_binding().target);
    REQUIRE(result.emitted == 0u);
    REQUIRE(pairs.empty());
}

TEST_CASE(
    "generated fixed-target reference SIMD rejects unsupported coordinates",
    "[component:bioseq][layer:unit][feature:minimizer][resource:highway]")
{
    SECTION("dna4/u8") {
        check_generated_reference_rejects_coordinate<
            seq::dna4, std::uint8_t>(
                minimizer_test_dna4_sequence_longer, "dna4/u8");
    }
    SECTION("dna4/u16") {
        check_generated_reference_rejects_coordinate<
            seq::dna4, std::uint16_t>(
                minimizer_test_dna4_sequence_longer, "dna4/u16");
    }
    SECTION("dna5/u8") {
        check_generated_reference_rejects_coordinate<
            seq::dna5, std::uint8_t>(minimizer_test_dna5_sequence, "dna5/u8");
    }
    SECTION("dna5/u16") {
        check_generated_reference_rejects_coordinate<
            seq::dna5, std::uint16_t>(minimizer_test_dna5_sequence, "dna5/u16");
    }
    SECTION("dna5/u32") {
        check_generated_reference_rejects_coordinate<
            seq::dna5, std::uint32_t>(minimizer_test_dna5_sequence, "dna5/u32");
    }
    SECTION("dna5/u64") {
        check_generated_reference_rejects_coordinate<
            seq::dna5, std::uint64_t>(minimizer_test_dna5_sequence, "dna5/u64");
    }
}

template<typename p_alphabet, typename p_storage_unit>
static void check_generated_bounded_nthash_supports_coordinate(
    std::vector<p_alphabet>& data,
    const std::string& coordinate)
{
    sketch_collector collector;
    const auto result = invoke_generated_minimizer_coordinate<
        p_alphabet, p_storage_unit>(
            data,
            ::bioseq::simd_dispatch::highway_kernel::minimizer_bounded_nthash,
            ::bioseq::minimizers::minimizer_kernel_algorithm::bounded_nthash,
            collector);

    CAPTURE(coordinate);
    REQUIRE(result.is_ok());
    REQUIRE(result.family_status
            == static_cast<std::uint32_t>(
                ::bioseq::minimizers::bounded_nthash_status::ok));
    REQUIRE(result.stats.algorithm
            == ::bioseq::minimizers::minimizer_kernel_algorithm::bounded_nthash);
    REQUIRE(result.stats.target
            == ::bioseq::simd_dispatch::highway_test_target_binding().target);
    REQUIRE(result.emitted == collector.extract_sketches_data().size());
    REQUIRE(result.stats.bounded_nthash.is_internally_consistent(true));
    REQUIRE(result.stats.bounded_nthash.source_storage_unit_bytes == sizeof(p_storage_unit));
    REQUIRE(result.stats.bounded_nthash.fallback_calls == 0u);
    REQUIRE(result.stats.bounded_nthash.scalar_hash_symbols == 0u);
}

template<typename p_alphabet, typename p_storage_unit>
static void check_generated_bounded_nthash_rejects_coordinate(
    std::vector<p_alphabet>& data,
    const std::string& coordinate)
{
    sketch_collector collector;
    const auto result = invoke_generated_minimizer_coordinate<
        p_alphabet, p_storage_unit>(
            data,
            ::bioseq::simd_dispatch::highway_kernel::minimizer_bounded_nthash,
            ::bioseq::minimizers::minimizer_kernel_algorithm::bounded_nthash,
            collector);
    const auto pairs = collector.extract_pairs();

    CAPTURE(coordinate);
    REQUIRE(result.status
            == ::bioseq::minimizers::minimizer_kernel_status::
                invalid_configuration);
    constexpr auto expected_family = [] {
        if constexpr (std::same_as<p_alphabet, seq::dna5>) {
            return ::bioseq::minimizers::bounded_nthash_status::
                unsupported_alphabet;
        } else {
            return ::bioseq::minimizers::bounded_nthash_status::
                unsupported_storage;
        }
    }();
    REQUIRE(result.family_status
            == static_cast<std::uint32_t>(expected_family));
    REQUIRE(result.stats.algorithm
            == ::bioseq::minimizers::minimizer_kernel_algorithm::bounded_nthash);
    REQUIRE(result.stats.target
            == ::bioseq::simd_dispatch::highway_test_target_binding().target);
    REQUIRE(result.stats.bounded_nthash.fail_closed_rejections == 1u);
    REQUIRE(result.stats.bounded_nthash.fallback_calls == 0u);
    REQUIRE(result.stats.bounded_nthash.scalar_hash_symbols == 0u);
    REQUIRE(result.emitted == 0u);
    REQUIRE(pairs.empty());
}

TEST_CASE(
    "generated fixed-target bounded ntHash exposes exact coordinate capability",
    "[component:bioseq][layer:unit][feature:minimizer][resource:highway]")
{
    SECTION("dna4/u32 is supported") {
        check_generated_bounded_nthash_supports_coordinate<seq::dna4, std::uint32_t>(
            minimizer_test_dna4_sequence_longer, "dna4/u32");
    }
    SECTION("dna4/u64 is supported") {
        check_generated_bounded_nthash_supports_coordinate<seq::dna4, std::uint64_t>(
            minimizer_test_dna4_sequence_longer, "dna4/u64");
    }
    SECTION("dna4/u8 fails closed") {
        check_generated_bounded_nthash_rejects_coordinate<seq::dna4, std::uint8_t>(
            minimizer_test_dna4_sequence_longer, "dna4/u8");
    }
    SECTION("dna4/u16 fails closed") {
        check_generated_bounded_nthash_rejects_coordinate<seq::dna4, std::uint16_t>(
            minimizer_test_dna4_sequence_longer, "dna4/u16");
    }
    SECTION("dna5/u8 fails closed") {
        check_generated_bounded_nthash_rejects_coordinate<seq::dna5, std::uint8_t>(
            minimizer_test_dna5_sequence, "dna5/u8");
    }
    SECTION("dna5/u16 fails closed") {
        check_generated_bounded_nthash_rejects_coordinate<seq::dna5, std::uint16_t>(
            minimizer_test_dna5_sequence, "dna5/u16");
    }
    SECTION("dna5/u32 fails closed") {
        check_generated_bounded_nthash_rejects_coordinate<seq::dna5, std::uint32_t>(
            minimizer_test_dna5_sequence, "dna5/u32");
    }
    SECTION("dna5/u64 fails closed") {
        check_generated_bounded_nthash_rejects_coordinate<seq::dna5, std::uint64_t>(
            minimizer_test_dna5_sequence, "dna5/u64");
    }
}

}  // namespace bioseq::simd

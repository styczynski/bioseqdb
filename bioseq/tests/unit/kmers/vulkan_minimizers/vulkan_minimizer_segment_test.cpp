/**
 * @file vulkan_minimizer_segment_test.cpp
 * @brief Vulkan minimizer representation and segmented-route tests.
 */
#include <hwy/highway.h>
#include <tests/support/common.hpp>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <tuple>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

import bioseq.tests.unit.kmers.vulkan_minimizers_support;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {

namespace vk = ::bioseq::gpu::vulkan;
namespace idx = ::bioseq::indexing;

using namespace vulkan_minimizers_test_support;

TEST_CASE("vulkan minimizer: minimizer_representation 16/32/64/128-bit layouts round-trip",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) { SUCCEED("no hardware Vulkan device; skipped"); return; }
    // 16-bit: k_position_bits=10, hash fits 5 bits -> uint16 storage.
    using representation_16 = minimizer_representation<10u, 2u>;
    using representation_32 = minimizer_representation<10u, 9u>;       // pos 10 + hash 21 -> uint32 storage
    using representation_64 = minimap_sketch_data;            // minimizer_representation<28,15>, 64-bit
    using representation_128 = minimizer_representation<28u, 30u>;     // no room in 64 -> 128-bit
    static_assert(sizeof(representation_16) == 2);
    static_assert(sizeof(representation_32) == 4);
    static_assert(sizeof(representation_64) == 8);
    static_assert(sizeof(representation_128) == 16);

    {
        const auto cpu = triples(cpu_sketch<representation_16>(dna4_fixture, 10, 2));
        const auto gpu = triples(gpu_sketch<representation_16>(dna4_fixture, 10, 2));
        ASSERT(cpu == gpu, fmt::format("u16 repr parity failed (cpu={} gpu={})",
                                       cpu.size(), gpu.size()), cpu.size(), gpu.size());
    }
    {
        const auto cpu = triples(cpu_sketch<representation_32>(dna4_fixture, 10, 9));
        const auto gpu = triples(gpu_sketch<representation_32>(dna4_fixture, 10, 9));
        ASSERT(cpu == gpu, fmt::format("u32 repr parity failed (cpu={} gpu={})",
                                       cpu.size(), gpu.size()), cpu.size(), gpu.size());
    }
    {
        const auto cpu = triples(cpu_sketch<representation_64>(dna4_fixture, 10, 15));
        const auto gpu = triples(gpu_sketch<representation_64>(dna4_fixture, 10, 15));
        ASSERT(cpu == gpu, fmt::format("u64 repr parity failed (cpu={} gpu={})",
                                       cpu.size(), gpu.size()), cpu.size(), gpu.size());
    }
    {
        const auto cpu = triples(cpu_sketch<representation_128>(dna4_fixture, 10, 30));
        const auto gpu = triples(gpu_sketch<representation_128>(dna4_fixture, 10, 30));
        ASSERT(cpu == gpu, fmt::format("u64x2 repr parity failed (cpu={} gpu={})",
                                       cpu.size(), gpu.size()), cpu.size(), gpu.size());
    }
}

// ===========================================================================
// Checked Vulkan segment-path tests.
// ===========================================================================

TEST_CASE("Vulkan segment minimizer sketch matches scalar fragmented windows",
          "[component:bioseq][layer:unit][feature:minimizer][resource:gpu]") {
    if (!vulkan_ready()) { SUCCEED("no hardware Vulkan device; skipped"); return; }

    auto run_case = []<class p_alphabet_type>() {
        using configuration_type = sequence_custom_alphabet<p_alphabet_type, std::uint64_t>;
        using range_type = compact_sequence_range<configuration_type>;
        using storage_unit = typename range_type::traits::storage_unit;
        using scanner_type = bioseq::storage::chunked_span_sequence_scanner<
            storage_unit,
            bioseq::storage::default_fragment_storage_units>;
        using lazy_type = bioseq::storage::lazy_sequence<configuration_type, scanner_type>;
        using p_minimizer_type = minimap_sketch_data;

        constexpr std::size_t w = 10;
        constexpr std::size_t k = 15;
        // The segment sketch uses fused tiled Vulkan execution for every supported
        // alphabet. dna4 is still expected to be exact; dna5/generic may contain
        // bounded tile-boundary extras but must cover every scalar minimizer.
        //  (1) draw ranks from the NON-AMBIGUOUS letters only (ACGT, ranks 0..3).
        //      The original periodic `i % alphabet_size` put an N every 5th base
        //      for dna5 (every 15-mer spans an N → vacuous). ACGT here keeps
        //      this segment case focused on window tiling rather than reset
        //      handling. And
        //  (2) make the length a MULTIPLE of letters_per_unit so the trailing
        //      unit has zero padding — otherwise the scalar unit-iterator
        //      sub-range processes the last unit's padding slots (decoded as A)
        //      as spurious end-of-window minimizers that the GPU (which honours
        //      tail_padding) correctly omits. This is a separate scalar/GPU
        //      end-of-sequence discrepancy, independent of the tiled bug.
        constexpr std::uint8_t k_non_ambiguous_rank_count =
            range_type::traits::alphabet_size < 4u
                ? static_cast<std::uint8_t>(range_type::traits::alphabet_size)
                : 4u;
        const std::size_t letter_count = range_type::traits::letters_per_unit * 50000u;
        std::vector<typename configuration_type::alphabet_type> letters;
        letters.reserve(letter_count);
        std::uint64_t lcg = 0x9e3779b97f4a7c15ULL;
        for (std::size_t i = 0; i < letter_count; ++i) {
            lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
            typename configuration_type::alphabet_type value{};
            value.assign_rank(static_cast<std::uint8_t>((lcg >> 33) % k_non_ambiguous_rank_count));
            letters.push_back(value);
        }
        range_type range(letters);
        lazy_type fragmented{scanner_type::from_range(range)};
        const auto fragmented_view =
            bioseq::storage::make_lazy_sequence_view(fragmented);

        const std::array<std::pair<std::size_t, std::size_t>, 4> windows{{
            {0, 4096},
            {17, 8192},
            {4096, 16384},
            {range.size() > 20000
                 ? static_cast<std::size_t>(range.size()) - 20000
                 : 0,
             static_cast<std::size_t>(range.size())},
        }};

        for (const auto& window : windows) {
            const std::size_t lo_unit = window.first;
            const std::size_t hi_unit = std::min(
                window.second, static_cast<std::size_t>(range.size()));
            basic_sketch_collector<p_minimizer_type> scalar;
            basic_sketch_collector<p_minimizer_type> gpu;

            auto begin_iter = fragmented.cbegin();
            begin_iter += static_cast<bioseq::storage::sequence_size_t>(lo_unit);
            auto end_iter = fragmented.cbegin();
            end_iter += static_cast<bioseq::storage::sequence_size_t>(hi_unit);
            collect_minimizers_view<minimap_int_hasher, p_minimizer_type>(
                scalar, fragmented, begin_iter, end_iter, w, k);

            vk::collect_minimizers_vulkan_segment_or_throw<p_minimizer_type>(
                gpu,
                fragmented_view,
                static_cast<bioseq::storage::sequence_size_t>(lo_unit)
                    * range_type::traits::letters_per_unit,
                std::min<bioseq::storage::sequence_size_t>(
                    static_cast<bioseq::storage::sequence_size_t>(hi_unit)
                        * range_type::traits::letters_per_unit,
                    fragmented.bp_size()),
                w,
                k);

            auto scalar_values = scalar.extract_sketches_data();
            auto gpu_values = gpu.extract_sketches_data();
            auto normalize = [](auto& values) {
                std::sort(
                    values.begin(),
                    values.end(),
                    [](const p_minimizer_type& lhs, const p_minimizer_type& rhs) {
                        return std::tuple{lhs.kmer(), lhs.last_pos(), lhs.strand()}
                            < std::tuple{rhs.kmer(), rhs.last_pos(), rhs.strand()};
                    });
            };
            normalize(scalar_values);
            normalize(gpu_values);
            if constexpr (std::is_same_v<p_alphabet_type, seq::dna4>) {
                REQUIRE(gpu_values.size() == scalar_values.size());
                for (std::size_t i = 0; i < scalar_values.size(); ++i) {
                    CHECK(gpu_values[i].kmer() == scalar_values[i].kmer());
                    CHECK(gpu_values[i].last_pos() == scalar_values[i].last_pos());
                    CHECK(gpu_values[i].strand() == scalar_values[i].strand());
                }
            } else {
                const std::uint64_t bp_count =
                    static_cast<std::uint64_t>(hi_unit - lo_unit)
                    * static_cast<std::uint64_t>(range_type::traits::letters_per_unit);
                require_gpu_minimizer_superset_with_bounded_tile_extras(
                    scalar_values,
                    gpu_values,
                    vk::make_sketch_tile_plan(
                        bp_count,
                        static_cast<std::uint32_t>(k),
                        static_cast<std::uint32_t>(w),
                        range_type::traits::letters_per_unit),
                    static_cast<std::uint32_t>(k),
                    static_cast<std::uint32_t>(w));
            }
        }
    };

    run_case.template operator()<seq::dna4>();
    run_case.template operator()<seq::dna5>();
}

TEST_CASE("Vulkan segment sorted pairs are ordered by kmer then encoded position",
          "[component:bioseq][layer:unit][feature:minimizer][resource:gpu]") {
    if (!vulkan_ready()) { SUCCEED("no hardware Vulkan device; skipped"); return; }

    {
        using configuration_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
        using range_type = compact_sequence_range<configuration_type>;
        using storage_unit = typename range_type::traits::storage_unit;
        using scanner_type = bioseq::storage::chunked_span_sequence_scanner<
            storage_unit,
            bioseq::storage::default_fragment_storage_units>;
        using lazy_type = bioseq::storage::lazy_sequence<configuration_type, scanner_type>;

        std::vector<typename configuration_type::alphabet_type> letters;
        letters.reserve(1u << 20);
        std::uint64_t lcg = 0x9e3779b97f4a7c15ULL;
        for (std::size_t i = 0; i < (1u << 20); ++i) {
            lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
            typename configuration_type::alphabet_type value{};
            value.assign_rank(static_cast<std::uint8_t>((lcg >> 33) % 4u));
            letters.push_back(value);
        }
        range_type range(letters);
        lazy_type fragmented{scanner_type::from_range(range)};
        const auto fragmented_view =
            bioseq::storage::make_lazy_sequence_view(fragmented);

        basic_sketch_collector<minimap_sketch_data> scalar;
        collect_minimizers_view<minimap_int_hasher, minimap_sketch_data>(
            scalar,
            fragmented,
            fragmented.cbegin(),
            fragmented.cend(),
            10,
            15);
        std::vector<std::pair<std::uint64_t, std::uint64_t>> expected;
        for (const minimap_sketch_data& minimizer : scalar.extract_sketches_data()) {
            expected.emplace_back(
                minimizer.kmer(),
                (static_cast<std::uint64_t>(minimizer.last_pos()) << 1)
                    | static_cast<std::uint64_t>(minimizer.strand()));
        }
        std::sort(expected.begin(), expected.end());

        vk::vulkan_segment_pair_execution_stats execution_stats{};
        const auto pairs =
            vk::collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
                fragmented_view,
                0,
                fragmented.bp_size(),
                0,
                0,
                static_cast<bioseq::storage::sequence_size_t>(fragmented.bp_size()),
                10,
                15,
                &execution_stats);
        REQUIRE(pairs == expected);
        REQUIRE(execution_stats.is_internally_consistent());
        REQUIRE(execution_stats.completed_roundtrips > 0);
        REQUIRE_FALSE(execution_stats.saturated);
    }

    using configuration_type = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    using range_type = compact_sequence_range<configuration_type>;
    using storage_unit = typename range_type::traits::storage_unit;
    using scanner_type = bioseq::storage::chunked_span_sequence_scanner<
        storage_unit,
        bioseq::storage::default_fragment_storage_units>;
    using lazy_type = bioseq::storage::lazy_sequence<configuration_type, scanner_type>;

    std::vector<typename configuration_type::alphabet_type> letters;
    constexpr std::size_t dna5_letter_count = range_type::traits::letters_per_unit * 50000u;
    letters.reserve(dna5_letter_count);
    // Varied LCG ranks ensure that the dna5 sequence has
    // minimizers. `(i*7 + i/31) % 5` is periodic and puts an N (rank 4) every
    // 5th base, so every 15-mer contains an N → zero minimizers → empty pairs.
    std::uint64_t lcg = 0x2545f4914f6cdd1dULL;
    for (std::size_t i = 0; i < dna5_letter_count; ++i) {
        lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
        typename configuration_type::alphabet_type value{};
        value.assign_rank(static_cast<std::uint8_t>(
            (lcg >> 33) % range_type::traits::alphabet_size));
        letters.push_back(value);
    }
    range_type range(letters);
    lazy_type fragmented{scanner_type::from_range(range)};
    const auto fragmented_view =
        bioseq::storage::make_lazy_sequence_view(fragmented);

    basic_sketch_collector<minimap_sketch_data> scalar;
    collect_minimizers_view<minimap_int_hasher, minimap_sketch_data>(
        scalar,
        fragmented,
        fragmented.cbegin(),
        fragmented.cend(),
        10,
        15);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> expected;
    for (const minimap_sketch_data& minimizer : scalar.extract_sketches_data()) {
        expected.emplace_back(
            minimizer.kmer(),
            (static_cast<std::uint64_t>(minimizer.last_pos()) << 1)
                | static_cast<std::uint64_t>(minimizer.strand()));
    }
    std::sort(expected.begin(), expected.end());

    const auto pairs =
        vk::collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
            fragmented_view,
            0,
            fragmented.bp_size(),
            0,
            0,
            static_cast<bioseq::storage::sequence_size_t>(fragmented.bp_size()),
            10,
            15);
    REQUIRE(!pairs.empty());
    require_gpu_pair_superset_with_bounded_tile_extras(
        expected,
        pairs,
        vk::make_sketch_tile_plan(
            fragmented.bp_size(),
            15,
            10,
            range_type::traits::letters_per_unit),
        15,
        10);
}

TEST_CASE("Vulkan borrowed segment-pair admission covers arena high-water",
          "[component:bioseq][layer:unit][feature:minimizer][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    // Reproduce the ordering that exposed the regression: an earlier Vulkan
    // operation leaves a large reusable arena allocation, then a small
    // persisted-index segment borrows a slice from its outer owner.  Requested
    // bytes for the small segment are much lower than this physical high-water;
    // the borrowed slice must nevertheless cover the allocation that is live.
    auto& runtime = vk::runtime::instance();
    constexpr std::uint64_t high_water_request = std::uint64_t{32} << 20u;
    REQUIRE(runtime.reusable_gpu_arena().ensure_capacity(
        high_water_request,
        0u,
        0u));
    REQUIRE(runtime.reusable_gpu_arena().device_buffer().allocation_bytes
            >= high_water_request);

    using configuration_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using range_type = compact_sequence_range<configuration_type>;
    using storage_unit = typename range_type::traits::storage_unit;
    using scanner_type = bioseq::storage::chunked_span_sequence_scanner<
        storage_unit,
        bioseq::storage::default_fragment_storage_units>;
    using lazy_type = bioseq::storage::lazy_sequence<configuration_type, scanner_type>;

    range_type range(dna4_fixture);
    lazy_type fragmented{scanner_type::from_range(range)};
    const auto fragmented_view =
        bioseq::storage::make_lazy_sequence_view(fragmented);
    vk::vulkan_segment_pair_execution_stats execution_stats{};
    const auto pairs =
        vk::collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
            fragmented_view,
            0u,
            fragmented.bp_size(),
            0u,
            0u,
            static_cast<bioseq::storage::sequence_size_t>(
                fragmented.bp_size()),
            10u,
            15u,
            &execution_stats);

    REQUIRE_FALSE(pairs.empty());
    REQUIRE(execution_stats.is_internally_consistent());
    REQUIRE(execution_stats.completed_roundtrips > 0u);
    REQUIRE_FALSE(execution_stats.saturated);
}

}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

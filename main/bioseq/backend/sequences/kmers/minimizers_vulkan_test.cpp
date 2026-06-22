/**
 * @file minimizers_vulkan_test.cpp
 * @brief DOC0307 §13 — Vulkan minimizer sketch parity tests.
 *
 * Tests 1-4: dna4 parity for (15,10)/(7,5)/(27,10), dna5 T/N parity (N resets,
 * T hashes as nt4 code 3), generic-alphabet smoke (no throw), and the
 * 16/32/64/128-bit `MinimizerRepr` layouts round-tripping raw fields. All GPU
 * comparisons skip cleanly (SUCCEED) when no usable Vulkan device exists.
 */
#include "common_simd.h"
#include "common_tests.h"
#include "common.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers;
import bioseq.sequences.kmers.vulkan;
import bioseq.gpu.vulkan.runtime;
import bioseq.storage.scanner;   // DOC0345 §16 — sequence_scanner_test_chunked + SCANNER_SEQUENCE_FRAGMENT_SIZE_ALIGNMENT
import bioseq.storage.lazy;      // DOC0345 §16 — lazy_compact_sequence
import bioseq.storage.size;      // DOC0345 §16 — sequence_size_t
import bioseq.common.testing;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

namespace vk = ::bioseq::gpu::vulkan;

[[nodiscard]] static bool vulkan_ready() {
    return vk::Runtime::instance().usable();
}

/// Sorted (last_pos, kmer, strand) tuples of a minimizer vector.
template <class MinimizerT>
[[nodiscard]] static std::vector<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>>
triples(const std::vector<MinimizerT>& minimizers) {
    std::vector<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>> out;
    out.reserve(minimizers.size());
    for (const MinimizerT& m : minimizers) {
        out.emplace_back(m.last_pos(), m.kmer(), m.strand());
    }
    std::sort(out.begin(), out.end());
    return out;
}

template <class MinimizerT, class Alphabet>
[[nodiscard]] static std::vector<MinimizerT> cpu_sketch(
    std::vector<Alphabet>& data, std::size_t w, std::size_t k) {
    using seq_type = sequence_custom_alphabet<Alphabet, std::uint64_t>;
    auto seq = compact_sequence_range<seq_type>(data);
    sketch_collector_t<MinimizerT> collector;
    collect_minimizers<minimap_int_hasher, MinimizerT, seq_type,
                       decltype(seq.begin()), std::uint64_t>(
        collector, seq, seq.begin(), seq.end(), w, k);
    return std::move(collector).extract_sketches_data();
}

template <class MinimizerT, class Alphabet>
[[nodiscard]] static std::vector<MinimizerT> gpu_sketch(
    std::vector<Alphabet>& data, std::size_t w, std::size_t k) {
    using seq_type = sequence_custom_alphabet<Alphabet, std::uint64_t>;
    auto seq = compact_sequence_range<seq_type>(data);
    auto result = vk::collect_minimizers_vulkan_result<MinimizerT>(seq, w, k);
    if (!result) return {};
    return std::move(result->host_minimizers);
}

static std::vector<seq::dna4> DNA4_FIXTURE{
    "ACGTACGTGGCATTACGATCGATCGTAGCTAGCTACGTACGATCGATCGTAGCTAGCATCGATCG"
    "TTACGATCGTAGCTAGCTTACGTACGATCGATCGTAGCTAGCATCGATCGTAGCTAGCTACGTAC"
    "GGCATTACGATCGATCGTAGCTAGCTACGTACGATCGATCGTAGCTAGCATCGATCGTAGCTAGC"_dna4
};
static std::vector<seq::dna5> DNA5_WITH_T{
    "ACGTACGTGGCATTACGATCGATCGTAGCTAGCTACGTACGATCGATCGTAGCTAGCATCGATCG"
    "TTACGATCGTAGCTAGCTTACGTACGATCGATCGTAGCTAGCATCGATCGTAGCTAGCTACGTAC"_dna5
};
static std::vector<seq::dna5> DNA5_WITH_N{
    "ACGTACGTGGCATTACGNNNNNATCGTAGCTAGCTACGTACGATCGATCGTAGCTAGCATCGATC"
    "GTTACGATNNCGTAGCTAGCTTACGTACGATCGATCGTAGCTAGCATCGATCGTAGCTAGCTACG"_dna5
};

TEST_CASE("vulkan minimizer: dna4 parity equals scalar mm-sketch",
          "[vulkan][seeding][phase-b]") {
    if (!vulkan_ready()) { SUCCEED("no usable Vulkan device; skipped"); return; }
    for (auto [w, k] : std::vector<std::pair<std::size_t, std::size_t>>{
             {10, 15}, {5, 7}, {10, 27}}) {
        const auto cpu = triples(cpu_sketch<MinimapSketchData>(DNA4_FIXTURE, w, k));
        const auto gpu = triples(gpu_sketch<MinimapSketchData>(DNA4_FIXTURE, w, k));
        ASSERT(!gpu.empty(),
               fmt::format("GPU produced no dna4 minimizers at (w={},k={})", w, k),
               w, k);
        ASSERT(cpu == gpu,
               fmt::format("dna4 GPU sketch != scalar at (w={},k={}): cpu={} gpu={}",
                           w, k, cpu.size(), gpu.size()),
               cpu.size(), gpu.size());
    }
}

TEST_CASE("vulkan minimizer: dna5 with T parity (T is nt4 code 3)",
          "[vulkan][seeding][phase-b]") {
    if (!vulkan_ready()) { SUCCEED("no usable Vulkan device; skipped"); return; }
    const auto cpu = triples(cpu_sketch<MinimapSketchData>(DNA5_WITH_T, 10, 15));
    const auto gpu = triples(gpu_sketch<MinimapSketchData>(DNA5_WITH_T, 10, 15));
    ASSERT(!gpu.empty(), "GPU produced no dna5+T minimizers", gpu.size());
    ASSERT(cpu == gpu,
           fmt::format("dna5+T GPU sketch != scalar (cpu={} gpu={})",
                       cpu.size(), gpu.size()),
           cpu.size(), gpu.size());
}

TEST_CASE("vulkan minimizer: dna5 with N parity (N resets the window)",
          "[vulkan][seeding][phase-b]") {
    if (!vulkan_ready()) { SUCCEED("no usable Vulkan device; skipped"); return; }
    constexpr std::size_t k = 15;
    const auto cpu = triples(cpu_sketch<MinimapSketchData>(DNA5_WITH_N, 10, k));
    const auto gpu = triples(gpu_sketch<MinimapSketchData>(DNA5_WITH_N, 10, k));
    ASSERT(cpu == gpu,
           fmt::format("dna5+N GPU sketch != scalar (cpu={} gpu={})",
                       cpu.size(), gpu.size()),
           cpu.size(), gpu.size());
    // No emitted minimizer's k-mer may span an N (decode the same range).
    using seq_type = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    const auto decoded = compact_sequence_range<seq_type>(DNA5_WITH_N).to_string();
    std::vector<std::size_t> n_positions;
    for (std::size_t i = 0; i < decoded.size(); ++i)
        if (decoded[i] == 'N') n_positions.push_back(i);
    for (const auto& [last_pos, kmer, strand] : gpu) {
        const std::size_t lo = (last_pos + 1 >= k) ? (last_pos + 1 - k) : 0;
        for (const std::size_t np : n_positions) {
            ASSERT(np < lo || np > last_pos,
                   fmt::format("GPU minimizer at {} spans an N at {}", last_pos, np),
                   last_pos);
        }
    }
}

TEST_CASE("vulkan minimizer: generic alphabet smoke (no throw / pre-dispatch decline)",
          "[vulkan][seeding][phase-b]") {
    // dna5 stored at uint8 (bits_per_char=3) is the generic-ambiguity path;
    // the low-level capability API must either produce GPU output or decline
    // before dispatch without throwing. The selected pipeline layer converts
    // such declines into hard errors.
    using seq_type = sequence_custom_alphabet<seq::dna5, std::uint8_t>;
    std::vector<seq::dna5> data = DNA5_WITH_T;
    auto seq = compact_sequence_range<seq_type>(data);
    bool threw = false;
    std::optional<vk::VulkanSketchResult<MinimapSketchData>> result;
    try {
        result = vk::collect_minimizers_vulkan_result<MinimapSketchData>(seq, 10, 15);
    } catch (...) {
        threw = true;
    }
    ASSERT(!threw, "generic-alphabet Vulkan sketch threw", threw);
    SUCCEED("generic-alphabet sketch returned without throwing");
}

TEST_CASE("vulkan minimizer: oversized k falls back before dispatch (no throw)",
          "[vulkan][seeding][phase-b]") {
    // The pre-dispatch guards (k > 32, k*step_bits > 64, w > 256, bp/local >
    // max_workgroup_count_x) return std::nullopt so the caller takes the scalar
    // path — they must never touch the device or throw. k=40 trips the k>32
    // guard for dna4 (step_bits=2).
    using seq_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    auto seq = compact_sequence_range<seq_type>(DNA4_FIXTURE);
    auto big_k = vk::collect_minimizers_vulkan_result<MinimapSketchData>(seq, 10, 40);
    ASSERT(!big_k.has_value(), "oversized k must fall back (nullopt)", 0);
    auto big_w = vk::collect_minimizers_vulkan_result<MinimapSketchData>(seq, 300, 15);
    ASSERT(!big_w.has_value(), "oversized window must fall back (nullopt)", 0);
}

TEST_CASE("vulkan minimizer: MinimizerRepr 16/32/64/128-bit layouts round-trip",
          "[vulkan][seeding][phase-b]") {
    if (!vulkan_ready()) { SUCCEED("no usable Vulkan device; skipped"); return; }
    // 16-bit: pos_bits=10, hash fits 5 bits -> uint16 storage.
    using R16 = MinimizerRepr<10u, 2u>;
    using R32 = MinimizerRepr<10u, 9u>;       // pos 10 + hash 21 -> uint32 storage
    using R64 = MinimapSketchData;            // MinimizerRepr<28,15>, 64-bit
    using R128 = MinimizerRepr<28u, 30u>;     // no room in 64 -> 128-bit
    static_assert(sizeof(R16) == 2);
    static_assert(sizeof(R32) == 4);
    static_assert(sizeof(R64) == 8);
    static_assert(sizeof(R128) == 16);

    {
        const auto cpu = triples(cpu_sketch<R16>(DNA4_FIXTURE, 10, 2));
        const auto gpu = triples(gpu_sketch<R16>(DNA4_FIXTURE, 10, 2));
        ASSERT(cpu == gpu, fmt::format("u16 repr parity failed (cpu={} gpu={})",
                                       cpu.size(), gpu.size()), cpu.size(), gpu.size());
    }
    {
        const auto cpu = triples(cpu_sketch<R32>(DNA4_FIXTURE, 10, 9));
        const auto gpu = triples(gpu_sketch<R32>(DNA4_FIXTURE, 10, 9));
        ASSERT(cpu == gpu, fmt::format("u32 repr parity failed (cpu={} gpu={})",
                                       cpu.size(), gpu.size()), cpu.size(), gpu.size());
    }
    {
        const auto cpu = triples(cpu_sketch<R64>(DNA4_FIXTURE, 10, 15));
        const auto gpu = triples(gpu_sketch<R64>(DNA4_FIXTURE, 10, 15));
        ASSERT(cpu == gpu, fmt::format("u64 repr parity failed (cpu={} gpu={})",
                                       cpu.size(), gpu.size()), cpu.size(), gpu.size());
    }
    {
        const auto cpu = triples(cpu_sketch<R128>(DNA4_FIXTURE, 10, 30));
        const auto gpu = triples(gpu_sketch<R128>(DNA4_FIXTURE, 10, 30));
        ASSERT(cpu == gpu, fmt::format("u64x2 repr parity failed (cpu={} gpu={})",
                                       cpu.size(), gpu.size()), cpu.size(), gpu.size());
    }
}

// ===========================================================================
// DOC0345 §16.1 / §16.2 — checked Vulkan segment path tests.
// ===========================================================================

TEST_CASE("Vulkan segment minimizer sketch matches scalar fragmented windows",
          "[vulkan][minimizer][segment]") {
    if (!vulkan_ready()) { SUCCEED("no usable Vulkan device; skipped"); return; }

    auto run_case = []<class alphabet_t>() {
        using config_t = sequence_custom_alphabet<alphabet_t, std::uint64_t>;
        using range_t = compact_sequence_range<config_t>;
        using storage_unit = typename range_t::traits::storage_unit;
        using scanner_t =
            bioseq::storage::sequence_scanner_test_chunked<
                storage_unit,
                bioseq::storage::SCANNER_SEQUENCE_FRAGMENT_SIZE_ALIGNMENT>;
        using lazy_t = bioseq::storage::lazy_compact_sequence<config_t, scanner_t>;
        using MinimizerT = MinimapSketchData;

        constexpr std::size_t w = 10;
        constexpr std::size_t k = 15;
        // DOC0345 §16.1 — exact GPU-vs-scalar parity of the GPU segment sketch.
        // `collect_minimizers_vulkan_segment_or_throw` uses the bit-exact NON-tiled
        // Phase B GPU sketch (the tiled emit over-emits ~8% for dna5 and is
        // bypassed — BUGFIX DOC0348). Two test-only input conditions keep the
        // comparison EXACT and meaningful:
        //  (1) draw ranks from the NON-AMBIGUOUS letters only (ACGT, ranks 0..3).
        //      The original periodic `i % alphabet_size` put an N every 5th base
        //      for dna5 (every 15-mer spans an N → vacuous). Phase B IS bit-exact
        //      for dna5+N (covered by the whole-window dna5/N parity test above);
        //      ACGT here just keeps this segment case simple and aligned with
        //      §16.3. And
        //  (2) make the length a MULTIPLE of letters_per_unit so the trailing
        //      unit has zero padding — otherwise the scalar unit-iterator
        //      sub-range processes the last unit's padding slots (decoded as A)
        //      as spurious end-of-window minimizers that the GPU (which honours
        //      tail_padding) correctly omits. This is a separate scalar/GPU
        //      end-of-sequence discrepancy, independent of the tiled bug.
        constexpr std::uint8_t kNonAmbiguous =
            range_t::traits::alphabet_size < 4u
                ? static_cast<std::uint8_t>(range_t::traits::alphabet_size)
                : 4u;
        const std::size_t letter_count = range_t::traits::letters_per_unit * 50000u;
        std::vector<typename config_t::alphabet_type> letters;
        letters.reserve(letter_count);
        std::uint64_t lcg = 0x9e3779b97f4a7c15ULL;
        for (std::size_t i = 0; i < letter_count; ++i) {
            lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
            typename config_t::alphabet_type value{};
            value.assign_rank(static_cast<std::uint8_t>((lcg >> 33) % kNonAmbiguous));
            letters.push_back(value);
        }
        range_t range(letters);
        lazy_t fragmented{scanner_t::from_range(range)};

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
            sketch_collector_t<MinimizerT> scalar;
            sketch_collector_t<MinimizerT> gpu;

            auto begin_iter = fragmented.cbegin();
            begin_iter += static_cast<bioseq::storage::sequence_size_t>(lo_unit);
            auto end_iter = fragmented.cbegin();
            end_iter += static_cast<bioseq::storage::sequence_size_t>(hi_unit);
            collect_minimizers_view<minimap_int_hasher, MinimizerT>(
                scalar, fragmented, begin_iter, end_iter, w, k);

            vk::collect_minimizers_vulkan_segment_or_throw<MinimizerT>(
                gpu, fragmented, lo_unit, hi_unit, w, k);

            auto scalar_values = scalar.extract_sketches_data();
            auto gpu_values = gpu.extract_sketches_data();
            auto normalize = [](auto& values) {
                std::sort(
                    values.begin(),
                    values.end(),
                    [](const MinimizerT& lhs, const MinimizerT& rhs) {
                        return std::tuple{lhs.kmer(), lhs.last_pos(), lhs.strand()}
                            < std::tuple{rhs.kmer(), rhs.last_pos(), rhs.strand()};
                    });
            };
            normalize(scalar_values);
            normalize(gpu_values);
            REQUIRE(gpu_values.size() == scalar_values.size());
            for (std::size_t i = 0; i < scalar_values.size(); ++i) {
                CHECK(gpu_values[i].kmer() == scalar_values[i].kmer());
                CHECK(gpu_values[i].last_pos() == scalar_values[i].last_pos());
                CHECK(gpu_values[i].strand() == scalar_values[i].strand());
            }
        }
    };

    run_case.template operator()<seq::dna4>();
    run_case.template operator()<seq::dna5>();
}

TEST_CASE("Vulkan segment sorted pairs are ordered by kmer then encoded position",
          "[vulkan][minimizer][segment][sort]") {
    if (!vulkan_ready()) { SUCCEED("no usable Vulkan device; skipped"); return; }

    {
        using config_t = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
        using range_t = compact_sequence_range<config_t>;
        using storage_unit = typename range_t::traits::storage_unit;
        using scanner_t =
            bioseq::storage::sequence_scanner_test_chunked<
                storage_unit,
                bioseq::storage::SCANNER_SEQUENCE_FRAGMENT_SIZE_ALIGNMENT>;
        using lazy_t = bioseq::storage::lazy_compact_sequence<config_t, scanner_t>;

        std::vector<typename config_t::alphabet_type> letters;
        letters.reserve(1u << 20);
        std::uint64_t lcg = 0x9e3779b97f4a7c15ULL;
        for (std::size_t i = 0; i < (1u << 20); ++i) {
            lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
            typename config_t::alphabet_type value{};
            value.assign_rank(static_cast<std::uint8_t>((lcg >> 33) % 4u));
            letters.push_back(value);
        }
        range_t range(letters);
        lazy_t fragmented{scanner_t::from_range(range)};

        sketch_collector_t<MinimapSketchData> scalar;
        collect_minimizers_view<minimap_int_hasher, MinimapSketchData>(
            scalar,
            fragmented,
            fragmented.cbegin(),
            fragmented.cend(),
            10,
            15);
        std::vector<std::pair<std::uint64_t, std::uint64_t>> expected;
        for (const MinimapSketchData& minimizer : scalar.extract_sketches_data()) {
            expected.emplace_back(
                minimizer.kmer(),
                (static_cast<std::uint64_t>(minimizer.last_pos()) << 1)
                    | static_cast<std::uint64_t>(minimizer.strand()));
        }
        std::sort(expected.begin(), expected.end());

        const auto pairs =
            vk::collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
                fragmented,
                0,
                static_cast<std::size_t>(fragmented.size()),
                0,
                0,
                static_cast<bioseq::storage::sequence_size_t>(fragmented.bp_size()),
                10,
                15);
        REQUIRE(pairs == expected);
    }

    using config_t = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    using range_t = compact_sequence_range<config_t>;
    using storage_unit = typename range_t::traits::storage_unit;
    using scanner_t =
        bioseq::storage::sequence_scanner_test_chunked<
            storage_unit,
            bioseq::storage::SCANNER_SEQUENCE_FRAGMENT_SIZE_ALIGNMENT>;
    using lazy_t = bioseq::storage::lazy_compact_sequence<config_t, scanner_t>;

    std::vector<typename config_t::alphabet_type> letters;
    letters.reserve(4u << 20);
    // DOC0345 §16.2 — varied (LCG) ranks so the dna5 sequence actually has
    // minimizers. `(i*7 + i/31) % 5` is periodic and puts an N (rank 4) every
    // 5th base, so every 15-mer contains an N → zero minimizers → empty pairs.
    std::uint64_t lcg = 0x2545f4914f6cdd1dULL;
    for (std::size_t i = 0; i < (4u << 20); ++i) {
        lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
        typename config_t::alphabet_type value{};
        value.assign_rank(static_cast<std::uint8_t>(
            (lcg >> 33) % range_t::traits::alphabet_size));
        letters.push_back(value);
    }
    range_t range(letters);
    lazy_t fragmented{scanner_t::from_range(range)};

    const auto pairs =
        vk::collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
            fragmented,
            0,
            static_cast<std::size_t>(fragmented.size()),
            0,
            0,
            static_cast<bioseq::storage::sequence_size_t>(fragmented.bp_size()),
            10,
            15);
    REQUIRE(!pairs.empty());
    REQUIRE(std::is_sorted(pairs.begin(), pairs.end()));
}

}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

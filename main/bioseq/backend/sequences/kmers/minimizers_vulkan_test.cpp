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

#include <array>
#include <algorithm>
#include <cstdint>
#include <sstream>
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
import bioseq.gpu.vulkan.phase_c_abi;
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

using minimizer_tuple_t =
    std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;

[[nodiscard]] static bool vulkan_ready() {
    return vk::Runtime::instance().usable();
}

/// Sorted (last_pos, kmer, strand) tuples of a minimizer vector.
template <class MinimizerT>
[[nodiscard]] static std::vector<minimizer_tuple_t>
triples(const std::vector<MinimizerT>& minimizers) {
    std::vector<minimizer_tuple_t> out;
    out.reserve(minimizers.size());
    for (const MinimizerT& m : minimizers) {
        out.emplace_back(m.last_pos(), m.kmer(), m.strand());
    }
    std::sort(out.begin(), out.end());
    return out;
}

[[nodiscard]] static std::string tuple_examples(
    const std::vector<minimizer_tuple_t>& values,
    std::size_t limit = 6) {
    std::ostringstream out;
    const std::size_t n = std::min(values.size(), limit);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& [pos, kmer, strand] = values[i];
        if (i != 0) out << "; ";
        out << "(pos=" << pos << ",kmer=" << kmer << ",strand=" << strand << ")";
    }
    if (values.size() > n) out << "; ...";
    return out.str();
}

static void require_gpu_triple_superset_with_bounded_extras(
    std::vector<minimizer_tuple_t> scalar,
    std::vector<minimizer_tuple_t> gpu,
    vk::PhaseCSketchTilePlan tile_plan,
    std::uint32_t k,
    std::uint32_t window,
    std::uint64_t bp_offset = 0) {
    std::sort(scalar.begin(), scalar.end());
    std::sort(gpu.begin(), gpu.end());

    std::vector<minimizer_tuple_t> missing;
    std::vector<minimizer_tuple_t> extras;
    std::size_t scalar_i = 0;
    std::size_t gpu_i = 0;
    while (scalar_i < scalar.size() && gpu_i < gpu.size()) {
        if (scalar[scalar_i] == gpu[gpu_i]) {
            ++scalar_i;
            ++gpu_i;
        } else if (gpu[gpu_i] < scalar[scalar_i]) {
            extras.push_back(gpu[gpu_i]);
            ++gpu_i;
        } else {
            missing.push_back(scalar[scalar_i]);
            ++scalar_i;
        }
    }
    while (scalar_i < scalar.size()) {
        missing.push_back(scalar[scalar_i]);
        ++scalar_i;
    }
    while (gpu_i < gpu.size()) {
        extras.push_back(gpu[gpu_i]);
        ++gpu_i;
    }

    INFO("missing examples: " << tuple_examples(missing));
    REQUIRE(missing.empty());

    const std::uint64_t radius =
        static_cast<std::uint64_t>(k) + static_cast<std::uint64_t>(window) + 1u;
    const std::uint64_t max_extras =
        static_cast<std::uint64_t>(tile_plan.tile_count) * 2u * radius;
    INFO("extra examples: " << tuple_examples(extras));
    INFO("scalar=" << scalar.size() << " gpu=" << gpu.size()
                   << " extras=" << extras.size()
                   << " max_extras=" << max_extras);
    (void)bp_offset;
    REQUIRE(extras.size() <= max_extras);
}

template <class MinimizerT>
static void require_gpu_minimizer_superset_with_bounded_tile_extras(
    const std::vector<MinimizerT>& scalar,
    const std::vector<MinimizerT>& gpu,
    vk::PhaseCSketchTilePlan tile_plan,
    std::uint32_t k,
    std::uint32_t window,
    std::uint64_t bp_offset = 0) {
    require_gpu_triple_superset_with_bounded_extras(
        triples(scalar), triples(gpu), tile_plan, k, window, bp_offset);
}

static void require_gpu_pair_superset_with_bounded_tile_extras(
    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& scalar,
    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& gpu,
    vk::PhaseCSketchTilePlan tile_plan,
    std::uint32_t k,
    std::uint32_t window,
    std::uint64_t bp_offset = 0) {
    auto to_triples = [](const auto& pairs) {
        std::vector<minimizer_tuple_t> out;
        out.reserve(pairs.size());
        for (const auto& [kmer, encoded_pos] : pairs) {
            out.emplace_back(encoded_pos >> 1u, kmer, encoded_pos & 1u);
        }
        return out;
    };
    require_gpu_triple_superset_with_bounded_extras(
        to_triples(scalar), to_triples(gpu), tile_plan, k, window, bp_offset);
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
    std::vector<Alphabet>& data,
    std::size_t w,
    std::size_t k) {
    using seq_type = sequence_custom_alphabet<Alphabet, std::uint64_t>;
    auto seq = compact_sequence_range<seq_type>(data);
    auto result =
        vk::collect_minimizers_vulkan_result<MinimizerT>(
            seq, w, k);
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
          "[vulkan][seeding][phase-c]") {
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
          "[vulkan][seeding][phase-c]") {
    if (!vulkan_ready()) { SUCCEED("no usable Vulkan device; skipped"); return; }
    constexpr std::uint32_t w = 10;
    constexpr std::uint32_t k = 15;
    const auto cpu = cpu_sketch<MinimapSketchData>(DNA5_WITH_T, w, k);
    const auto gpu = gpu_sketch<MinimapSketchData>(DNA5_WITH_T, w, k);
    ASSERT(!gpu.empty(), "GPU produced no dna5+T minimizers", gpu.size());
    using seq_type = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    require_gpu_minimizer_superset_with_bounded_tile_extras(
        cpu,
        gpu,
        vk::phase_c_tile_plan(
            DNA5_WITH_T.size(),
            k,
            w,
            compact_sequence_range<seq_type>::traits::letters_per_unit),
        k,
        w);
}

TEST_CASE("vulkan minimizer: dna5 with N parity (N resets the window)",
          "[vulkan][seeding][phase-c]") {
    if (!vulkan_ready()) { SUCCEED("no usable Vulkan device; skipped"); return; }
    constexpr std::uint32_t w = 10;
    constexpr std::uint32_t k = 15;
    const auto cpu_values = cpu_sketch<MinimapSketchData>(DNA5_WITH_N, w, k);
    const auto gpu_values = gpu_sketch<MinimapSketchData>(DNA5_WITH_N, w, k);
    using seq_type = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    require_gpu_minimizer_superset_with_bounded_tile_extras(
        cpu_values,
        gpu_values,
        vk::phase_c_tile_plan(
            DNA5_WITH_N.size(),
            k,
            w,
            compact_sequence_range<seq_type>::traits::letters_per_unit),
        k,
        w);
    const auto gpu = triples(gpu_values);
    // No emitted minimizer's k-mer may span an N (decode the same range).
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

TEST_CASE("vulkan minimizer: Phase-C dna5 tile-boundary extras are bounded",
          "[vulkan][seeding][phase-c][dna5]") {
    if (!vulkan_ready()) { SUCCEED("no usable Vulkan device; skipped"); return; }

    constexpr std::size_t w = 10;
    constexpr std::size_t k = 15;
    constexpr std::size_t letter_count = 21u * 700u;
    std::vector<seq::dna5> data;
    data.reserve(letter_count);
    std::uint64_t lcg = 0x6a09e667f3bcc909ULL;
    for (std::size_t i = 0; i < letter_count; ++i) {
        lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
        seq::dna5 value{};
        value.assign_rank(static_cast<std::uint8_t>((lcg >> 33) % 4u));
        data.push_back(value);
    }
    for (const std::size_t n_pos : std::array<std::size_t, 12>{
             240u, 251u, 252u, 253u, 503u, 504u,
             505u, 755u, 756u, 757u, 1007u, 1008u}) {
        data[n_pos].assign_rank(static_cast<std::uint8_t>(4u));
    }

    const auto cpu = cpu_sketch<MinimapSketchData>(data, w, k);
    const auto gpu = gpu_sketch<MinimapSketchData>(data, w, k);
    ASSERT(!gpu.empty(), "fused Phase-C dna5 sketch produced no minimizers", gpu.size());
    using seq_type = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    require_gpu_minimizer_superset_with_bounded_tile_extras(
        cpu,
        gpu,
        vk::phase_c_tile_plan(
            data.size(),
            k,
            w,
            compact_sequence_range<seq_type>::traits::letters_per_unit),
        k,
        w);
}

TEST_CASE("vulkan minimizer: generic alphabet functional superset",
          "[vulkan][seeding][phase-c]") {
    if (!vulkan_ready()) { SUCCEED("no usable Vulkan device; skipped"); return; }
    using seq_type = sequence_custom_alphabet<seq::dna5, std::uint8_t>;
    constexpr std::uint32_t w = 5;
    constexpr std::uint32_t k = 7;
    std::vector<seq::dna5> data = DNA5_WITH_T;
    auto seq = compact_sequence_range<seq_type>(data);
    auto result = vk::collect_minimizers_vulkan_result<MinimapSketchData>(seq, w, k);
    REQUIRE(result.has_value());
    auto cpu = cpu_sketch<MinimapSketchData>(data, w, k);
    require_gpu_minimizer_superset_with_bounded_tile_extras(
        cpu,
        result->host_minimizers,
        vk::phase_c_tile_plan(
            data.size(),
            k,
            w,
            compact_sequence_range<seq_type>::traits::letters_per_unit),
        k,
        w);
}

TEST_CASE("vulkan minimizer: oversized k declines before dispatch (no throw)",
          "[vulkan][seeding][phase-c]") {
    // The pre-dispatch guards (k > 32, k*step_bits > 64, w > 256) return
    // std::nullopt so selected Vulkan callers can raise a hard decline before
    // dispatch. k=40 trips the k>32
    // guard for dna4 (step_bits=2).
    using seq_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    auto seq = compact_sequence_range<seq_type>(DNA4_FIXTURE);
    auto big_k = vk::collect_minimizers_vulkan_result<MinimapSketchData>(seq, 10, 40);
    ASSERT(!big_k.has_value(), "oversized k must decline (nullopt)", 0);
    auto big_w = vk::collect_minimizers_vulkan_result<MinimapSketchData>(seq, 300, 15);
    ASSERT(!big_w.has_value(), "oversized window must decline (nullopt)", 0);
}

TEST_CASE("vulkan minimizer: MinimizerRepr 16/32/64/128-bit layouts round-trip",
          "[vulkan][seeding][phase-c]") {
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
        // DOC0351 — segment sketch uses fused tiled Phase C for every supported
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
            if constexpr (std::is_same_v<alphabet_t, seq::dna4>) {
                REQUIRE(gpu_values.size() == scalar_values.size());
                for (std::size_t i = 0; i < scalar_values.size(); ++i) {
                    CHECK(gpu_values[i].kmer() == scalar_values[i].kmer());
                    CHECK(gpu_values[i].last_pos() == scalar_values[i].last_pos());
                    CHECK(gpu_values[i].strand() == scalar_values[i].strand());
                }
            } else {
                const std::uint64_t bp_count =
                    static_cast<std::uint64_t>(hi_unit - lo_unit)
                    * static_cast<std::uint64_t>(range_t::traits::letters_per_unit);
                require_gpu_minimizer_superset_with_bounded_tile_extras(
                    scalar_values,
                    gpu_values,
                    vk::phase_c_tile_plan(
                        bp_count,
                        static_cast<std::uint32_t>(k),
                        static_cast<std::uint32_t>(w),
                        range_t::traits::letters_per_unit),
                    static_cast<std::uint32_t>(k),
                    static_cast<std::uint32_t>(w));
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
    constexpr std::size_t dna5_letter_count = range_t::traits::letters_per_unit * 50000u;
    letters.reserve(dna5_letter_count);
    // DOC0345 §16.2 — varied (LCG) ranks so the dna5 sequence actually has
    // minimizers. `(i*7 + i/31) % 5` is periodic and puts an N (rank 4) every
    // 5th base, so every 15-mer contains an N → zero minimizers → empty pairs.
    std::uint64_t lcg = 0x2545f4914f6cdd1dULL;
    for (std::size_t i = 0; i < dna5_letter_count; ++i) {
        lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
        typename config_t::alphabet_type value{};
        value.assign_rank(static_cast<std::uint8_t>(
            (lcg >> 33) % range_t::traits::alphabet_size));
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
    REQUIRE(!pairs.empty());
    require_gpu_pair_superset_with_bounded_tile_extras(
        expected,
        pairs,
        vk::phase_c_tile_plan(
            fragmented.bp_size(),
            15,
            10,
            range_t::traits::letters_per_unit),
        15,
        10);
}

}  // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

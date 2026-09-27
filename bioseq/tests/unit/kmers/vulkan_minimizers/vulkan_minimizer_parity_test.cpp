/**
 * @file vulkan_minimizer_parity_test.cpp
 * @brief Vulkan minimizer scalar parity and decline tests.
 */
#include <hwy/highway.h>
#include <cstring>

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>
#include <libassert/assert.hpp>
#include <libassert/assert-catch2.hpp>

import std;
import bioseq.tests.unit.kmers.vulkan_minimizers_support;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {

namespace vk = ::bioseq::gpu::vulkan;
namespace idx = ::bioseq::indexing;

using namespace vulkan_minimizers_test_support;

TEST_CASE("vulkan minimizer: dna4 parity equals scalar mm-sketch",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    for (auto [w, k] :
         std::vector<std::pair<std::size_t, std::size_t>>{{10, 15}, {5, 7}, {10, 27}}) {
        const auto cpu = triples(cpu_sketch<minimap_sketch_data>(dna4_fixture, w, k));
        const auto gpu = triples(gpu_sketch<minimap_sketch_data>(dna4_fixture, w, k));
        ASSERT(!gpu.empty(),
               fmt::format("GPU produced no dna4 minimizers at (w={},k={})", w, k),
               w,
               k);
        ASSERT(cpu == gpu,
               fmt::format("dna4 GPU sketch != scalar at (w={},k={}): cpu={} gpu={}",
                           w,
                           k,
                           cpu.size(),
                           gpu.size()),
               cpu.size(),
               gpu.size());
    }
}

TEST_CASE("vulkan minimizer: dna5 with T parity (T is nt4 code 3)",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    constexpr std::uint32_t w = 10;
    constexpr std::uint32_t k = 15;
    const auto cpu = cpu_sketch<minimap_sketch_data>(dna5_with_t, w, k);
    const auto gpu = gpu_sketch<minimap_sketch_data>(dna5_with_t, w, k);
    ASSERT(!gpu.empty(), "GPU produced no dna5+T minimizers", gpu.size());
    using seq_type = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    require_gpu_minimizer_superset_with_bounded_tile_extras(
        cpu,
        gpu,
        vk::make_sketch_tile_plan(
            dna5_with_t.size(), k, w, compact_sequence_range<seq_type>::traits::letters_per_unit),
        k,
        w);
}

TEST_CASE("vulkan minimizer: dna5 with N parity (N resets the window)",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    constexpr std::uint32_t w = 10;
    constexpr std::uint32_t k = 15;
    const auto cpu_values = cpu_sketch<minimap_sketch_data>(dna5_with_n, w, k);
    const auto gpu_values = gpu_sketch<minimap_sketch_data>(dna5_with_n, w, k);
    using seq_type = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    require_gpu_minimizer_superset_with_bounded_tile_extras(
        cpu_values,
        gpu_values,
        vk::make_sketch_tile_plan(
            dna5_with_n.size(), k, w, compact_sequence_range<seq_type>::traits::letters_per_unit),
        k,
        w);
    const auto gpu = triples(gpu_values);
    // No emitted minimizer's k-mer may span an N (decode the same range).
    const auto decoded = compact_sequence_range<seq_type>(dna5_with_n).to_string();
    std::vector<std::size_t> n_positions;
    for (std::size_t i = 0; i < decoded.size(); ++i)
        if (decoded[i] == 'N')
            n_positions.push_back(i);
    for (const auto& [last_pos, kmer, strand] : gpu) {
        const std::size_t lo = (last_pos + 1 >= k) ? (last_pos + 1 - k) : 0;
        for (const std::size_t np : n_positions) {
            ASSERT(np < lo || np > last_pos,
                   fmt::format("GPU minimizer at {} spans an N at {}", last_pos, np),
                   last_pos);
        }
    }
}

TEST_CASE("vulkan minimizer: dna5 tile-boundary extras are bounded",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

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
             240u, 251u, 252u, 253u, 503u, 504u, 505u, 755u, 756u, 757u, 1007u, 1008u}) {
        data[n_pos].assign_rank(static_cast<std::uint8_t>(4u));
    }

    const auto cpu = cpu_sketch<minimap_sketch_data>(data, w, k);
    const auto gpu = gpu_sketch<minimap_sketch_data>(data, w, k);
    ASSERT(!gpu.empty(), "fused dna5 sketch produced no minimizers", gpu.size());
    using seq_type = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    require_gpu_minimizer_superset_with_bounded_tile_extras(
        cpu,
        gpu,
        vk::make_sketch_tile_plan(
            data.size(), k, w, compact_sequence_range<seq_type>::traits::letters_per_unit),
        k,
        w);
}

TEST_CASE("vulkan minimizer: generic alphabet functional superset",
          "[component:bioseq][layer:unit][feature:seeding][resource:gpu]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    using seq_type = sequence_custom_alphabet<seq::dna5, std::uint8_t>;
    constexpr std::uint32_t w = 5;
    constexpr std::uint32_t k = 7;
    std::vector<seq::dna5> data = dna5_with_t;
    auto seq = compact_sequence_range<seq_type>(data);
    auto sequence_owner = bioseq::storage::make_resident_lazy_sequence(seq);
    const auto sequence_view = bioseq::storage::make_lazy_sequence_view(sequence_owner);
    auto result = vk::collect_minimizers_vulkan_result<minimap_sketch_data>(sequence_view, w, k);
    REQUIRE(result.has_value());
    auto cpu = cpu_sketch<minimap_sketch_data>(data, w, k);
    require_gpu_minimizer_superset_with_bounded_tile_extras(
        cpu,
        result->host_minimizers,
        vk::make_sketch_tile_plan(
            data.size(), k, w, compact_sequence_range<seq_type>::traits::letters_per_unit),
        k,
        w);
}

TEST_CASE("vulkan minimizer: oversized k declines before dispatch (no throw)",
          "[component:bioseq][layer:unit][feature:seeding]") {
    // The pre-dispatch guards (k > 32, k*step_bits > 64, w > 256) return
    // std::nullopt so selected Vulkan callers can raise a hard decline before
    // dispatch. k=40 trips the k>32
    // guard for dna4 (step_bits=2).
    using seq_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    auto seq = compact_sequence_range<seq_type>(dna4_fixture);
    auto sequence_owner = bioseq::storage::make_resident_lazy_sequence(seq);
    const auto sequence_view = bioseq::storage::make_lazy_sequence_view(sequence_owner);
    vk::vulkan_sketch_decline decline{};
    auto big_k =
        vk::collect_minimizers_vulkan_result<minimap_sketch_data>(sequence_view, 10, 40, &decline);
    ASSERT(!big_k.has_value(), "oversized k must decline (nullopt)", 0);
    REQUIRE(decline.reason == vk::vulkan_sketch_decline_reason::kmer_size_unsupported);
    REQUIRE(decline.kmer_size == 40u);
    auto big_w =
        vk::collect_minimizers_vulkan_result<minimap_sketch_data>(sequence_view, 300, 15, &decline);
    ASSERT(!big_w.has_value(), "oversized window must decline (nullopt)", 0);
    REQUIRE(decline.reason == vk::vulkan_sketch_decline_reason::window_size_unsupported);
    REQUIRE(decline.window_size == 300u);
}

TEST_CASE("vulkan minimizer: typed decline preserves admission snapshot and strict message",
          "[component:bioseq][layer:unit][feature:seeding]") {
    const vk::vulkan_sketch_decline decline{
        .reason = vk::vulkan_sketch_decline_reason::memory_admission_denied,
        .bp_size = 101u,
        .padded_sequence_bytes = 32u,
        .max_minimizers = 17u,
        .query_gate_words = 3u,
        .window_size = 10u,
        .kmer_size = 15u,
        .admission =
            {
                .reason = bioseq::parallelism::memory_admission_failure_reason::budget_exhausted,
                .requested_bytes = 701u,
                .reserved_bytes = 211u,
                .budget_ceiling_bytes = 503u,
                .allocatable_after_reservations_bytes = 292u,
                .process_footprint_bytes = 401u,
                .system_available_bytes = 601u,
                .system_total_bytes = 809u,
                .process_soft_cap_bytes = 307u,
            },
    };
    const std::string formatted = vk::format_vulkan_sketch_decline(decline);
    REQUIRE(formatted.find("reason=memory_admission_denied") != std::string::npos);
    REQUIRE(formatted.find("admission_reason=budget_exhausted") != std::string::npos);
    REQUIRE(formatted.find("required_bytes=701") != std::string::npos);
    REQUIRE(formatted.find("allocatable_after_reservations_bytes=292") != std::string::npos);
    REQUIRE(formatted.find("reserved_bytes=211") != std::string::npos);
    REQUIRE(formatted.find("budget_ceiling_bytes=503") != std::string::npos);
    REQUIRE(formatted.find("process_footprint_bytes=401") != std::string::npos);
    REQUIRE(formatted.find("system_available_bytes=601") != std::string::npos);
    REQUIRE(formatted.find("system_total_bytes=809") != std::string::npos);
    REQUIRE(formatted.find("process_soft_cap_bytes=307") != std::string::npos);

    bool caught = false;
    try {
        vk::throw_vulkan_strict_decline("typed target-index fixture", decline);
    } catch (const vk::vulkan_runtime_error& error) {
        caught = true;
        const std::string message = error.what();
        REQUIRE(message.find("typed target-index fixture selected") != std::string::npos);
        REQUIRE(message.find("reason=memory_admission_denied") != std::string::npos);
        REQUIRE(message.find("admission_reason=budget_exhausted") != std::string::npos);
        REQUIRE(message.find("system_total_bytes=809") != std::string::npos);
    }
    REQUIRE(caught);
}

TEST_CASE("vulkan minimizer: only reducible declines permit segment retry",
          "[component:bioseq][layer:unit][feature:seeding]") {
    STATIC_REQUIRE(vk::vulkan_sketch_decline_allows_segment_split(
        vk::vulkan_sketch_decline_reason::memory_admission_denied));
    STATIC_REQUIRE(vk::vulkan_sketch_decline_allows_segment_split(
        vk::vulkan_sketch_decline_reason::output_capacity_overflow));
    STATIC_REQUIRE(vk::vulkan_sketch_decline_allows_segment_split(
        vk::vulkan_sketch_decline_reason::sequence_size_exceeds_u32));
    STATIC_REQUIRE_FALSE(vk::vulkan_sketch_decline_allows_segment_split(
        vk::vulkan_sketch_decline_reason::hardware_device_unavailable));
    STATIC_REQUIRE_FALSE(vk::vulkan_sketch_decline_allows_segment_split(
        vk::vulkan_sketch_decline_reason::kmer_size_unsupported));
    STATIC_REQUIRE_FALSE(vk::vulkan_sketch_decline_allows_segment_split(
        vk::vulkan_sketch_decline_reason::query_gate_exceeds_u32));
}

TEST_CASE("vulkan minimizer: target-index decline identity survives wrappers",
          "[component:bioseq][layer:unit][feature:index]") {
    STATIC_REQUIRE(vk::vulkan_target_index_failure_decline_reason(
                       vk::gpu_index_build_failure::output_capacity_overflow) ==
                   vk::vulkan_sketch_decline_reason::output_capacity_overflow);
    STATIC_REQUIRE(vk::vulkan_target_index_failure_decline_reason(
                       vk::gpu_index_build_failure::memory_plan_invalid) ==
                   vk::vulkan_sketch_decline_reason::memory_plan_invalid);
    STATIC_REQUIRE(vk::vulkan_target_index_failure_decline_reason(
                       vk::gpu_index_build_failure::device_buffer_allocation_failed) ==
                   vk::vulkan_sketch_decline_reason::device_allocation_failed);
    STATIC_REQUIRE(vk::vulkan_target_index_failure_decline_reason(
                       vk::gpu_index_build_failure::count_peak_memory_admission_denied) ==
                   vk::vulkan_sketch_decline_reason::memory_admission_denied);
    STATIC_REQUIRE(vk::vulkan_target_index_failure_decline_reason(
                       vk::gpu_index_build_failure::execution_peak_memory_admission_denied) ==
                   vk::vulkan_sketch_decline_reason::memory_admission_denied);
    STATIC_REQUIRE(vk::vulkan_target_index_failure_decline_reason(
                       vk::gpu_index_build_failure::index_invariant_failed) ==
                   vk::vulkan_sketch_decline_reason::target_index_build_invariant_failed);

    using seq_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    auto sequence = compact_sequence_range<seq_type>(dna4_fixture);
    auto sequence_owner = bioseq::storage::make_resident_lazy_sequence(sequence);
    const auto sequence_view = bioseq::storage::make_lazy_sequence_view(sequence_owner);
    vk::vulkan_sketch_decline decline{};
    auto built =
        vk::build_gpu_index_on_device<minimap_sketch_data>(sequence_view, 10u, 40u, {}, &decline);
    REQUIRE_FALSE(built.has_value());
    REQUIRE(decline.reason == vk::vulkan_sketch_decline_reason::kmer_size_unsupported);
    REQUIRE(decline.bp_size == sequence.bp_size());
    REQUIRE(decline.window_size == 10u);
    REQUIRE(decline.kmer_size == 40u);
}

} // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

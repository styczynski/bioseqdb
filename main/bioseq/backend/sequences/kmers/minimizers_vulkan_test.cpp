/**
 * @file minimizers_vulkan_test.cpp
 * @brief DOC0307 §13 — Vulkan minimizer sketch parity tests.
 *
 * Tests 1-4: dna4 parity for (15,10)/(7,5)/(27,10), dna5 T/N parity (N resets,
 * T hashes as nt4 code 3), generic-alphabet smoke (no throw), and the
 * 16/32/64/128-bit `MinimizerRepr` layouts round-tripping raw fields. All GPU
 * comparisons skip cleanly (SUCCEED) when no hardware Vulkan device exists.
 */
#include "common_simd.h"
#include "common_tests.h"
#include "common.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
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
import bioseq.parallelism.memory_admission;
import bioseq.storage.scanner;   // DOC0345 §16 — sequence_scanner_test_chunked + SCANNER_SEQUENCE_FRAGMENT_SIZE_ALIGNMENT
import bioseq.storage.lazy;      // DOC0345 §16 — lazy_compact_sequence
import bioseq.storage.size;      // DOC0345 §16 — sequence_size_t

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

namespace vk = ::bioseq::gpu::vulkan;

using minimizer_tuple_t =
    std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;

[[nodiscard]] static bool vulkan_ready() {
    return vk::hardware_device_available(vk::Runtime::instance());
}

[[nodiscard]] static bool software_vulkan_ready() {
    auto& runtime = vk::Runtime::instance();
    return runtime.usable() && runtime.features().software_device;
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

template <class Alphabet>
static void require_exact_device_only_batch_parity(
    std::vector<Alphabet> first_data,
    std::vector<Alphabet> second_data)
{
    using MinimizerT = MinimapSketchData;
    using seq_type = sequence_custom_alphabet<Alphabet, std::uint64_t>;
    constexpr std::size_t window_size = 10u;
    constexpr std::size_t kmer_size = 15u;

    auto first_sequence = compact_sequence_range<seq_type>(first_data);
    auto second_sequence = compact_sequence_range<seq_type>(second_data);
    const std::array<vk::SequenceUploadView, 2> uploads{
        vk::make_sequence_upload_view(first_sequence),
        vk::make_sequence_upload_view(second_sequence),
    };
    const auto rank_lut =
        vk::make_vulkan_rank_lut<decltype(first_sequence)>();
    const std::span<const vk::SequenceUploadView> upload_span{
        uploads.data(), uploads.size()};

    auto host_and_device =
        vk::collect_minimap_sketch_data_vulkan_upload_batch_result(
            upload_span,
            rank_lut,
            window_size,
            kmer_size,
            vk::VulkanSketchHostMaterialization::host_and_device);
    auto device_only =
        vk::collect_minimap_sketch_data_vulkan_upload_batch_result(
            upload_span,
            rank_lut,
            window_size,
            kmer_size,
            vk::VulkanSketchHostMaterialization::device_only);

    REQUIRE(host_and_device.has_value());
    REQUIRE(device_only.has_value());
    REQUIRE(host_and_device->dispatch_proof.accepted());
    REQUIRE(host_and_device->dispatch_proof.materialization()
            == vk::VulkanSketchHostMaterialization::host_and_device);
    REQUIRE(host_and_device->dispatch_proof.count_dispatches() == 1u);
    REQUIRE(host_and_device->dispatch_proof.prefix_scan_sequences() == 1u);
    REQUIRE(host_and_device->dispatch_proof.prefix_scan_dispatches() == 3u);
    REQUIRE(host_and_device->dispatch_proof.emit_dispatches() == 1u);
    REQUIRE(host_and_device->dispatch_proof.completed_roundtrips() == 1u);
    REQUIRE(host_and_device->materialization
            == vk::VulkanSketchHostMaterialization::host_and_device);
    REQUIRE_FALSE(host_and_device->host_minimizers.empty());
    REQUIRE(device_only->dispatch_proof.accepted());
    REQUIRE(device_only->dispatch_proof.materialization()
            == vk::VulkanSketchHostMaterialization::device_only);
    REQUIRE(device_only->dispatch_proof.count_dispatches() == 1u);
    REQUIRE(device_only->dispatch_proof.prefix_scan_sequences() == 1u);
    REQUIRE(device_only->dispatch_proof.prefix_scan_dispatches() == 3u);
    REQUIRE(device_only->dispatch_proof.emit_dispatches() == 1u);
    REQUIRE(device_only->dispatch_proof.completed_roundtrips() == 2u);
    REQUIRE(device_only->materialization
            == vk::VulkanSketchHostMaterialization::device_only);
    REQUIRE(device_only->host_minimizers.empty());

    REQUIRE(host_and_device->device != nullptr);
    REQUIRE(device_only->device != nullptr);
    REQUIRE(host_and_device->memory_retention != nullptr);
    REQUIRE(device_only->memory_retention != nullptr);
    REQUIRE(host_and_device->device->batch_memory_retention
            == host_and_device->memory_retention);
    REQUIRE(device_only->device->batch_memory_retention
            == device_only->memory_retention);
    REQUIRE(host_and_device->device->capacity
            >= host_and_device->device->count);
    const std::uint64_t host_vector_capacity_bytes =
        static_cast<std::uint64_t>(host_and_device->host_minimizers.capacity())
        * sizeof(MinimizerT);
    const std::uint64_t host_query_record_capacity_bytes =
        static_cast<std::uint64_t>(host_and_device->query_records.capacity())
        * sizeof(vk::PhaseCSeedQueryRecordWire);
    REQUIRE(host_and_device->memory_retention->permit.bytes()
            == host_and_device->device->words.allocation_bytes
                + host_vector_capacity_bytes
                + host_query_record_capacity_bytes);
    const std::uint64_t device_query_record_capacity_bytes =
        static_cast<std::uint64_t>(device_only->query_records.capacity())
        * sizeof(vk::PhaseCSeedQueryRecordWire);
    REQUIRE(device_only->memory_retention->permit.bytes()
            == device_only->device->words.allocation_bytes
                + device_query_record_capacity_bytes);
    REQUIRE(device_only->device_minimizer_count
            == host_and_device->host_minimizers.size());
    REQUIRE(device_only->device->count
            == device_only->device_minimizer_count);
    REQUIRE(device_only->device->capacity
            == device_only->device_minimizer_count);
    const std::uint64_t exact_device_bytes = std::max<std::uint64_t>(
        vk::minimizer_wire_traits<MinimizerT>::device_bytes_for_count(
            device_only->device_minimizer_count),
        4u);
    REQUIRE(device_only->device->words.bytes == exact_device_bytes);

    REQUIRE(device_only->query_records.size() == uploads.size());
    REQUIRE(host_and_device->query_records.size() == uploads.size());
    std::uint64_t query_cursor = 0u;
    for (std::size_t query_index = 0; query_index < uploads.size(); ++query_index) {
        const auto& actual = device_only->query_records[query_index];
        const auto& expected = host_and_device->query_records[query_index];
        REQUIRE(actual.minimizer_offset == query_cursor);
        REQUIRE(actual.minimizer_offset == expected.minimizer_offset);
        REQUIRE(actual.minimizer_count == expected.minimizer_count);
        REQUIRE(actual.query_len == expected.query_len);
        REQUIRE(actual.flags == expected.flags);
        query_cursor += actual.minimizer_count;
    }
    REQUIRE(query_cursor == device_only->device_minimizer_count);

    // Test-only readback: production device-only mode never creates a host
    // hash vector, but this focused oracle downloads its exact resident bytes
    // and proves byte/order parity with the ordinary materialized call.
    const std::size_t minimizer_count = static_cast<std::size_t>(
        device_only->device_minimizer_count);
    std::vector<std::byte> downloaded_bytes(
        minimizer_count * sizeof(MinimizerT));
    vk::Runtime::instance().download_slice(
        device_only->device->words,
        0u,
        std::span<std::byte>{
            downloaded_bytes.data(), downloaded_bytes.size()});
    std::vector<MinimizerT> downloaded(minimizer_count);
    if (!downloaded_bytes.empty()) {
        std::memcpy(
            downloaded.data(),
            downloaded_bytes.data(),
            downloaded_bytes.size());
    }
    REQUIRE(downloaded.size() == host_and_device->host_minimizers.size());
    for (std::size_t i = 0; i < downloaded.size(); ++i) {
        REQUIRE(downloaded[i].raw_value()
                == host_and_device->host_minimizers[i].raw_value());
    }
}

TEST_CASE("vulkan minimizer: dna4 batch supports exact device-only materialization",
          "[vulkan][seeding][phase-c][batch][device-only]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    std::vector<seq::dna4> second{
        DNA4_FIXTURE.begin() + 17,
        DNA4_FIXTURE.end() - 11};
    require_exact_device_only_batch_parity(DNA4_FIXTURE, std::move(second));
}

TEST_CASE("vulkan minimizer: dna5 batch supports exact device-only materialization",
          "[vulkan][seeding][phase-c][batch][device-only]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    require_exact_device_only_batch_parity(DNA5_WITH_T, DNA5_WITH_N);
}

TEST_CASE("vulkan minimizer: device-only zero emission is dispatched and sealed",
          "[vulkan][seeding][phase-c][batch][device-only][zero]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    using seq_type = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    constexpr std::size_t window_size = 10u;
    constexpr std::size_t kmer_size = 15u;

    std::vector<seq::dna5> too_short = "NNNNNNNNNNNNNN"_dna5;
    auto too_short_sequence = compact_sequence_range<seq_type>(too_short);
    const std::array<vk::SequenceUploadView, 1> too_short_uploads{
        vk::make_sequence_upload_view(too_short_sequence),
    };
    const auto rank_lut =
        vk::make_vulkan_rank_lut<decltype(too_short_sequence)>();
    auto nondispatchable =
        vk::collect_minimap_sketch_data_vulkan_upload_batch_result(
            std::span<const vk::SequenceUploadView>{
                too_short_uploads.data(), too_short_uploads.size()},
            rank_lut,
            window_size,
            kmer_size,
            vk::VulkanSketchHostMaterialization::device_only);
    REQUIRE_FALSE(nondispatchable.has_value());

    std::vector<seq::dna5> all_n =
        "NNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNN"
        "NNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNN"_dna5;
    REQUIRE(all_n.size() >= kmer_size);
    auto all_n_sequence = compact_sequence_range<seq_type>(all_n);
    const std::array<vk::SequenceUploadView, 1> all_n_uploads{
        vk::make_sequence_upload_view(all_n_sequence),
    };
    const auto all_n_tile_plan = vk::phase_c_tile_plan(
        all_n.size(),
        static_cast<std::uint32_t>(kmer_size),
        static_cast<std::uint32_t>(window_size),
        all_n_uploads[0].letters_per_unit,
        vk::Runtime::instance().features().max_workgroup_count_x);
    REQUIRE(all_n_tile_plan.dispatchable);
    const std::uint64_t pessimistic_count =
        vk::estimate_vulkan_minimizer_capacity(
            all_n.size(),
            window_size);
    const auto allocation_requirements =
        vk::Runtime::instance().phase_c_sketch_batch_allocation_requirements(
            all_n_uploads[0].padded_byte_size,
            all_n_tile_plan.tile_count,
            static_cast<std::uint32_t>(
                vk::minimizer_wire_traits<MinimapSketchData>::word_kind),
            vk::minimizer_wire_traits<
                MinimapSketchData>::words32_per_minimizer,
            pessimistic_count);
    REQUIRE(allocation_requirements.valid);
    REQUIRE(allocation_requirements.arena_requested_bytes > 0u);
    REQUIRE(allocation_requirements.arena_allocation_bytes
            >= allocation_requirements.arena_requested_bytes);
    REQUIRE(allocation_requirements.pessimistic_output_requested_bytes
            == std::max<std::uint64_t>(
                vk::minimizer_wire_traits<
                    MinimapSketchData>::device_bytes_for_count(
                        pessimistic_count),
                4u));
    REQUIRE(allocation_requirements.pessimistic_output_allocation_bytes
            >= allocation_requirements.pessimistic_output_requested_bytes);
    auto result =
        vk::collect_minimap_sketch_data_vulkan_upload_batch_result(
            std::span<const vk::SequenceUploadView>{
                all_n_uploads.data(), all_n_uploads.size()},
            rank_lut,
            window_size,
            kmer_size,
            vk::VulkanSketchHostMaterialization::device_only);

    REQUIRE(result.has_value());
    REQUIRE(result->dispatch_proof.accepted());
    REQUIRE(result->dispatch_proof.materialization()
            == vk::VulkanSketchHostMaterialization::device_only);
    REQUIRE(result->dispatch_proof.count_dispatches() == 1u);
    REQUIRE(result->dispatch_proof.prefix_scan_sequences() == 1u);
    REQUIRE(result->dispatch_proof.prefix_scan_dispatches() == 3u);
    REQUIRE(result->dispatch_proof.emit_dispatches() == 1u);
    REQUIRE(result->dispatch_proof.completed_roundtrips() == 2u);
    REQUIRE(result->query_records.size() == 1u);
    REQUIRE(result->query_records[0].minimizer_offset == 0u);
    REQUIRE(result->query_records[0].minimizer_count == 0u);
    REQUIRE(result->query_records[0].query_len == all_n.size());
    REQUIRE(result->query_records[0].flags == 0u);
    REQUIRE(result->host_minimizers.empty());
    REQUIRE(result->host_minimizers.capacity() == 0u);
    REQUIRE(result->device != nullptr);
    REQUIRE(result->memory_retention != nullptr);
    REQUIRE(result->device->batch_memory_retention
            == result->memory_retention);
    REQUIRE(result->device_minimizer_count == 0u);
    REQUIRE(result->device->count == 0u);
    REQUIRE(result->device->capacity == 0u);
    REQUIRE(result->device->words.bytes == 4u);
    REQUIRE(result->memory_retention->permit.bytes()
            == result->device->words.allocation_bytes
                + static_cast<std::uint64_t>(
                    result->query_records.capacity())
                    * sizeof(vk::PhaseCSeedQueryRecordWire));
    REQUIRE(result->memory_retention->permit.bytes()
            >= result->device->words.allocation_bytes);

    std::vector<std::byte> downloaded;
    vk::Runtime::instance().download_slice(
        result->device->words,
        0u,
        std::span<std::byte>{downloaded.data(), downloaded.size()});
    REQUIRE(downloaded.empty());
}

TEST_CASE("vulkan minimizer: dna4 parity equals scalar mm-sketch",
          "[vulkan][seeding][phase-c]") {
    if (!vulkan_ready()) { SUCCEED("no hardware Vulkan device; skipped"); return; }
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
    if (!vulkan_ready()) { SUCCEED("no hardware Vulkan device; skipped"); return; }
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
    if (!vulkan_ready()) { SUCCEED("no hardware Vulkan device; skipped"); return; }
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
    if (!vulkan_ready()) { SUCCEED("no hardware Vulkan device; skipped"); return; }

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
    if (!vulkan_ready()) { SUCCEED("no hardware Vulkan device; skipped"); return; }
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
    vk::VulkanSketchDecline decline{};
    auto big_k = vk::collect_minimizers_vulkan_result<MinimapSketchData>(
        seq, 10, 40, &decline);
    ASSERT(!big_k.has_value(), "oversized k must decline (nullopt)", 0);
    REQUIRE(decline.reason == vk::VulkanSketchDeclineReason::kmer_size_unsupported);
    REQUIRE(decline.kmer_size == 40u);
    auto big_w = vk::collect_minimizers_vulkan_result<MinimapSketchData>(
        seq, 300, 15, &decline);
    ASSERT(!big_w.has_value(), "oversized window must decline (nullopt)", 0);
    REQUIRE(decline.reason == vk::VulkanSketchDeclineReason::window_size_unsupported);
    REQUIRE(decline.window_size == 300u);
}

TEST_CASE("vulkan minimizer: typed decline preserves admission snapshot and strict message",
          "[vulkan][seeding][phase-c][decline]") {
    const vk::VulkanSketchDecline decline{
        .reason = vk::VulkanSketchDeclineReason::memory_admission_denied,
        .bp_size = 101u,
        .padded_sequence_bytes = 32u,
        .max_minimizers = 17u,
        .query_gate_words = 3u,
        .window_size = 10u,
        .kmer_size = 15u,
        .admission = {
            .reason = bioseq::parallelism::MemoryAdmissionFailureReason::
                budget_exhausted,
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
    REQUIRE(formatted.find("allocatable_after_reservations_bytes=292")
            != std::string::npos);
    REQUIRE(formatted.find("reserved_bytes=211") != std::string::npos);
    REQUIRE(formatted.find("budget_ceiling_bytes=503") != std::string::npos);
    REQUIRE(formatted.find("process_footprint_bytes=401") != std::string::npos);
    REQUIRE(formatted.find("system_available_bytes=601") != std::string::npos);
    REQUIRE(formatted.find("system_total_bytes=809") != std::string::npos);
    REQUIRE(formatted.find("process_soft_cap_bytes=307") != std::string::npos);

    bool caught = false;
    try {
        vk::throw_vulkan_strict_decline("typed target-index fixture", decline);
    } catch (const vk::VulkanRuntimeError& error) {
        caught = true;
        const std::string message = error.what();
        REQUIRE(message.find("typed target-index fixture selected")
                != std::string::npos);
        REQUIRE(message.find("reason=memory_admission_denied")
                != std::string::npos);
        REQUIRE(message.find("admission_reason=budget_exhausted")
                != std::string::npos);
        REQUIRE(message.find("system_total_bytes=809") != std::string::npos);
    }
    REQUIRE(caught);
}

TEST_CASE("vulkan minimizer: only reducible declines permit segment retry",
          "[vulkan][seeding][phase-c][decline][segment]") {
    STATIC_REQUIRE(vk::vulkan_sketch_decline_allows_segment_split(
        vk::VulkanSketchDeclineReason::memory_admission_denied));
    STATIC_REQUIRE(vk::vulkan_sketch_decline_allows_segment_split(
        vk::VulkanSketchDeclineReason::output_capacity_overflow));
    STATIC_REQUIRE(vk::vulkan_sketch_decline_allows_segment_split(
        vk::VulkanSketchDeclineReason::sequence_size_exceeds_u32));
    STATIC_REQUIRE_FALSE(vk::vulkan_sketch_decline_allows_segment_split(
        vk::VulkanSketchDeclineReason::hardware_device_unavailable));
    STATIC_REQUIRE_FALSE(vk::vulkan_sketch_decline_allows_segment_split(
        vk::VulkanSketchDeclineReason::kmer_size_unsupported));
    STATIC_REQUIRE_FALSE(vk::vulkan_sketch_decline_allows_segment_split(
        vk::VulkanSketchDeclineReason::query_gate_exceeds_u32));
}

TEST_CASE("vulkan minimizer: target-index decline identity survives wrappers",
          "[vulkan][seeding][phase-c][decline][target-index]") {
    STATIC_REQUIRE(vk::vulkan_target_index_failure_decline_reason(
        vk::GpuBuiltIndexFailureReason::output_capacity_overflow)
        == vk::VulkanSketchDeclineReason::output_capacity_overflow);
    STATIC_REQUIRE(vk::vulkan_target_index_failure_decline_reason(
        vk::GpuBuiltIndexFailureReason::memory_plan_invalid)
        == vk::VulkanSketchDeclineReason::memory_plan_invalid);
    STATIC_REQUIRE(vk::vulkan_target_index_failure_decline_reason(
        vk::GpuBuiltIndexFailureReason::device_buffer_allocation_failed)
        == vk::VulkanSketchDeclineReason::device_allocation_failed);
    STATIC_REQUIRE(vk::vulkan_target_index_failure_decline_reason(
        vk::GpuBuiltIndexFailureReason::count_peak_memory_admission_denied)
        == vk::VulkanSketchDeclineReason::memory_admission_denied);
    STATIC_REQUIRE(vk::vulkan_target_index_failure_decline_reason(
        vk::GpuBuiltIndexFailureReason::index_invariant_failed)
        == vk::VulkanSketchDeclineReason::
            target_index_build_invariant_failed);

    using seq_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    auto sequence = compact_sequence_range<seq_type>(DNA4_FIXTURE);
    vk::VulkanSketchDecline decline{};
    auto built = vk::build_gpu_index_on_device<MinimapSketchData>(
        sequence,
        10u,
        40u,
        {},
        &decline);
    REQUIRE_FALSE(built.has_value());
    REQUIRE(decline.reason
            == vk::VulkanSketchDeclineReason::kmer_size_unsupported);
    REQUIRE(decline.bp_size == sequence.bp_size());
    REQUIRE(decline.window_size == 10u);
    REQUIRE(decline.kmer_size == 40u);
}

TEST_CASE("vulkan minimizer: target-index count admission covers persistent arena high-water",
          "[vulkan][seeding][phase-c][admission][arena]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto& runtime = vk::Runtime::instance();
    constexpr std::uint64_t high_water_request = std::uint64_t{16} << 20;
    REQUIRE(runtime.phase_c_arena().ensure_capacity(
        high_water_request, 0u, 0u));
    const std::uint64_t high_water =
        runtime.phase_c_arena().device_buffer().allocation_bytes;
    REQUIRE(high_water >= high_water_request);
    REQUIRE(runtime.phase_c_arena_allocation_requirement(1024u) >= high_water);

    auto mutable_gate_words = std::make_shared<std::vector<std::uint32_t>>(
        vk::kPhaseCQueryGateWordCount,
        (std::numeric_limits<std::uint32_t>::max)());
    const std::shared_ptr<const std::vector<std::uint32_t>> gate_words =
        std::move(mutable_gate_words);
    const auto gate_identity =
        vk::canonical_vulkan_sketch_query_gate_identity(
            std::span<const std::uint32_t>{*gate_words},
            vk::kPhaseCQueryGateMask,
            gate_words);
    const vk::VulkanSketchQueryGate gate{
        std::span<const std::uint32_t>{*gate_words},
        vk::kPhaseCQueryGateMask,
        gate_identity,
        true};
    using seq_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    auto sequence = compact_sequence_range<seq_type>(DNA4_FIXTURE);
    vk::VulkanSketchDecline decline{};
    auto built = vk::build_gpu_index_on_device<MinimapSketchData>(
        sequence, 10u, 15u, gate, &decline);
    INFO(vk::format_vulkan_sketch_decline(decline));
    REQUIRE(built.has_value());
    REQUIRE(built->valid);
    REQUIRE(built->ownership.count_phase_admissions == 1u);
    REQUIRE(built->ownership.main_peak_admissions == 1u);
    REQUIRE(built->ownership.gate_count_dispatches == 1u);
    REQUIRE(built->ownership.gated_emit_dispatches == 1u);
    REQUIRE(built->ownership.ungated_emit_dispatches == 0u);
    REQUIRE(built->ownership.retained_minimizer_count > 0u);
    REQUIRE(built->ownership.distinct_minimizer_count > 0u);
    REQUIRE(built->ownership.peak_observed_bytes
            <= built->ownership.peak_admitted_bytes);
}

TEST_CASE("vulkan minimizer: truncated query gate fails before target-index dispatch",
          "[vulkan][seeding][phase-c][gate]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    using seq_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    auto sequence = compact_sequence_range<seq_type>(DNA4_FIXTURE);
    const std::array<std::uint32_t, 1> truncated_words{1u};
    const vk::VulkanSketchQueryGate truncated_gate{
        std::span<const std::uint32_t>{truncated_words}, 31u};
    REQUIRE_THROWS_AS(
        vk::build_gpu_index_on_device<MinimapSketchData>(
            sequence, 10u, 15u, truncated_gate),
        vk::VulkanRuntimeError);
}

TEST_CASE("vulkan minimizer: selected segment preserves typed strict cause",
          "[vulkan][seeding][phase-c][decline][segment]") {
    using config_t = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using range_t = compact_sequence_range<config_t>;
    using storage_unit = typename range_t::traits::storage_unit;
    using scanner_t =
        bioseq::storage::sequence_scanner_test_chunked<
            storage_unit,
            bioseq::storage::SCANNER_SEQUENCE_FRAGMENT_SIZE_ALIGNMENT>;
    using lazy_t = bioseq::storage::lazy_compact_sequence<config_t, scanner_t>;

    range_t range(DNA4_FIXTURE);
    lazy_t fragmented{scanner_t::from_range(range)};
    sketch_collector_t<MinimapSketchData> collector;
    bool caught = false;
    try {
        vk::collect_minimizers_vulkan_segment_or_throw<MinimapSketchData>(
            collector,
            fragmented,
            0u,
            static_cast<std::size_t>(fragmented.size()),
            10u,
            40u);
    } catch (const vk::VulkanRuntimeError& error) {
        caught = true;
        const std::string message = error.what();
        REQUIRE(message.find("Vulkan segment minimizer sketch selected")
                != std::string::npos);
        REQUIRE(message.find("reason=kmer_size_unsupported")
                != std::string::npos);
        REQUIRE(message.find("w=10 k=40") != std::string::npos);
    }
    REQUIRE(caught);
}

TEST_CASE("vulkan minimizer: explicit output overflow has typed identity",
          "[vulkan][seeding][phase-c][decline][overflow]") {
    if (!vulkan_ready()) {
        SUCCEED("no hardware Vulkan device; output-overflow dispatch skipped");
        return;
    }
    using seq_type = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    auto sequence = compact_sequence_range<seq_type>(DNA4_FIXTURE);
    const auto upload = vk::make_sequence_upload_view(sequence);
    const auto rank_lut = vk::make_vulkan_rank_lut<decltype(sequence)>();
    bool capacity_overflowed = false;
    vk::VulkanSketchDecline decline{};
    auto result = vk::sketch_uploaded_window_result<MinimapSketchData>(
        upload,
        rank_lut,
        10u,
        15u,
        1u,
        &capacity_overflowed,
        {},
        &decline);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(capacity_overflowed);
    REQUIRE(decline.reason
            == vk::VulkanSketchDeclineReason::output_capacity_overflow);
    REQUIRE(decline.max_minimizers == 1u);
}

TEST_CASE("vulkan minimizer: software device declines selected paths",
          "[vulkan][seeding][phase-c][mandatory]") {
    if (!software_vulkan_ready()) {
        SUCCEED("No software Vulkan device detected; hardware GPU cases cover the selected path");
        return;
    }

    using config_t = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using range_t = compact_sequence_range<config_t>;
    using storage_unit = typename range_t::traits::storage_unit;
    using scanner_t =
        bioseq::storage::sequence_scanner_test_chunked<
            storage_unit,
            bioseq::storage::SCANNER_SEQUENCE_FRAGMENT_SIZE_ALIGNMENT>;
    using lazy_t = bioseq::storage::lazy_compact_sequence<config_t, scanner_t>;

    range_t range(DNA4_FIXTURE);
    auto result = vk::collect_minimizers_vulkan_result<MinimapSketchData>(range, 10, 15);
    REQUIRE_FALSE(result.has_value());

    lazy_t fragmented{scanner_t::from_range(range)};
    REQUIRE_THROWS_AS(
        vk::collect_sorted_minimizer_pairs_vulkan_segment_or_throw(
            fragmented,
            0,
            static_cast<std::size_t>(fragmented.size()),
            0,
            0,
            static_cast<bioseq::storage::sequence_size_t>(fragmented.bp_size()),
            10,
            15),
        vk::VulkanRuntimeError);
}

TEST_CASE("vulkan minimizer: MinimizerRepr 16/32/64/128-bit layouts round-trip",
          "[vulkan][seeding][phase-c]") {
    if (!vulkan_ready()) { SUCCEED("no hardware Vulkan device; skipped"); return; }
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
    if (!vulkan_ready()) { SUCCEED("no hardware Vulkan device; skipped"); return; }

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
    if (!vulkan_ready()) { SUCCEED("no hardware Vulkan device; skipped"); return; }

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

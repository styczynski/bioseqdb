/**
 * @file minimizer_vulkan_index_test.cpp
 * @brief DOC0307 §13 — GPU minimizer index view + cache identity tests.
 *
 * The open-addressed and hash-sorted dense GPU control representations derived
 * from `MinimizerHashmapFragmentIndex` resolve the same (hash -> positions)
 * hits as CPU `lookup`, and distinct `assembly_identity` values produce
 * distinct cache keys / hashes. Pure-host tests cover both layouts; resident
 * upload tests skip when no device exists.
 */
#include "common_tests.h"
#include "common.h"

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

import bioseq.common.mlogging;
import bioseq.sequences.kmers;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_vulkan;
import bioseq.indexing.storage;
import bioseq.indexing.type;
import bioseq.sequences.kmers.vulkan;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;

namespace {

using MinimapSketchData = bioseq::indexing::MinimapSketchData;
using bioseq::indexing::MinimizerHashmapFragmentIndex;
namespace idx_ns = bioseq::indexing;

/// Host simulation of the shader's open-addressed `find_slot`.
[[nodiscard]] static std::uint32_t host_find_slot(
    const idx_ns::GpuMinimizerIndexUpload& upload, std::uint64_t hash) {
    std::uint32_t p = static_cast<std::uint32_t>(idx_ns::splitmix64(hash)) & upload.slot_mask;
    for (std::uint32_t step = 0; step <= upload.slot_mask; ++step) {
        const auto& slot = upload.slots[p];
        if (slot.pos_count == 0) return 0xffffffffu;
        if (slot.hash == hash) return p;
        p = (p + 1u) & upload.slot_mask;
    }
    return 0xffffffffu;
}

/// Host simulation of the shader's sorted-dense `find_slot` branch.
[[nodiscard]] static std::uint32_t host_find_sorted_dense_slot(
    std::span<const idx_ns::GpuMinimizerSlot> slots,
    std::uint64_t hash) {
    std::size_t lo = 0u;
    std::size_t hi = slots.size();
    while (lo < hi) {
        const std::size_t mid = (lo + hi) >> 1u;
        if (slots[mid].hash < hash) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo < slots.size() && slots[lo].hash == hash
        ? static_cast<std::uint32_t>(lo)
        : 0xffffffffu;
}

[[nodiscard]] static std::uint32_t host_find_slot(
    std::span<const idx_ns::GpuMinimizerSlot> slots,
    std::uint32_t slot_mask,
    std::uint64_t hash) {
    std::uint32_t p = static_cast<std::uint32_t>(idx_ns::splitmix64(hash)) & slot_mask;
    for (std::uint32_t step = 0; step <= slot_mask; ++step) {
        const auto& slot = slots[p];
        if (slot.pos_count == 0) return 0xffffffffu;
        if (slot.hash == hash) return p;
        p = (p + 1u) & slot_mask;
    }
    return 0xffffffffu;
}

[[nodiscard]] static MinimizerHashmapFragmentIndex build_demo_index() {
    std::vector<MinimapSketchData> minimizers;
    // Several distinct kmers; one with multiple positions (a repeat).
    minimizers.emplace_back(0x111ULL, 5u, 0u);
    minimizers.emplace_back(0x222ULL, 9u, 1u);
    minimizers.emplace_back(0x333ULL, 13u, 0u);
    minimizers.emplace_back(0x222ULL, 40u, 0u);   // repeat kmer 0x222
    minimizers.emplace_back(0x444ULL, 77u, 1u);
    minimizers.emplace_back(0x222ULL, 100u, 1u);  // repeat kmer 0x222 again
    MinimizerHashmapFragmentIndex idx;
    idx.build(minimizers, 256);
    return idx;
}

template <class T>
[[nodiscard]] static std::vector<std::byte> object_bytes(
    const std::vector<T>& values)
{
    const auto bytes = std::as_bytes(std::span<const T>{values});
    return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

struct PersistedGpuFixture {
    std::array<std::vector<idx_ns::GpuMinimizerSlot>, 4> controls;
    std::array<std::vector<std::uint64_t>, 4> positions;
    std::array<std::vector<std::byte>, 4> control_bytes;
    std::array<std::vector<std::byte>, 4> position_bytes;
    std::array<idx_ns::MinimizerBucketDirEntry, 4> entries;
    idx_ns::MinimizerGenerationHead head;
    idx_ns::MinimizerGenerationManifest manifest;
    idx_ns::DirView directory;
};

class IndexMemoryCapScope {
public:
    explicit IndexMemoryCapScope(std::uint64_t cap)
        : controller_(bioseq::parallelism::MemoryAdmissionController::instance()),
          previous_(controller_.index_memory_cap_bytes()) {
        controller_.set_index_memory_cap_bytes(cap);
    }
    ~IndexMemoryCapScope() {
        controller_.set_index_memory_cap_bytes(previous_);
    }

private:
    bioseq::parallelism::MemoryAdmissionController& controller_;
    std::uint64_t previous_ = 0u;
};

[[nodiscard]] static bioseq::gpu::vulkan::GpuBuiltIndex
make_adoptable_gpu_built_index(
    bioseq::gpu::vulkan::Runtime& runtime,
    bool gate_enabled,
    bool empty = false)
{
    constexpr VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bioseq::gpu::vulkan::GpuBuiltIndex built{};
    built.slots = runtime.create_device_buffer(64u, usage);
    built.positions = runtime.create_device_buffer(64u, usage);
    if (!built.slots.mapped || !built.positions.mapped) {
        throw std::runtime_error("test GPU buffers are not host visible");
    }
    std::memset(
        built.slots.mapped, 0,
        static_cast<std::size_t>(built.slots.allocation_bytes));
    std::memset(
        built.positions.mapped, 0,
        static_cast<std::size_t>(built.positions.allocation_bytes));
    const std::uint64_t resident_bytes =
        built.slots.allocation_bytes + built.positions.allocation_bytes;
    auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
        .try_reserve(resident_bytes);
    if (!permit) {
        throw std::runtime_error("test GPU ownership permit was denied");
    }
    built.permit = std::move(*permit);
    built.valid = true;
    built.target_len = 64u;
    built.occupied_slots = empty ? 0u : 2u;
    built.position_count = empty ? 0u : 2u;
    built.slot_mask = 0u;
    built.peak_admission_bytes = resident_bytes;
    built.observed_device_peak_bytes = resident_bytes;
    if (gate_enabled) {
        auto gate_words = std::make_shared<std::vector<std::uint32_t>>(
            bioseq::gpu::vulkan::kPhaseCQueryGateWordCount, 0u);
        (*gate_words)[0] = 1u;
        built.gate_identity =
            bioseq::gpu::vulkan::GpuBuiltIndexGateIdentity{
                .enabled = true,
                .word_count =
                    bioseq::gpu::vulkan::kPhaseCQueryGateWordCount,
                .mask = bioseq::gpu::vulkan::kPhaseCQueryGateMask,
                .digest_lo = 11u,
                .digest_hi = 12u,
                .canonical_words = std::move(gate_words),
            };
    }
    built.materialization = bioseq::parallelism::MemoryMaterializationCommit{
        .reflected_delta_bytes = 0u,
        .admitted_bytes = resident_bytes,
        .evidence = bioseq::parallelism::MemoryMaterializationEvidence{
            .actual_allocation_bytes = resident_bytes,
            .fully_touched_bytes = resident_bytes,
            .host_visible = true,
            .host_coherent = true,
            .process_footprint_reflected = false,
        },
        .permit_released = false,
    };
    auto& proof = built.ownership;
    proof.count_phase_admissions = 1u;
    proof.main_peak_admissions = 1u;
    proof.exact_count_dispatches = 1u;
    proof.exact_count_prefix_scan_dispatches = 3u;
    proof.exact_count_control_readbacks = 1u;
    proof.deterministic_tile_emit_dispatches = empty ? 0u : 1u;
    proof.atomic_append_dispatches = 0u;
    proof.emit_validation_control_readbacks = empty ? 0u : 1u;
    proof.counted_minimizer_count = built.position_count;
    proof.verified_emit_count = built.position_count;
    proof.count_emit_parity_verified = 1u;
    proof.target_minimizer_capacity_estimate = 4u;
    proof.capacity_estimate_underestimated = 0u;
    proof.idle_arena_trim_attempts_before_count = 1u;
    proof.idle_arena_trim_successes_before_count = 1u;
    proof.direct_sequence_arena_copies = 1u;
    proof.direct_tile_arena_fills = 1u;
    proof.host_tile_staging_elided_bytes = 24u;
    proof.count_phase_host_transient_bytes = 0u;
    proof.main_phase_host_transient_bytes = 0u;
    proof.host_transient_excluded_before_main_bytes = 24u;
    proof.arena_trimmed_before_permit_shrink = 1u;
    proof.resident_bytes = resident_bytes;
    proof.gate_count_dispatches = gate_enabled ? 1u : 0u;
    proof.gated_emit_dispatches = gate_enabled && !empty ? 1u : 0u;
    proof.ungated_emit_dispatches = !gate_enabled && !empty ? 1u : 0u;
    proof.query_gate_enabled = gate_enabled ? 1u : 0u;
    proof.query_gate_word_count = gate_enabled
        ? bioseq::gpu::vulkan::kPhaseCQueryGateWordCount : 0u;
    proof.query_gate_mask = gate_enabled
        ? bioseq::gpu::vulkan::kPhaseCQueryGateMask : 0u;
    proof.query_gate_digest_lo = gate_enabled ? 11u : 0u;
    proof.query_gate_digest_hi = gate_enabled ? 12u : 0u;
    proof.retained_minimizer_count = built.position_count;
    proof.distinct_minimizer_count = built.occupied_slots;
    proof.peak_admitted_bytes = resident_bytes;
    proof.peak_observed_bytes = resident_bytes;
    proof.materialization_admitted_bytes = resident_bytes;
    proof.materialization_actual_allocation_bytes = resident_bytes;
    proof.materialization_fully_touched_bytes = resident_bytes;
    proof.materialization_host_visible = 1u;
    proof.materialization_host_coherent = 1u;
    return built;
}

static void refresh_persisted_gpu_fixture_head(PersistedGpuFixture& fixture) {
    const auto encoded_manifest =
        idx_ns::encode_minimizer_generation_manifest(fixture.manifest);
    if (!encoded_manifest) {
        throw std::runtime_error("failed to encode persisted GPU fixture manifest");
    }
    fixture.head = idx_ns::MinimizerGenerationHead{
        .wire_format = fixture.manifest.wire_format,
        .generation_id = fixture.manifest.generation_id,
        .source_identity = fixture.manifest.source_identity,
        .compatibility_checksum =
            idx_ns::minimizer_generation_compatibility_checksum(fixture.manifest),
        .manifest_checksum = idx_ns::crc64_ecma182(
            std::span<const std::byte>{*encoded_manifest}),
        .source_content_checksum = fixture.manifest.source_content_checksum,
    };
}

[[nodiscard]] static PersistedGpuFixture make_persisted_gpu_fixture() {
    PersistedGpuFixture fixture;
    const auto profile = idx_ns::minimizer_generation_payload_profile(
        idx_ns::MinimizerGenerationWireFormat::sorted_dense_global64,
        idx_ns::kLegacyMinimapV1SchemeIdentity,
        15u);
    if (!profile) {
        throw std::runtime_error("failed to derive persisted GPU fixture profile");
    }
    fixture.controls[0] = {
        idx_ns::GpuMinimizerSlot{.hash = 0x111u, .pos_offset = 0u, .pos_count = 1u},
        idx_ns::GpuMinimizerSlot{.hash = 0x222u, .pos_offset = 1u, .pos_count = 2u},
    };
    fixture.positions[0] = {10u, 20u, 22u};
    fixture.controls[1] = {
        idx_ns::GpuMinimizerSlot{.hash = 0x333u, .pos_offset = 0u, .pos_count = 1u},
        idx_ns::GpuMinimizerSlot{.hash = 0x444u, .pos_offset = 1u, .pos_count = 2u},
    };
    fixture.positions[1] = {30u, 40u, 42u};
    fixture.controls[2] = fixture.controls[0];
    fixture.positions[2] = {2'010u, 2'020u, 2'022u};
    fixture.controls[3] = fixture.controls[1];
    fixture.positions[3] = {2'030u, 2'040u, 2'042u};
    for (std::size_t index = 0; index < fixture.controls.size(); ++index) {
        fixture.control_bytes[index] = object_bytes(fixture.controls[index]);
        fixture.position_bytes[index] = object_bytes(fixture.positions[index]);
        fixture.entries[index] = idx_ns::MinimizerBucketDirEntry{
            .partition_id = static_cast<std::uint32_t>(index),
            .position_segment = static_cast<std::int32_t>(index / 2u),
            .lo_hash = fixture.controls[index].front().hash,
            .hi_hash = fixture.controls[index].back().hash,
            .sub_key = static_cast<idx_ns::index_sub_key_t>(100 + index),
            .control_first_chunk = 0u,
            .control_chunk_count = 1u,
            .position_first_chunk = 0u,
            .position_chunk_count = 1u,
            .control_element_count = fixture.controls[index].size(),
            .position_element_count = fixture.positions[index].size(),
            .control_encoded_bytes = fixture.control_bytes[index].size(),
            .position_encoded_bytes = fixture.position_bytes[index].size(),
            .control_checksum = idx_ns::crc64_ecma182(
                fixture.control_bytes[index]),
            .position_checksum = idx_ns::crc64_ecma182(
                fixture.position_bytes[index]),
        };
    }
    auto encoded_directory = idx_ns::encode_minimizer_bucket_directory(
        std::span<const idx_ns::MinimizerBucketDirEntry>{fixture.entries},
        *profile);
    if (!encoded_directory) {
        throw std::runtime_error("failed to encode persisted GPU fixture directory");
    }
    auto directory = idx_ns::DirView::decode(*encoded_directory, *profile);
    if (!directory) {
        throw std::runtime_error("failed to decode persisted GPU fixture directory");
    }
    fixture.directory = std::move(*directory);

    fixture.manifest = idx_ns::MinimizerGenerationManifest{
        .wire_format =
            idx_ns::MinimizerGenerationWireFormat::sorted_dense_global64,
        .generation_id = 77u,
        .source_identity = 0x5152535455565758ULL,
        .source_content_checksum = 0x6162636465666768ULL,
        .target_length = 2'000u,
        .control_count = 8u,
        .position_count = 12u,
        .payload_bytes = 8u * sizeof(idx_ns::GpuMinimizerSlot)
            + 12u * sizeof(std::uint64_t),
        .payload_checksum = 0u,
        .directory_bytes = fixture.directory.encoded_bytes().size(),
        .directory_checksum = idx_ns::crc64_ecma182(
            fixture.directory.encoded_bytes()),
        .position_segment_count = 2u,
        .extent_count = 8u,
        .directory_chunk_count = 1u,
        .alphabet_code = 1u,
        .storage_unit_byte_width = 8u,
        .kmer_size = 15u,
        .window_size = 10u,
        .gpu_abi_version = idx_ns::kMinimizerSortedDenseGpuAbiVersion,
        .position_segment_span_bp = 1'000u,
        .segment_geometry_version = idx_ns::kMinimizerSegmentGeometryVersion,
        .minimizer_scheme_id = idx_ns::kLegacyMinimapV1SchemeId,
        .minimizer_scheme_version = idx_ns::kLegacyMinimapV1SchemeVersion,
        .producer_backend_id = idx_ns::MinimizerProducerBackendId::scalar,
        .producer_backend_identity_version =
            idx_ns::kMinimizerProducerBackendIdentityVersion,
    };
    refresh_persisted_gpu_fixture_head(fixture);
    return fixture;
}

[[nodiscard]] static PersistedGpuFixture make_empty_persisted_gpu_fixture() {
    PersistedGpuFixture fixture;
    const auto profile = idx_ns::minimizer_generation_payload_profile(
        idx_ns::MinimizerGenerationWireFormat::sorted_dense_global64,
        idx_ns::kLegacyMinimapV1SchemeIdentity,
        15u);
    if (!profile) {
        throw std::runtime_error(
            "failed to derive empty persisted GPU fixture profile");
    }
    const std::span<const std::byte> no_directory_bytes{};
    auto directory = idx_ns::DirView::decode(no_directory_bytes, *profile);
    if (!directory) {
        throw std::runtime_error("failed to decode empty persisted GPU directory");
    }
    fixture.directory = std::move(*directory);
    fixture.manifest = idx_ns::MinimizerGenerationManifest{
        .wire_format =
            idx_ns::MinimizerGenerationWireFormat::sorted_dense_global64,
        .generation_id = 88u,
        .source_identity = 0x7172737475767778ULL,
        .source_content_checksum = 0x8182838485868788ULL,
        .target_length = 0u,
        .control_count = 0u,
        .position_count = 0u,
        .payload_bytes = 0u,
        .payload_checksum = 0u,
        .directory_bytes = 0u,
        .directory_checksum = idx_ns::crc64_ecma182(no_directory_bytes),
        .position_segment_count = 0u,
        .extent_count = 0u,
        .directory_chunk_count = 0u,
        .alphabet_code = 1u,
        .storage_unit_byte_width = 8u,
        .kmer_size = 15u,
        .window_size = 10u,
        .gpu_abi_version = idx_ns::kMinimizerSortedDenseGpuAbiVersion,
        .position_segment_span_bp = 0u,
        .segment_geometry_version = idx_ns::kMinimizerSegmentGeometryVersion,
        .minimizer_scheme_id = idx_ns::kLegacyMinimapV1SchemeId,
        .minimizer_scheme_version = idx_ns::kLegacyMinimapV1SchemeVersion,
        .producer_backend_id = idx_ns::MinimizerProducerBackendId::scalar,
        .producer_backend_identity_version =
            idx_ns::kMinimizerProducerBackendIdentityVersion,
    };
    refresh_persisted_gpu_fixture_head(fixture);
    return fixture;
}

[[nodiscard]] constexpr idx_ns::ExpectedMinimizerCompatibility
persisted_gpu_expected_compatibility() noexcept {
    return {
        .wire_format =
            idx_ns::MinimizerGenerationWireFormat::sorted_dense_global64,
        .alphabet_code = 1u,
        .storage_unit_byte_width = 8u,
        .kmer_size = 15u,
        .window_size = 10u,
        .gpu_abi_version = idx_ns::kMinimizerSortedDenseGpuAbiVersion,
        .minimizer_scheme_id = idx_ns::kLegacyMinimapV1SchemeId,
        .minimizer_scheme_version = idx_ns::kLegacyMinimapV1SchemeVersion,
        .producer_backend_id = idx_ns::MinimizerProducerBackendId::scalar,
        .producer_backend_identity_version =
            idx_ns::kMinimizerProducerBackendIdentityVersion,
    };
}

struct Grouped32GpuStreamFixture {
    std::array<std::vector<idx_ns::MinimizerGrouped32Control>, 4> controls;
    std::array<std::vector<std::uint32_t>, 4> positions;
    std::array<std::vector<std::byte>, 4> control_bytes;
    std::array<std::vector<std::byte>, 4> position_bytes;
    std::array<idx_ns::MinimizerBucketDirEntry, 4> entries;
    idx_ns::MinimizerGenerationHead head;
    idx_ns::MinimizerGenerationManifest manifest;
    idx_ns::DirView directory;
};

[[nodiscard]] constexpr idx_ns::ExpectedMinimizerCompatibility
grouped32_gpu_expected_compatibility() noexcept {
    return {
        .wire_format = idx_ns::MinimizerGenerationWireFormat::
            grouped_hash32_segment_local32,
        .alphabet_code = 1u,
        .storage_unit_byte_width = 8u,
        .kmer_size = 15u,
        .window_size = 10u,
        .gpu_abi_version = idx_ns::kMinimizerGrouped32GpuAbiVersion,
        .minimizer_scheme_id = idx_ns::kLegacyMinimapV1SchemeId,
        .minimizer_scheme_version = idx_ns::kLegacyMinimapV1SchemeVersion,
        .producer_backend_id = idx_ns::MinimizerProducerBackendId::exact_simd,
        .producer_backend_identity_version =
            idx_ns::kMinimizerProducerBackendIdentityVersion,
    };
}

[[nodiscard]] static Grouped32GpuStreamFixture
make_grouped32_gpu_stream_fixture() {
    Grouped32GpuStreamFixture fixture;
    const auto profile = idx_ns::minimizer_generation_payload_profile(
        idx_ns::MinimizerGenerationWireFormat::
            grouped_hash32_segment_local32,
        idx_ns::kLegacyMinimapV1SchemeIdentity, 15u);
    if (!profile) {
        throw std::runtime_error("failed to derive grouped32 GPU profile");
    }
    fixture.controls = {{
        {{.hash = 0x111u, .position_offset = 0u, .position_count = 1u},
         {.hash = 0x222u, .position_offset = 1u, .position_count = 2u}},
        {{.hash = 0x333u, .position_offset = 0u, .position_count = 4u}},
        {{.hash = 0x111u, .position_offset = 0u, .position_count = 2u},
         {.hash = 0x555u, .position_offset = 2u, .position_count = 1u}},
        {{.hash = 0x777u, .position_offset = 0u, .position_count = 1u}},
    }};
    fixture.positions = {{
        {20u, 40u, 44u},
        {60u, 62u, 64u, 66u},
        {20u, 24u, 80u},
        {100u},
    }};
    std::uint64_t control_count = 0u;
    std::uint64_t position_count = 0u;
    std::uint64_t payload_bytes = 0u;
    for (std::size_t index = 0u; index < fixture.entries.size(); ++index) {
        for (const auto& control : fixture.controls[index]) {
            const auto encoded = idx_ns::encode_minimizer_grouped32_control(
                control, *profile);
            if (!encoded) {
                throw std::runtime_error("failed to encode grouped32 control");
            }
            fixture.control_bytes[index].insert(
                fixture.control_bytes[index].end(),
                encoded->begin(), encoded->end());
        }
        for (const std::uint32_t position : fixture.positions[index]) {
            const auto encoded = idx_ns::encode_minimizer_grouped32_position(
                position, *profile);
            if (!encoded) {
                throw std::runtime_error("failed to encode grouped32 position");
            }
            fixture.position_bytes[index].insert(
                fixture.position_bytes[index].end(),
                encoded->begin(), encoded->end());
        }
        fixture.entries[index] = idx_ns::MinimizerBucketDirEntry{
            .partition_id = static_cast<std::uint32_t>(index),
            .position_segment = static_cast<std::int32_t>(index / 2u),
            .lo_hash = fixture.controls[index].front().hash,
            .hi_hash = fixture.controls[index].back().hash,
            .sub_key = static_cast<idx_ns::index_sub_key_t>(300 + index),
            .control_first_chunk = 0u,
            .control_chunk_count = 1u,
            .position_first_chunk = 0u,
            .position_chunk_count = 1u,
            .control_element_count = fixture.controls[index].size(),
            .position_element_count = fixture.positions[index].size(),
            .control_encoded_bytes = fixture.control_bytes[index].size(),
            .position_encoded_bytes = fixture.position_bytes[index].size(),
            .control_checksum = idx_ns::crc64_ecma182(
                fixture.control_bytes[index]),
            .position_checksum = idx_ns::crc64_ecma182(
                fixture.position_bytes[index]),
        };
        control_count += fixture.controls[index].size();
        position_count += fixture.positions[index].size();
        payload_bytes += fixture.control_bytes[index].size()
            + fixture.position_bytes[index].size();
    }
    const auto encoded_directory = idx_ns::encode_minimizer_bucket_directory(
        std::span<const idx_ns::MinimizerBucketDirEntry>{fixture.entries},
        *profile);
    if (!encoded_directory) {
        throw std::runtime_error("failed to encode grouped32 GPU directory");
    }
    auto directory = idx_ns::DirView::decode(*encoded_directory, *profile);
    if (!directory) {
        throw std::runtime_error("failed to decode grouped32 GPU directory");
    }
    fixture.directory = std::move(*directory);
    fixture.manifest = idx_ns::MinimizerGenerationManifest{
        .wire_format = idx_ns::MinimizerGenerationWireFormat::
            grouped_hash32_segment_local32,
        .generation_id = 404u,
        .source_identity = 0x4142434445464748ULL,
        .source_content_checksum = 0x5152535455565758ULL,
        .target_length = 2'000u,
        .control_count = control_count,
        .position_count = position_count,
        .payload_bytes = payload_bytes,
        .payload_checksum = 0u,
        .directory_bytes = fixture.directory.encoded_bytes().size(),
        .directory_checksum = idx_ns::crc64_ecma182(
            fixture.directory.encoded_bytes()),
        .position_segment_count = 2u,
        .extent_count = 8u,
        .directory_chunk_count = 1u,
        .alphabet_code = 1u,
        .storage_unit_byte_width = 8u,
        .kmer_size = 15u,
        .window_size = 10u,
        .gpu_abi_version = idx_ns::kMinimizerGrouped32GpuAbiVersion,
        .position_segment_span_bp = 1'000u,
        .segment_geometry_version = idx_ns::kMinimizerSegmentGeometryVersion,
        .minimizer_scheme_id = idx_ns::kLegacyMinimapV1SchemeId,
        .minimizer_scheme_version = idx_ns::kLegacyMinimapV1SchemeVersion,
        .producer_backend_id = idx_ns::MinimizerProducerBackendId::exact_simd,
        .producer_backend_identity_version =
            idx_ns::kMinimizerProducerBackendIdentityVersion,
    };
    const auto encoded_manifest =
        idx_ns::encode_minimizer_generation_manifest(fixture.manifest);
    if (!encoded_manifest) {
        throw std::runtime_error("failed to encode grouped32 manifest");
    }
    fixture.head = idx_ns::MinimizerGenerationHead{
        .wire_format = fixture.manifest.wire_format,
        .generation_id = fixture.manifest.generation_id,
        .source_identity = fixture.manifest.source_identity,
        .compatibility_checksum =
            idx_ns::minimizer_generation_compatibility_checksum(
                fixture.manifest),
        .manifest_checksum = idx_ns::crc64_ecma182(
            std::span<const std::byte>{*encoded_manifest}),
        .source_content_checksum = fixture.manifest.source_content_checksum,
    };
    return fixture;
}

template <class UInt>
requires (std::is_unsigned_v<UInt>)
static void store_test_le(
    std::span<std::byte> bytes,
    std::size_t offset,
    UInt value)
{
    if (offset > bytes.size() || sizeof(UInt) > bytes.size() - offset) {
        throw std::runtime_error("Format-5 fixture scalar write overflow");
    }
    for (std::size_t index = 0u; index < sizeof(UInt); ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            (value >> (index * 8u)) & static_cast<UInt>(0xffu));
    }
}

struct ShapeV5GpuResidentFixture {
    std::array<std::vector<std::byte>, 2> control_bytes;
    std::array<std::vector<std::byte>, 2> position_bytes;
    std::array<idx_ns::MinimizerBucketDirEntry, 2> entries;
    idx_ns::MinimizerGenerationHead head;
    idx_ns::MinimizerGenerationManifest manifest;
    idx_ns::DirView directory;
};

[[nodiscard]] constexpr idx_ns::ExpectedMinimizerCompatibility
shape_v5_gpu_expected_compatibility() noexcept {
    return {
        .wire_format = idx_ns::MinimizerGenerationWireFormat::
            grouped_shape_soa_segment_local,
        .alphabet_code = idx_ns::kMinimizerPersistedDna4AlphabetCode,
        .storage_unit_byte_width = 8u,
        .kmer_size = 15u,
        .window_size = 10u,
        .gpu_abi_version = idx_ns::kMinimizerShapeV5GpuAbiVersion,
        .minimizer_scheme_id = idx_ns::kLegacyMinimapV1SchemeId,
        .minimizer_scheme_version = idx_ns::kLegacyMinimapV1SchemeVersion,
        .producer_backend_id = idx_ns::MinimizerProducerBackendId::exact_simd,
        .producer_backend_identity_version =
            idx_ns::kMinimizerProducerBackendIdentityVersion,
        .max_occ_cap = 10u,
    };
}

[[nodiscard]] static ShapeV5GpuResidentFixture
make_shape_v5_gpu_resident_fixture() {
    ShapeV5GpuResidentFixture fixture;
    constexpr std::uint64_t generation_id = 505u;
    const auto profile = idx_ns::minimizer_generation_payload_profile(
        idx_ns::MinimizerGenerationWireFormat::
            grouped_shape_soa_segment_local,
        idx_ns::kLegacyMinimapV1SchemeIdentity,
        idx_ns::kMinimizerPersistedDna4AlphabetCode,
        15u, 32u, 10u);
    if (!profile || !profile->shape_v5) {
        throw std::runtime_error("failed to derive Format-5 GPU profile");
    }

    const std::array<std::vector<std::uint32_t>, 2> hashes{{
        {1u, 2u}, {3u},
    }};
    const std::array<std::vector<std::uint16_t>, 2> prefixes{{
        {0u, 2u, 3u}, {0u, 1u},
    }};
    const std::array<std::vector<std::uint16_t>, 2> counts{{
        {2u, 1u}, {1u},
    }};
    const std::array<std::vector<std::uint16_t>, 2> positions{{
        {7u, 18u, 23u}, {10u},
    }};

    std::uint64_t control_count = 0u;
    std::uint64_t position_count = 0u;
    std::uint64_t payload_bytes = 0u;
    for (std::size_t ordinal = 0u; ordinal < fixture.entries.size(); ++ordinal) {
        const auto layout = idx_ns::minimizer_shape_v5_control_layout(
            hashes[ordinal].size(),
            idx_ns::MinimizerScalarWidth::u32,
            idx_ns::MinimizerScalarWidth::u16,
            idx_ns::MinimizerScalarWidth::u16);
        if (!layout) {
            throw std::runtime_error("failed to derive Format-5 control layout");
        }
        auto& controls = fixture.control_bytes[ordinal];
        controls.resize(static_cast<std::size_t>(layout->encoded_bytes));
        for (std::size_t index = 0u; index < hashes[ordinal].size(); ++index) {
            store_test_le<std::uint32_t>(
                controls, index * sizeof(std::uint32_t),
                hashes[ordinal][index]);
        }
        for (std::size_t index = 0u; index < prefixes[ordinal].size(); ++index) {
            store_test_le<std::uint16_t>(
                controls,
                static_cast<std::size_t>(layout->prefix_offset)
                    + index * sizeof(std::uint16_t),
                prefixes[ordinal][index]);
        }
        for (std::size_t index = 0u; index < counts[ordinal].size(); ++index) {
            store_test_le<std::uint16_t>(
                controls,
                static_cast<std::size_t>(layout->count_offset)
                    + index * sizeof(std::uint16_t),
                counts[ordinal][index]);
        }
        auto& encoded_positions = fixture.position_bytes[ordinal];
        encoded_positions.resize(
            positions[ordinal].size() * sizeof(std::uint16_t));
        for (std::size_t index = 0u; index < positions[ordinal].size(); ++index) {
            store_test_le<std::uint16_t>(
                encoded_positions, index * sizeof(std::uint16_t),
                positions[ordinal][index]);
        }
        fixture.entries[ordinal] = idx_ns::MinimizerBucketDirEntry{
            .partition_id = static_cast<std::uint32_t>(ordinal),
            .position_segment = static_cast<std::int32_t>(ordinal),
            .lo_hash = hashes[ordinal].front(),
            .hi_hash = hashes[ordinal].back(),
            .sub_key = static_cast<idx_ns::index_sub_key_t>(ordinal),
            .control_first_chunk = 0u,
            .control_chunk_count = 1u,
            .position_first_chunk = 0u,
            .position_chunk_count = 1u,
            .control_element_count = hashes[ordinal].size(),
            .position_element_count = positions[ordinal].size(),
            .control_encoded_bytes = controls.size(),
            .position_encoded_bytes = encoded_positions.size(),
            .control_checksum = idx_ns::crc64_ecma182(controls),
            .position_checksum = idx_ns::crc64_ecma182(encoded_positions),
            .shape_offset_width = idx_ns::MinimizerScalarWidth::u16,
        };
        control_count += hashes[ordinal].size();
        position_count += positions[ordinal].size();
        payload_bytes += controls.size() + encoded_positions.size();
    }

    const auto encoded_directory = idx_ns::encode_minimizer_bucket_directory(
        std::span<const idx_ns::MinimizerBucketDirEntry>{fixture.entries},
        *profile);
    if (!encoded_directory) {
        throw std::runtime_error("failed to encode Format-5 GPU directory");
    }
    auto directory = idx_ns::DirView::decode(*encoded_directory, *profile);
    if (!directory) {
        throw std::runtime_error("failed to decode Format-5 GPU directory");
    }
    fixture.directory = std::move(*directory);

    idx_ns::Crc64Ecma182 payload_row_checksums{};
    for (std::size_t ordinal = 0u; ordinal < fixture.entries.size(); ++ordinal) {
        const auto& entry = fixture.entries[ordinal];
        payload_row_checksums.update_little_endian(
            idx_ns::crc64_index_chunk_row(
                generation_id, entry.position_segment, entry.sub_key, 0u,
                static_cast<std::uint32_t>(
                    fixture.control_bytes[ordinal].size()),
                fixture.control_bytes[ordinal]));
    }
    for (std::size_t ordinal = 0u; ordinal < fixture.entries.size(); ++ordinal) {
        const auto& entry = fixture.entries[ordinal];
        const auto position_sub_key =
            idx_ns::derive_minimizer_position_extent_sub_key(entry.sub_key);
        if (!position_sub_key) {
            throw std::runtime_error("failed to derive Format-5 position key");
        }
        payload_row_checksums.update_little_endian(
            idx_ns::crc64_index_chunk_row(
                generation_id, entry.position_segment, *position_sub_key, 0u,
                static_cast<std::uint32_t>(
                    fixture.position_bytes[ordinal].size()),
                fixture.position_bytes[ordinal]));
    }

    fixture.manifest = idx_ns::MinimizerGenerationManifest{
        .wire_format = idx_ns::MinimizerGenerationWireFormat::
            grouped_shape_soa_segment_local,
        .generation_id = generation_id,
        .source_identity = 0x6162636465666768ULL,
        .source_content_checksum = 0x7172737475767778ULL,
        .target_length = 64u,
        .control_count = control_count,
        .position_count = position_count,
        .payload_bytes = payload_bytes,
        .payload_checksum = payload_row_checksums.checksum(),
        .directory_bytes = fixture.directory.encoded_bytes().size(),
        .directory_checksum = idx_ns::crc64_ecma182(
            fixture.directory.encoded_bytes()),
        .position_segment_count = 2u,
        .extent_count = 4u,
        .directory_chunk_count = 1u,
        .alphabet_code = idx_ns::kMinimizerPersistedDna4AlphabetCode,
        .storage_unit_byte_width = 8u,
        .kmer_size = 15u,
        .window_size = 10u,
        .gpu_abi_version = idx_ns::kMinimizerShapeV5GpuAbiVersion,
        .position_segment_span_bp = 32u,
        .segment_geometry_version = idx_ns::kMinimizerSegmentGeometryVersion,
        .minimizer_scheme_id = idx_ns::kLegacyMinimapV1SchemeId,
        .minimizer_scheme_version = idx_ns::kLegacyMinimapV1SchemeVersion,
        .producer_backend_id = idx_ns::MinimizerProducerBackendId::exact_simd,
        .producer_backend_identity_version =
            idx_ns::kMinimizerProducerBackendIdentityVersion,
        .shape_v5 = profile->shape_v5,
    };
    const auto encoded_manifest =
        idx_ns::encode_minimizer_generation_manifest_v5(fixture.manifest);
    if (!encoded_manifest) {
        throw std::runtime_error("failed to encode Format-5 GPU manifest");
    }
    fixture.head = idx_ns::MinimizerGenerationHead{
        .wire_format = fixture.manifest.wire_format,
        .generation_id = fixture.manifest.generation_id,
        .source_identity = fixture.manifest.source_identity,
        .compatibility_checksum =
            idx_ns::minimizer_generation_compatibility_checksum(
                fixture.manifest),
        .manifest_checksum = idx_ns::crc64_ecma182(
            std::span<const std::byte>{*encoded_manifest}),
        .source_content_checksum = fixture.manifest.source_content_checksum,
    };
    return fixture;
}

[[nodiscard]] static idx_ns::GpuDeviceUuid synthetic_device_uuid(
    std::uint8_t seed) noexcept
{
    idx_ns::GpuDeviceUuid uuid{};
    for (std::size_t index = 0; index < uuid.size(); ++index) {
        uuid[index] = static_cast<std::uint8_t>(seed + index);
    }
    return uuid;
}

[[nodiscard]] static std::span<const std::uint64_t> host_resident_lookup(
    const idx_ns::GpuResidentSortedDensePartition& resident,
    std::uint64_t hash)
{
    const auto* controls = static_cast<const idx_ns::GpuMinimizerSlot*>(
        resident.controls().mapped);
    const auto* positions = static_cast<const std::uint64_t*>(
        resident.positions().mapped);
    const auto* found = std::lower_bound(
        controls, controls + resident.control_count(), hash,
        [](const idx_ns::GpuMinimizerSlot& control, std::uint64_t wanted) {
            return control.hash < wanted;
        });
    if (found == controls + resident.control_count() || found->hash != hash) {
        return {};
    }
    return std::span<const std::uint64_t>{
        positions + found->pos_offset, found->pos_count};
}

template <class Cache, class Resident>
concept publicly_inserts_resident = requires(
    Cache& cache,
    std::shared_ptr<Resident> resident,
    idx_ns::GpuPersistedIndexLocalProof& proof) {
    cache.insert_resident(std::move(resident), proof);
};

TEST_CASE("vulkan index: GPU slot table resolves the same hits as CPU lookup",
          "[vulkan][index][phase-b]") {
    const auto idx = build_demo_index();
    const auto upload = idx_ns::build_gpu_minimizer_upload(idx);

    idx.for_each_table_entry([&](std::uint64_t kmer, std::uint64_t) {
        const auto cpu_hits = idx.lookup(kmer);  // span of encoded positions
        const std::uint32_t slot_idx = host_find_slot(upload, kmer);
        ASSERT(slot_idx != 0xffffffffu,
               fmt::format("GPU slot table missed kmer {}", kmer), kmer);
        const auto& slot = upload.slots[slot_idx];
        ASSERT(slot.pos_count == cpu_hits.size(),
               fmt::format("kmer {} count mismatch (gpu {} vs cpu {})",
                           kmer, slot.pos_count, cpu_hits.size()),
               slot.pos_count, cpu_hits.size());
        for (std::uint32_t j = 0; j < slot.pos_count; ++j) {
            ASSERT(upload.positions[slot.pos_offset + j] == cpu_hits[j],
                   fmt::format("kmer {} position {} mismatch", kmer, j), j);
        }
    });
    // A kmer not in the index must miss.
    ASSERT(host_find_slot(upload, 0xDEADBEEFULL) == 0xffffffffu,
           "absent kmer unexpectedly found in slot table", 0);
}

TEST_CASE("vulkan index: XXL upper-bound plan is 2^27 slots at <=70% load",
          "[vulkan][index][phase-b][capacity]") {
    constexpr std::uint64_t positions = 93'308'896u;
    constexpr std::uint64_t expected_capacity = std::uint64_t{1} << 27;
    const std::uint64_t capacity = idx_ns::gpu_slot_capacity(positions);
    REQUIRE(capacity == expected_capacity);
    REQUIRE(idx_ns::gpu_index_load_ppm(positions, capacity) <= 700'000u);
    REQUIRE(capacity * sizeof(idx_ns::GpuMinimizerSlot) == 2'147'483'648u);
    REQUIRE(positions * sizeof(std::uint64_t) == 746'471'168u);
    REQUIRE(capacity * sizeof(idx_ns::GpuMinimizerSlot)
            + positions * sizeof(std::uint64_t) == 2'893'954'816u);

    constexpr std::uint64_t max_distinct_for_2p27 =
        ((std::uint64_t{1} << 27) * 7u) / 10u;
    REQUIRE(idx_ns::gpu_slot_capacity(max_distinct_for_2p27)
            == expected_capacity);
    REQUIRE(idx_ns::gpu_slot_capacity(max_distinct_for_2p27 + 1u)
            == (std::uint64_t{1} << 28));
}

TEST_CASE("vulkan index: storage-range preflight accepts equality and rejects +1/unknown",
          "[vulkan][index][phase-b][limits]") {
    constexpr std::uint64_t limit = 4'294'967'295u;
    REQUIRE(idx_ns::gpu_index_storage_ranges_fit(limit, 8u, limit));
    REQUIRE_FALSE(idx_ns::gpu_index_storage_ranges_fit(limit + 1u, 8u, limit));
    REQUIRE_FALSE(idx_ns::gpu_index_storage_ranges_fit(16u, limit + 1u, limit));
    REQUIRE_FALSE(idx_ns::gpu_index_storage_ranges_fit(16u, 8u, 0u));
}

TEST_CASE("vulkan index: direct fill preserves collision and wraparound parity",
          "[vulkan][index][phase-b][direct-fill]") {
    std::vector<std::uint64_t> colliding_hashes;
    for (std::uint64_t hash = 1u; colliding_hashes.size() < 3u; ++hash) {
        if ((idx_ns::splitmix64(hash) & 7u) == 7u) {
            colliding_hashes.push_back(hash);
        }
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs{
        {colliding_hashes[0], 11u},
        {colliding_hashes[1], 22u},
        {colliding_hashes[2], 33u},
    };
    MinimizerHashmapFragmentIndex idx;
    idx.build_from_pairs(pairs, 100u);
    const std::uint32_t capacity = idx_ns::gpu_slot_capacity(idx.table_size());
    REQUIRE(capacity == 8u);
    std::vector<idx_ns::GpuMinimizerSlot> final_slots(capacity);
    std::vector<std::uint64_t> final_positions(idx.positions.size());
    const auto stats = idx_ns::direct_fill_gpu_minimizer_index(
        idx, final_slots, final_positions);
    REQUIRE(stats.unique_key_count == 3u);
    REQUIRE(stats.slot_capacity == 8u);
    REQUIRE(stats.load_ppm <= 700'000u);
    REQUIRE(stats.max_probe == 3u);
    REQUIRE(final_positions == idx.positions);
    for (const std::uint64_t hash : colliding_hashes) {
        const std::uint32_t slot = host_find_slot(
            std::span<const idx_ns::GpuMinimizerSlot>{final_slots},
            capacity - 1u, hash);
        REQUIRE(slot != 0xffffffffu);
        const auto cpu = idx.lookup(hash);
        REQUIRE(final_slots[slot].pos_count == cpu.size());
        REQUIRE(final_positions[final_slots[slot].pos_offset] == cpu.front());
    }
    REQUIRE(final_slots[7].pos_count != 0u);
    REQUIRE(final_slots[0].pos_count != 0u);
    REQUIRE(final_slots[1].pos_count != 0u);
    REQUIRE(idx_ns::gpu_index_upload_peak_bytes(idx) == 0u);
}

TEST_CASE("vulkan index: sorted-dense direct fill preserves shader lookup parity",
          "[vulkan][index][phase-b][direct-fill][sorted-dense]") {
    auto idx = build_demo_index();
    std::vector<idx_ns::GpuMinimizerSlot> final_slots(idx.table_size());
    std::vector<std::uint64_t> final_positions(idx.positions.size());
    const auto stats = idx_ns::direct_fill_gpu_minimizer_index_sorted_dense(
        idx, final_slots, final_positions);

    REQUIRE(stats.unique_key_count == idx.table_size());
    REQUIRE(stats.slot_capacity == idx.table_size());
    REQUIRE(stats.load_ppm == 1'000'000u);
    REQUIRE(stats.max_probe == 0u);
    REQUIRE(final_positions == idx.positions);
    REQUIRE(std::is_sorted(
        final_slots.begin(), final_slots.end(),
        [](const auto& lhs, const auto& rhs) {
            return lhs.hash < rhs.hash;
        }));
    for (const std::uint64_t hash : {0x111ULL, 0x222ULL, 0x333ULL, 0x444ULL}) {
        const std::uint32_t slot = host_find_sorted_dense_slot(
            final_slots, hash);
        REQUIRE(slot != 0xffffffffu);
        const auto cpu = idx.lookup(hash);
        REQUIRE(final_slots[slot].pos_count == cpu.size());
        REQUIRE(std::equal(
            cpu.begin(), cpu.end(),
            final_positions.begin() + final_slots[slot].pos_offset));
    }
    REQUIRE(host_find_sorted_dense_slot(final_slots, 0x999ULL)
            == 0xffffffffu);
}

TEST_CASE("vulkan index: compact plan removes the observed XXL power-of-two cliff",
          "[vulkan][index][phase-b][planning][sorted-dense]") {
    constexpr std::uint64_t unique_keys = 70'249'364u;
    constexpr std::uint64_t positions = 93'308'896u;
    const auto open = idx_ns::plan_gpu_index_materialization(
        unique_keys, positions,
        idx_ns::GpuIndexRepresentation::open_addressed_70);
    const auto dense = idx_ns::plan_gpu_index_materialization(
        unique_keys, positions,
        idx_ns::GpuIndexRepresentation::sorted_dense);

    REQUIRE(open.slot_capacity == 134'217'728u);
    REQUIRE(open.slot_bytes == 2'147'483'648u);
    REQUIRE(open.position_bytes == 746'471'168u);
    REQUIRE(open.resident_bytes == 2'893'954'816u);
    REQUIRE(dense.slot_capacity == unique_keys);
    REQUIRE(dense.slot_bytes == 1'123'989'824u);
    REQUIRE(dense.position_bytes == open.position_bytes);
    REQUIRE(dense.resident_bytes == 1'870'460'992u);
    REQUIRE(dense.resident_bytes < open.resident_bytes);
    REQUIRE(dense.load_ppm == 1'000'000u);
}

TEST_CASE("vulkan index: in-memory cache key is pointer-stable and content-keyed",
          "[vulkan][index][phase-b]") {
    auto idx_a = build_demo_index();
    auto idx_b = build_demo_index();  // identical content, different object

    const auto key_a = idx_ns::make_gpu_index_cache_key(idx_a, idx_ns::GpuIndexCacheIdentity{});
    const auto key_a2 = idx_ns::make_gpu_index_cache_key(idx_a, idx_ns::GpuIndexCacheIdentity{});
    const auto key_b = idx_ns::make_gpu_index_cache_key(idx_b, idx_ns::GpuIndexCacheIdentity{});

    ASSERT(key_a == key_a2, "same in-memory index must produce the same key", 0);
    ASSERT(!(key_a == key_b),
           "distinct in-memory index objects must produce distinct keys", 0);
}

TEST_CASE("vulkan index: distinct assembly identities do not share a cache key",
          "[vulkan][index][phase-b]") {
    // Two identical local segment indexes with different add_position_offset:
    // they must carry distinct explicit assembly_identity values and key apart.
    auto seg_lo = build_demo_index();
    auto seg_hi = build_demo_index();
    seg_hi.add_position_offset(1'000'000);  // global offset for a later segment

    const auto key_lo = idx_ns::make_gpu_index_cache_key(
        seg_lo, idx_ns::GpuIndexCacheIdentity{.assembly_identity = 0xA11CEULL});
    const auto key_hi = idx_ns::make_gpu_index_cache_key(
        seg_hi, idx_ns::GpuIndexCacheIdentity{.assembly_identity = 0xB0B0ULL});

    ASSERT(!(key_lo == key_hi),
           "different assembly identities must not share a cache key", 0);
    // The same identity on the same index must be reproducible.
    const auto key_lo2 = idx_ns::make_gpu_index_cache_key(
        seg_lo, idx_ns::GpuIndexCacheIdentity{.assembly_identity = 0xA11CEULL});
    ASSERT(key_lo == key_lo2, "assembly key must be reproducible", 0);

    idx_ns::GpuMinimizerIndexCacheKeyHash hasher;
    ASSERT(hasher(key_lo) != hasher(key_hi) || !(key_lo == key_hi),
           "distinct assembly keys collide on both equality and hash", 0);
}

TEST_CASE("vulkan index: query-gate cache identity compares canonical words after digest",
          "[vulkan][index][phase-c][gate]") {
    using Cache = idx_ns::GpuMinimizerIndexCache;
    Cache cache{std::uint64_t{64} << 20};
    auto words_a_mut = std::make_shared<std::vector<std::uint32_t>>(
        bioseq::gpu::vulkan::kPhaseCQueryGateWordCount, 0u);
    (*words_a_mut)[0] = 0x1u;
    (*words_a_mut)[1] = 0x20u;
    auto words_b_mut =
        std::make_shared<std::vector<std::uint32_t>>(*words_a_mut);
    (*words_b_mut)[2] = 0x301u;
    const std::shared_ptr<const std::vector<std::uint32_t>> words_a =
        std::move(words_a_mut);
    const std::shared_ptr<const std::vector<std::uint32_t>> words_b =
        std::move(words_b_mut);
    const Cache::GateIdentity first{
        .enabled = true,
        .word_count = bioseq::gpu::vulkan::kPhaseCQueryGateWordCount,
        .mask = bioseq::gpu::vulkan::kPhaseCQueryGateMask,
        .digest_lo = 0x1111222233334444ULL,
        .digest_hi = 0x5555666677778888ULL,
        .canonical_words = words_a,
    };
    auto forced_collision = first;
    forced_collision.canonical_words = words_b;
    REQUIRE(cache.bind_gate_once(first).has_value());
    const auto rejected = cache.bind_gate_once(forced_collision);
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error() == Cache::GateBindError::identity_mismatch);

    Cache zero_cache{std::uint64_t{64} << 20};
    const auto zero_words =
        std::make_shared<const std::vector<std::uint32_t>>(
            bioseq::gpu::vulkan::kPhaseCQueryGateWordCount, 0u);
    REQUIRE(zero_cache.bind_gate_once(Cache::GateIdentity{
        .enabled = true,
        .word_count = bioseq::gpu::vulkan::kPhaseCQueryGateWordCount,
        .mask = bioseq::gpu::vulkan::kPhaseCQueryGateMask,
        .digest_lo = 9u,
        .digest_hi = 10u,
        .canonical_words = zero_words,
    }).has_value());
    const auto disabled = zero_cache.bind_gate_once(Cache::GateIdentity{});
    REQUIRE_FALSE(disabled.has_value());
    REQUIRE(disabled.error() == Cache::GateBindError::identity_mismatch);
}

TEST_CASE("vulkan index: every cache API rejects an unbound gated key",
          "[vulkan][index][phase-c][gate]") {
    using Cache = idx_ns::GpuMinimizerIndexCache;
    Cache cache{std::uint64_t{64} << 20};
    idx_ns::GpuMinimizerIndexCacheKey gated_key{};
    gated_key.gate_enabled = true;
    gated_key.gate_word_count = 1u;
    gated_key.gate_mask = 31u;
    gated_key.gate_digest_lo = 11u;
    gated_key.gate_digest_hi = 12u;

    auto& runtime = ::bioseq::gpu::vulkan::Runtime::instance();
    auto idx = build_demo_index();
    REQUIRE_THROWS_AS(
        cache.get_or_upload(
            runtime, idx, gated_key, /*cache_enabled=*/false),
        bioseq::gpu::vulkan::VulkanRuntimeError);
    REQUIRE_THROWS_AS(
        cache.insert_upload(
            runtime, idx_ns::GpuMinimizerIndexUpload{}, gated_key,
            /*cache_enabled=*/false),
        bioseq::gpu::vulkan::VulkanRuntimeError);
    auto empty_handle = std::make_shared<idx_ns::GpuResidentMinimizerIndex>();
    const auto built_insert = cache.insert_built(gated_key, empty_handle);
    REQUIRE_FALSE(built_insert.has_value());
    REQUIRE(built_insert.error()
            == Cache::BuiltInsertError::gate_identity_mismatch);

    const auto truncated_words =
        std::make_shared<const std::vector<std::uint32_t>>(1u, 1u);
    const auto malformed = cache.bind_gate_once(Cache::GateIdentity{
        .enabled = true,
        .word_count = 1u,
        .mask = 31u,
        .digest_lo = 1u,
        .digest_hi = 2u,
        .canonical_words = truncated_words,
    });
    REQUIRE_FALSE(malformed.has_value());
    REQUIRE(malformed.error() == Cache::GateBindError::invalid_identity);
}

TEST_CASE("vulkan index: typed upload decline preserves every predicate and admission field",
          "[vulkan][index][phase-b][decline]") {
    const bioseq::parallelism::MemoryAdmissionFailure admission{
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
    };
    for (const auto reason : {
            idx_ns::GpuIndexUploadDeclineReason::
                resident_memory_admission_denied,
            idx_ns::GpuIndexUploadDeclineReason::
                upload_peak_memory_admission_denied}) {
        const idx_ns::GpuIndexUploadDecline decline{
            .reason = reason,
            .resident_bytes = 101u,
            .upload_peak_bytes = 202u,
            .slot_bytes = 303u,
            .position_bytes = 404u,
            .unique_key_count = 505u,
            .slot_capacity = 606u,
            .staging_bytes = 0u,
            .actual_allocation_bytes = 707u,
            .fully_touched_bytes = 707u,
            .reflected_footprint_delta_bytes = 808u,
            .load_ppm = 699'999u,
            .max_probe = 9u,
            .materialization_permit_released = true,
            .process_footprint_reflected = true,
            .admission = admission,
        };
        const std::string formatted =
            idx_ns::format_gpu_index_upload_decline(decline);
        REQUIRE(formatted.find(idx_ns::gpu_index_upload_decline_reason_name(reason))
                != std::string::npos);
        REQUIRE(formatted.find("admission_reason=budget_exhausted")
                != std::string::npos);
        REQUIRE(formatted.find("required_bytes=701") != std::string::npos);
        REQUIRE(formatted.find("reserved_bytes=211") != std::string::npos);
        REQUIRE(formatted.find("budget_ceiling_bytes=503") != std::string::npos);
        REQUIRE(formatted.find("process_footprint_bytes=401")
                != std::string::npos);
        REQUIRE(formatted.find("system_available_bytes=601")
                != std::string::npos);
        REQUIRE(formatted.find("system_total_bytes=809") != std::string::npos);
        REQUIRE(formatted.find("process_soft_cap_bytes=307")
                != std::string::npos);
        REQUIRE(formatted.find("resident_bytes=101") != std::string::npos);
        REQUIRE(formatted.find("upload_peak_bytes=202") != std::string::npos);
        REQUIRE(formatted.find("slot_bytes=303") != std::string::npos);
        REQUIRE(formatted.find("position_bytes=404") != std::string::npos);
        REQUIRE(formatted.find("unique_key_count=505") != std::string::npos);
        REQUIRE(formatted.find("slot_capacity=606") != std::string::npos);
        REQUIRE(formatted.find("load_ppm=699999") != std::string::npos);
        REQUIRE(formatted.find("max_probe=9") != std::string::npos);
        REQUIRE(formatted.find("representation=open_addressed_70")
                != std::string::npos);
        REQUIRE(formatted.find("staging_bytes=0") != std::string::npos);
        REQUIRE(formatted.find("actual_allocation_bytes=707")
                != std::string::npos);
        REQUIRE(formatted.find("fully_touched_bytes=707")
                != std::string::npos);
        REQUIRE(formatted.find("reflected_footprint_delta_bytes=808")
                != std::string::npos);
        REQUIRE(formatted.find("materialization_permit_released=true")
                != std::string::npos);
        REQUIRE(formatted.find("process_footprint_reflected=true")
                != std::string::npos);
    }

    for (const auto reason : {
            idx_ns::GpuIndexUploadDeclineReason::device_abi_overflow,
            idx_ns::GpuIndexUploadDeclineReason::
                non_host_visible_device_memory}) {
        const idx_ns::GpuIndexUploadDecline decline{
            .reason = reason,
            .resident_bytes = 11u,
            .upload_peak_bytes = 22u,
            .slot_bytes = 33u,
            .position_bytes = 44u,
        };
        const std::string formatted =
            idx_ns::format_gpu_index_upload_decline(decline);
        REQUIRE(formatted.find(idx_ns::gpu_index_upload_decline_reason_name(reason))
                != std::string::npos);
        REQUIRE(formatted.find("slot_bytes=33") != std::string::npos);
        REQUIRE(formatted.find("position_bytes=44") != std::string::npos);
    }
}

TEST_CASE("vulkan index: resident upload + cache reuse (device path)",
          "[vulkan][index][phase-b]") {
    auto& runtime = ::bioseq::gpu::vulkan::Runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    auto idx = build_demo_index();
    idx_ns::GpuMinimizerIndexCache cache{std::uint64_t{64} << 20};
    const auto key = idx_ns::make_gpu_index_cache_key(idx, idx_ns::GpuIndexCacheIdentity{});

    idx_ns::GpuIndexUploadDecline diagnostic{};
    auto handle1 = cache.get_or_upload(
        runtime, idx, key, /*cache_enabled=*/true, &diagnostic);
    ASSERT(handle1 != nullptr, "resident upload returned nullptr on a usable device", 0);
    ASSERT(handle1->target_len == idx.target_len, "resident target_len mismatch", 0);
    ASSERT(handle1->position_count == idx.positions.size(),
           "resident position_count mismatch", 0);
    REQUIRE(diagnostic.reason == idx_ns::GpuIndexUploadDeclineReason::none);
    REQUIRE(diagnostic.unique_key_count == idx.table_size());
    REQUIRE(diagnostic.slot_capacity == idx_ns::gpu_slot_capacity(idx.table_size()));
    REQUIRE(diagnostic.load_ppm <= 700'000u);
    REQUIRE(diagnostic.max_probe == handle1->max_probe);
    REQUIRE(diagnostic.staging_bytes == 0u);
    REQUIRE(diagnostic.upload_peak_bytes == 0u);
    REQUIRE(handle1->representation
            == idx_ns::GpuIndexRepresentation::open_addressed_70);
    REQUIRE(handle1->unique_key_count == idx.table_size());
    REQUIRE(handle1->slot_capacity == diagnostic.slot_capacity);
    REQUIRE(handle1->materialization.admitted_bytes
            == handle1->slots.allocation_bytes
                + handle1->positions.allocation_bytes);
    REQUIRE(handle1->materialization.evidence.actual_allocation_bytes
            == handle1->materialization.admitted_bytes);
    REQUIRE(diagnostic.actual_allocation_bytes
            == handle1->materialization.evidence.actual_allocation_bytes);
    REQUIRE(diagnostic.fully_touched_bytes
            == handle1->materialization.evidence.fully_touched_bytes);
    REQUIRE(diagnostic.reflected_footprint_delta_bytes
            == handle1->materialization.reflected_delta_bytes);
    REQUIRE(diagnostic.materialization_permit_released
            == handle1->materialization.permit_released);
    REQUIRE(diagnostic.process_footprint_reflected
            == handle1->materialization.evidence.process_footprint_reflected);
    if (handle1->materialization.permit_released) {
        REQUIRE(handle1->permit.bytes() == 0u);
        REQUIRE(handle1->materialization.reflected_delta_bytes
                >= handle1->materialization.admitted_bytes);
    } else {
        REQUIRE(handle1->permit.bytes()
                == handle1->materialization.admitted_bytes);
    }

    idx_ns::GpuIndexUploadDecline cache_hit_diagnostic{};
    auto handle2 = cache.get_or_upload(
        runtime, idx, key, /*cache_enabled=*/true, &cache_hit_diagnostic);
    ASSERT(handle2.get() == handle1.get(),
           "cache must return the same resident handle for a repeated key", 0);
    REQUIRE(cache_hit_diagnostic.reason
            == idx_ns::GpuIndexUploadDeclineReason::none);
    REQUIRE(cache_hit_diagnostic.slot_capacity == 0u);
    REQUIRE(cache_hit_diagnostic.actual_allocation_bytes == 0u);

    cache.clear();
}

TEST_CASE("vulkan index: admission cliff selects and attests sorted-dense device layout",
          "[vulkan][index][phase-b][admission][sorted-dense]") {
    auto& runtime = ::bioseq::gpu::vulkan::Runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    constexpr std::uint64_t distinct = 100'000u;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
    pairs.reserve(distinct);
    for (std::uint64_t i = 0u; i < distinct; ++i) {
        pairs.emplace_back(i * 0x9e3779b97f4a7c15ULL, i << 1u);
    }
    MinimizerHashmapFragmentIndex idx;
    idx.build_from_pairs(pairs, 1'000'000u);
    const auto open = idx_ns::plan_gpu_index_materialization(
        idx.table_size(), idx.positions.size(),
        idx_ns::GpuIndexRepresentation::open_addressed_70);
    const auto dense = idx_ns::plan_gpu_index_materialization(
        idx.table_size(), idx.positions.size(),
        idx_ns::GpuIndexRepresentation::sorted_dense);
    REQUIRE(dense.resident_bytes + (std::uint64_t{1} << 20)
            < open.resident_bytes);

    IndexMemoryCapScope compact_only_cap{
        dense.resident_bytes + (std::uint64_t{1} << 20)};
    idx_ns::GpuMinimizerIndexCache cache{std::uint64_t{16} << 20};
    const auto key = idx_ns::make_gpu_index_cache_key(
        idx, idx_ns::GpuIndexCacheIdentity{});
    idx_ns::GpuIndexUploadDecline diagnostic{};
    auto handle = cache.get_or_upload(
        runtime, idx, key, /*cache_enabled=*/true, &diagnostic);

    REQUIRE(handle != nullptr);
    REQUIRE(diagnostic.reason == idx_ns::GpuIndexUploadDeclineReason::none);
    REQUIRE(diagnostic.representation
            == idx_ns::GpuIndexRepresentation::sorted_dense);
    REQUIRE(diagnostic.resident_bytes == dense.resident_bytes);
    REQUIRE(diagnostic.slot_capacity == distinct);
    REQUIRE(diagnostic.load_ppm == 1'000'000u);
    REQUIRE(diagnostic.max_probe == 0u);
    REQUIRE(diagnostic.staging_bytes == 0u);
    REQUIRE(handle->representation
            == idx_ns::GpuIndexRepresentation::sorted_dense);
    REQUIRE(handle->slot_mask == 0u);
    REQUIRE(handle->bucket_count == distinct);
    REQUIRE(handle->unique_key_count == distinct);
    REQUIRE(handle->slot_capacity == distinct);

    const auto mapped_slots = std::span<const idx_ns::GpuMinimizerSlot>{
        static_cast<const idx_ns::GpuMinimizerSlot*>(handle->slots.mapped),
        static_cast<std::size_t>(handle->bucket_count)};
    for (const std::uint64_t ordinal : {0u, 1u, 50'000u, 99'999u}) {
        const std::uint64_t hash = ordinal * 0x9e3779b97f4a7c15ULL;
        const std::uint32_t slot = host_find_sorted_dense_slot(
            mapped_slots, hash);
        REQUIRE(slot != 0xffffffffu);
        REQUIRE(mapped_slots[slot].pos_count == 1u);
    }
    cache.clear();
}

TEST_CASE("vulkan index: GPU-built adoption transfers retained ownership without re-reserve",
          "[vulkan][index][phase-c][admission][ownership]") {
    auto& runtime = ::bioseq::gpu::vulkan::Runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto& admission = bioseq::parallelism::MemoryAdmissionController::instance();
    const std::uint64_t before = admission.reserved_bytes();
    auto built = make_adoptable_gpu_built_index(
        runtime, /*gate_enabled=*/false);
    const std::uint64_t with_built = admission.reserved_bytes();
    REQUIRE(with_built > before);
    IndexMemoryCapScope tiny_cap{1u};
    auto adopted = idx_ns::adopt_gpu_built_index(
        runtime, std::move(built), idx_ns::GpuMinimizerIndexCacheKey{});
    REQUIRE(adopted.has_value());
    REQUIRE((*adopted)->ownership.resident_transfers == 1u);
    REQUIRE((*adopted)->ownership.adoption_rereserve_attempts == 0u);
    REQUIRE((*adopted)->ownership.adoption_successes == 1u);
    REQUIRE(admission.reserved_bytes() == with_built);
    REQUIRE_FALSE(built.valid);
    REQUIRE_FALSE(built.slots.valid());
    REQUIRE_FALSE(built.positions.valid());
    adopted->reset();
    REQUIRE(admission.reserved_bytes() == before);
}

TEST_CASE("vulkan index: GPU-built move assignment releases destination after buffers",
          "[vulkan][index][phase-c][ownership]") {
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<
        bioseq::gpu::vulkan::GpuBuiltIndex>);
    STATIC_REQUIRE(std::is_nothrow_move_assignable_v<
        bioseq::gpu::vulkan::GpuBuiltIndex>);
    auto& runtime = ::bioseq::gpu::vulkan::Runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto& admission = bioseq::parallelism::MemoryAdmissionController::instance();
    const std::uint64_t before = admission.reserved_bytes();
    {
        auto destination = make_adoptable_gpu_built_index(
            runtime, /*gate_enabled=*/false);
        const std::uint64_t one_resident = destination.ownership.resident_bytes;
        auto source = make_adoptable_gpu_built_index(
            runtime, /*gate_enabled=*/false);
        REQUIRE(admission.reserved_bytes() == before + 2u * one_resident);
        destination = std::move(source);
        REQUIRE(destination.valid);
        REQUIRE_FALSE(source.valid);
        REQUIRE_FALSE(source.slots.valid());
        REQUIRE_FALSE(source.positions.valid());
        REQUIRE_FALSE(static_cast<bool>(source.permit));
        REQUIRE(admission.reserved_bytes() == before + one_resident);
    }
    REQUIRE(admission.reserved_bytes() == before);
}

TEST_CASE("vulkan index: adoption rejects inconsistent evidence without consuming source",
          "[vulkan][index][phase-c][ownership]") {
    auto& runtime = ::bioseq::gpu::vulkan::Runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto built = make_adoptable_gpu_built_index(
        runtime, /*gate_enabled=*/false);
    built.materialization.evidence.fully_touched_bytes--;
    built.ownership.materialization_fully_touched_bytes--;
    auto adopted = idx_ns::adopt_gpu_built_index(
        runtime, std::move(built), idx_ns::GpuMinimizerIndexCacheKey{});
    REQUIRE_FALSE(adopted.has_value());
    REQUIRE(adopted.error()
            == idx_ns::GpuBuiltIndexAdoptionError::evidence_mismatch);
    REQUIRE(built.valid);
    REQUIRE(built.slots.valid());
    REQUIRE(built.positions.valid());
    REQUIRE(static_cast<bool>(built.permit));

    built.materialization.evidence.fully_touched_bytes++;
    built.ownership.materialization_fully_touched_bytes++;
    built.occupied_slots = built.position_count + 1u;
    built.ownership.distinct_minimizer_count = built.occupied_slots;
    adopted = idx_ns::adopt_gpu_built_index(
        runtime, std::move(built), idx_ns::GpuMinimizerIndexCacheKey{});
    REQUIRE_FALSE(adopted.has_value());
    REQUIRE(adopted.error() == idx_ns::GpuBuiltIndexAdoptionError::invalid_build);
    REQUIRE(built.valid);
    REQUIRE(built.slots.valid());
    REQUIRE(built.positions.valid());

    built.occupied_slots = built.position_count;
    built.ownership.distinct_minimizer_count = built.occupied_slots;
    built.target_len = built.position_count - 1u;
    adopted = idx_ns::adopt_gpu_built_index(
        runtime, std::move(built), idx_ns::GpuMinimizerIndexCacheKey{});
    REQUIRE_FALSE(adopted.has_value());
    REQUIRE(adopted.error() == idx_ns::GpuBuiltIndexAdoptionError::invalid_build);
    REQUIRE(built.valid);
}

TEST_CASE("vulkan index: adoption rejects tampered deterministic target-build proof",
          "[vulkan][index][phase-c][ownership][tamper]") {
    auto& runtime = ::bioseq::gpu::vulkan::Runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    constexpr std::uint32_t proof_field_count = 12u;
    for (std::uint32_t field = 0u; field < proof_field_count; ++field) {
        auto built = make_adoptable_gpu_built_index(
            runtime, /*gate_enabled=*/false);
        switch (field) {
            case 0u: built.ownership.exact_count_dispatches = 0u; break;
            case 1u:
                built.ownership.exact_count_prefix_scan_dispatches = 2u;
                break;
            case 2u:
                built.ownership.exact_count_control_readbacks = 0u;
                break;
            case 3u:
                built.ownership.deterministic_tile_emit_dispatches = 0u;
                break;
            case 4u: built.ownership.atomic_append_dispatches = 1u; break;
            case 5u:
                built.ownership.emit_validation_control_readbacks = 0u;
                break;
            case 6u: built.ownership.counted_minimizer_count = 1u; break;
            case 7u: built.ownership.verified_emit_count = 1u; break;
            case 8u: built.ownership.count_emit_parity_verified = 0u; break;
            case 9u:
                built.ownership.arena_trimmed_before_permit_shrink = 0u;
                break;
            case 10u:
                built.ownership.target_minimizer_capacity_estimate = 0u;
                break;
            case 11u:
                built.ownership.capacity_estimate_underestimated = 1u;
                break;
            default: FAIL("unreachable proof field");
        }
        INFO("tampered proof field ordinal=" << field);
        auto adopted = idx_ns::adopt_gpu_built_index(
            runtime, std::move(built), idx_ns::GpuMinimizerIndexCacheKey{});
        REQUIRE_FALSE(adopted.has_value());
        REQUIRE(adopted.error()
                == idx_ns::GpuBuiltIndexAdoptionError::evidence_mismatch);
        REQUIRE(built.valid);
        REQUIRE(built.slots.valid());
        REQUIRE(built.positions.valid());
    }
}

TEST_CASE("vulkan index: ungated T-zero sentinel adopts without an emit dispatch",
          "[vulkan][index][phase-c][ownership][empty]") {
    auto& runtime = ::bioseq::gpu::vulkan::Runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto built = make_adoptable_gpu_built_index(
        runtime, /*gate_enabled=*/false, /*empty=*/true);
    REQUIRE(built.ownership.ungated_emit_dispatches == 0u);
    REQUIRE(built.ownership.deterministic_tile_emit_dispatches == 0u);
    auto adopted = idx_ns::adopt_gpu_built_index(
        runtime, std::move(built), idx_ns::GpuMinimizerIndexCacheKey{});
    REQUIRE(adopted.has_value());
    REQUIRE((*adopted)->position_count == 0u);
    REQUIRE((*adopted)->bucket_count == 0u);
    REQUIRE((*adopted)->ownership.counted_minimizer_count == 0u);
    REQUIRE((*adopted)->ownership.verified_emit_count == 0u);
}

TEST_CASE("vulkan index: released materialization and gated T-zero sentinel adopt",
          "[vulkan][index][phase-c][ownership]") {
    auto& runtime = ::bioseq::gpu::vulkan::Runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto built = make_adoptable_gpu_built_index(
        runtime, /*gate_enabled=*/true, /*empty=*/true);
    const auto bytes = built.ownership.resident_bytes;
    built.permit.release();
    built.materialization.permit_released = true;
    built.materialization.evidence.process_footprint_reflected = true;
    built.materialization.reflected_delta_bytes = bytes;
    built.ownership.materialization_permit_released = 1u;
    built.ownership.materialization_process_footprint_reflected = 1u;
    built.ownership.materialization_reflected_delta_bytes = bytes;
    const auto gate_identity = built.gate_identity;
    idx_ns::GpuMinimizerIndexCacheKey gated_key{};
    gated_key.gate_enabled = true;
    gated_key.gate_word_count = gate_identity.word_count;
    gated_key.gate_mask = gate_identity.mask;
    gated_key.gate_digest_lo = 11u;
    gated_key.gate_digest_hi = 12u;
    gated_key.gate_canonical_words = gate_identity.canonical_words;
    auto adopted = idx_ns::adopt_gpu_built_index(
        runtime, std::move(built), gated_key);
    REQUIRE(adopted.has_value());
    REQUIRE((*adopted)->position_count == 0u);
    REQUIRE((*adopted)->bucket_count == 0u);
    REQUIRE((*adopted)->ownership.gate_count_dispatches == 1u);
    REQUIRE((*adopted)->ownership.gated_emit_dispatches == 0u);
    REQUIRE((*adopted)->ownership.ungated_emit_dispatches == 0u);
}

TEST_CASE("vulkan index: Gate-A build cannot adopt under forced-collision Gate-B key",
          "[vulkan][index][phase-c][gate][ownership]") {
    auto& runtime = ::bioseq::gpu::vulkan::Runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto built = make_adoptable_gpu_built_index(
        runtime, /*gate_enabled=*/true);
    auto colliding_words = std::make_shared<std::vector<std::uint32_t>>(
        *built.gate_identity.canonical_words);
    (*colliding_words)[7] ^= 1u;
    idx_ns::GpuMinimizerIndexCacheKey colliding_key{
        .gate_enabled = true,
        .gate_word_count = built.gate_identity.word_count,
        .gate_mask = built.gate_identity.mask,
        .gate_digest_lo = built.gate_identity.digest_lo,
        .gate_digest_hi = built.gate_identity.digest_hi,
        .gate_canonical_words = std::move(colliding_words),
    };
    auto adopted = idx_ns::adopt_gpu_built_index(
        runtime, std::move(built), colliding_key);
    REQUIRE_FALSE(adopted.has_value());
    REQUIRE(adopted.error()
            == idx_ns::GpuBuiltIndexAdoptionError::evidence_mismatch);
    REQUIRE(built.valid);
    REQUIRE(built.slots.valid());
    REQUIRE(built.positions.valid());
    REQUIRE(static_cast<bool>(built.permit));
}

TEST_CASE("vulkan index: Gate-B-bound cache rejects adopted Gate-A resident on digest collision",
          "[vulkan][index][phase-c][gate][cache]") {
    auto& runtime = ::bioseq::gpu::vulkan::Runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto built = make_adoptable_gpu_built_index(
        runtime, /*gate_enabled=*/true);
    const auto gate_a = built.gate_identity;
    idx_ns::GpuMinimizerIndexCacheKey key_a{
        .gate_enabled = true,
        .gate_word_count = gate_a.word_count,
        .gate_mask = gate_a.mask,
        .gate_digest_lo = gate_a.digest_lo,
        .gate_digest_hi = gate_a.digest_hi,
        .gate_canonical_words = gate_a.canonical_words,
    };
    auto adopted = idx_ns::adopt_gpu_built_index(
        runtime, std::move(built), key_a);
    REQUIRE(adopted.has_value());
    auto resident_a = std::move(*adopted);

    auto words_b_mut = std::make_shared<std::vector<std::uint32_t>>(
        *gate_a.canonical_words);
    (*words_b_mut)[9] ^= 1u;
    const std::shared_ptr<const std::vector<std::uint32_t>> words_b =
        std::move(words_b_mut);
    using Cache = idx_ns::GpuMinimizerIndexCache;
    Cache cache{std::uint64_t{64} << 20};
    REQUIRE(cache.bind_gate_once(Cache::GateIdentity{
        .enabled = true,
        .word_count = gate_a.word_count,
        .mask = gate_a.mask,
        .digest_lo = gate_a.digest_lo,
        .digest_hi = gate_a.digest_hi,
        .canonical_words = words_b,
    }).has_value());
    auto key_b = key_a;
    key_b.gate_canonical_words = words_b;
    const auto inserted = cache.insert_built(key_b, resident_a);
    REQUIRE_FALSE(inserted.has_value());
    REQUIRE(inserted.error() == Cache::BuiltInsertError::gate_identity_mismatch);
}

TEST_CASE("vulkan index: cache cannot publish a resident under another target key",
          "[vulkan][index][phase-c][cache][ownership]") {
    auto& runtime = ::bioseq::gpu::vulkan::Runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto built = make_adoptable_gpu_built_index(
        runtime, /*gate_enabled=*/false);
    idx_ns::GpuMinimizerIndexCacheKey key_a{.source_id = 101u};
    auto adopted = idx_ns::adopt_gpu_built_index(
        runtime, std::move(built), key_a);
    REQUIRE(adopted.has_value());
    auto resident = std::move(*adopted);
    auto key_b = key_a;
    key_b.source_id = 202u;
    idx_ns::GpuMinimizerIndexCache cache{std::uint64_t{64} << 20};
    const auto inserted = cache.insert_built(key_b, resident);
    REQUIRE_FALSE(inserted.has_value());
    REQUIRE(inserted.error()
            == idx_ns::GpuMinimizerIndexCache::BuiltInsertError::
                resident_key_mismatch);
}

TEST_CASE("DOC0371 persisted GPU key invalidates every immutable identity axis",
          "[vulkan][index][persisted][identity]") {
    const auto uuid = synthetic_device_uuid(3u);
    const idx_ns::GpuPersistedMinimizerIndexCacheKey base{
        .generation_id = 99u,
        .source_identity = 0xabcdefu,
        .position_segment = 7,
        .sub_key = 123,
        .device_uuid = uuid,
        .sorted_dense_abi_version = idx_ns::kMinimizerSortedDenseGpuAbiVersion,
    };
    REQUIRE(idx_ns::valid_gpu_persisted_cache_key(base));

    std::array<idx_ns::GpuPersistedMinimizerIndexCacheKey, 6> changed{};
    changed.fill(base);
    changed[0].generation_id++;
    changed[1].source_identity++;
    changed[2].position_segment++;
    changed[3].sub_key++;
    changed[4].device_uuid[0] ^= 0xffu;
    changed[5].sorted_dense_abi_version++;
    const idx_ns::GpuPersistedMinimizerIndexCacheKeyHash hash{};
    for (const auto& candidate : changed) {
        REQUIRE_FALSE(candidate == base);
        INFO("base hash=" << hash(base) << " candidate hash=" << hash(candidate));
    }

    auto unbound = base;
    unbound.device_uuid = {};
    REQUIRE_FALSE(idx_ns::valid_gpu_persisted_cache_key(unbound));
    auto bad_abi = base;
    bad_abi.sorted_dense_abi_version++;
    REQUIRE_FALSE(idx_ns::valid_gpu_persisted_cache_key(bad_abi));
}

TEST_CASE("DOC0371 resident construction and publication have no public bypass",
          "[vulkan][index][persisted][publication]") {
    STATIC_REQUIRE_FALSE(std::default_initializable<
        idx_ns::GpuResidentSortedDensePartition>);
    STATIC_REQUIRE_FALSE(std::default_initializable<
        idx_ns::GpuResidentAuthoritativeDirectory>);
    STATIC_REQUIRE_FALSE(publicly_inserts_resident<
        idx_ns::GpuPersistedMinimizerIndexCache,
        idx_ns::GpuResidentSortedDensePartition>);
}

TEST_CASE("DOC0371 persisted GPU local proof counters saturate fail-closed",
          "[vulkan][index][persisted][proof]") {
    idx_ns::GpuPersistedIndexLocalProof proof{};
    proof.streamed_control_bytes =
        (std::numeric_limits<std::uint64_t>::max)() - 1u;
    idx_ns::add_gpu_persisted_proof_counter(
        proof, proof.streamed_control_bytes, 2u);
    REQUIRE(proof.streamed_control_bytes
            == (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE(proof.saturated);
    REQUIRE_FALSE(proof.has_forbidden_work());
}

TEST_CASE("DOC0371 device selects authoritative directory ordinals without host hashes",
          "[vulkan][index][persisted][directory][gpu]") {
    auto& runtime = ::bioseq::gpu::vulkan::Runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto fixture = make_persisted_gpu_fixture();
    idx_ns::GpuPersistedIndexLocalProof proof{};
    auto directory = idx_ns::upload_authoritative_gpu_directory(
        runtime, fixture.head, fixture.manifest,
        persisted_gpu_expected_compatibility(), fixture.directory, proof);
    REQUIRE(directory.has_value());
    REQUIRE(proof.authoritative_directory_uploads == 1u);
    REQUIRE(proof.authoritative_directory_bytes
            == fixture.directory.encoded_bytes().size());

    const std::vector<MinimapSketchData> query{
        MinimapSketchData{0x222u, 1u, 0u},
        MinimapSketchData{0x444u, 2u, 1u},
        MinimapSketchData{0x999u, 3u, 0u},
        MinimapSketchData{0x222u, 4u, 1u},
    };
    using query_traits =
        bioseq::gpu::vulkan::minimizer_wire_traits<MinimapSketchData>;
    const auto query_bytes = std::as_bytes(
        std::span<const MinimapSketchData>{query});
    constexpr VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto query_buffer = runtime.create_device_buffer(query_bytes.size(), usage);
    REQUIRE(query_buffer.mapped != nullptr);
    std::memcpy(query_buffer.mapped, query_bytes.data(), query_bytes.size());

    const auto ordinals = idx_ns::select_gpu_persisted_directory_ordinals(
        runtime,
        idx_ns::GpuDeviceQueryMinimizerView{
            .device_words = &query_buffer,
            .count = static_cast<std::uint32_t>(query.size()),
            .word_kind = static_cast<std::uint32_t>(query_traits::word_kind),
            .words32_per_record = query_traits::words32_per_minimizer,
            .hash_shift = query_traits::hash_shift,
        },
        **directory,
        proof);
    const std::array expected{0u, 1u, 2u, 3u};
    const auto bound_values = ordinals.values_for(**directory);
    REQUIRE(bound_values.has_value());
    REQUIRE(std::ranges::equal(*bound_values, expected));
    std::array<bool, 2> covered_segments{};
    for (std::uint32_t ordinal : *bound_values) {
        const auto* entry = fixture.directory.entry_by_ordinal(ordinal);
        REQUIRE(entry != nullptr);
        covered_segments[static_cast<std::size_t>(entry->position_segment)] = true;
    }
    REQUIRE(covered_segments[0]);
    REQUIRE(covered_segments[1]);
    REQUIRE(proof.device_partition_selection_dispatches == 1u);
    REQUIRE(proof.device_query_minimizers_processed == query.size());
    REQUIRE(proof.opaque_ordinals_read_back == expected.size());
    REQUIRE_FALSE(proof.has_forbidden_work());
    REQUIRE(proof.forbidden_host_query_hash_materializations == 0u);
}

TEST_CASE("DOC0371 nonempty query plus empty directory reports zero device work",
          "[vulkan][index][persisted][directory][zero-work]") {
    auto& runtime = ::bioseq::gpu::vulkan::Runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto fixture = make_empty_persisted_gpu_fixture();
    idx_ns::GpuPersistedIndexLocalProof proof{};
    auto directory = idx_ns::upload_authoritative_gpu_directory(
        runtime, fixture.head, fixture.manifest,
        persisted_gpu_expected_compatibility(), fixture.directory, proof);
    REQUIRE(directory.has_value());

    const std::vector<MinimapSketchData> query{
        MinimapSketchData{0x222u, 1u, 0u}};
    using query_traits =
        bioseq::gpu::vulkan::minimizer_wire_traits<MinimapSketchData>;
    const auto query_bytes = std::as_bytes(
        std::span<const MinimapSketchData>{query});
    constexpr VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto query_buffer = runtime.create_device_buffer(query_bytes.size(), usage);
    REQUIRE(query_buffer.mapped != nullptr);
    std::memcpy(query_buffer.mapped, query_bytes.data(), query_bytes.size());

    const auto selection = idx_ns::select_gpu_persisted_directory_ordinals(
        runtime,
        idx_ns::GpuDeviceQueryMinimizerView{
            .device_words = &query_buffer,
            .count = static_cast<std::uint32_t>(query.size()),
            .word_kind = static_cast<std::uint32_t>(query_traits::word_kind),
            .words32_per_record = query_traits::words32_per_minimizer,
            .hash_shift = query_traits::hash_shift,
        },
        **directory,
        proof);
    const auto values = selection.values_for(**directory);
    REQUIRE(values.has_value());
    REQUIRE(values->empty());
    REQUIRE(proof.device_partition_selection_dispatches == 0u);
    REQUIRE(proof.device_query_minimizers_processed == 0u);
    REQUIRE(proof.opaque_ordinals_read_back == 0u);
    REQUIRE_FALSE(proof.has_forbidden_work());
}

TEST_CASE("DOC0371 resident permit outlives cache until final immutable handle",
          "[vulkan][index][persisted][admission][lifetime]") {
    auto& runtime = ::bioseq::gpu::vulkan::Runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto fixture = make_persisted_gpu_fixture();
    idx_ns::GpuPersistedIndexLocalProof proof{};
    auto resident_directory = idx_ns::upload_authoritative_gpu_directory(
        runtime, fixture.head, fixture.manifest,
        persisted_gpu_expected_compatibility(), fixture.directory, proof);
    REQUIRE(resident_directory.has_value());
    const std::vector<MinimapSketchData> query{
        MinimapSketchData{0x222u, 1u, 0u}};
    using query_traits =
        bioseq::gpu::vulkan::minimizer_wire_traits<MinimapSketchData>;
    const auto query_bytes = std::as_bytes(
        std::span<const MinimapSketchData>{query});
    constexpr VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto query_buffer = runtime.create_device_buffer(query_bytes.size(), usage);
    REQUIRE(query_buffer.mapped != nullptr);
    std::memcpy(query_buffer.mapped, query_bytes.data(), query_bytes.size());
    const auto selection = idx_ns::select_gpu_persisted_directory_ordinals(
        runtime,
        idx_ns::GpuDeviceQueryMinimizerView{
            .device_words = &query_buffer,
            .count = static_cast<std::uint32_t>(query.size()),
            .word_kind = static_cast<std::uint32_t>(query_traits::word_kind),
            .words32_per_record = query_traits::words32_per_minimizer,
            .hash_shift = query_traits::hash_shift,
        },
        **resident_directory,
        proof);

    auto& admission = bioseq::parallelism::MemoryAdmissionController::instance();
    const std::uint64_t before_resident = admission.reserved_bytes();
    std::shared_ptr<const idx_ns::GpuResidentSortedDensePartition> handle;
    {
        idx_ns::GpuPersistedMinimizerIndexCache cache{1u << 20};
        const auto& entry = fixture.entries[0];
        const idx_ns::GpuPersistedMinimizerIndexCacheKey key{
            .generation_id = fixture.manifest.generation_id,
            .source_identity = fixture.manifest.source_identity,
            .position_segment = entry.position_segment,
            .sub_key = entry.sub_key,
            .device_uuid = runtime.features().device_uuid,
            .sorted_dense_abi_version =
                idx_ns::kMinimizerSortedDenseGpuAbiVersion,
        };
        auto stream = idx_ns::GpuSortedDensePartitionStream::begin(
            runtime, key, selection, **resident_directory,
            fixture.directory, fixture.manifest,
            persisted_gpu_expected_compatibility(), entry.partition_id);
        REQUIRE(stream.has_value());
        REQUIRE(stream->accept_control_bytes(
            fixture.control_bytes[0]).has_value());
        REQUIRE(stream->accept_position_bytes(
            fixture.position_bytes[0]).has_value());
        auto finished = stream->finish(cache, proof);
        REQUIRE(finished.has_value());
        handle = *finished;
        REQUIRE(admission.reserved_bytes() > before_resident);
        cache.clear();
        REQUIRE(admission.reserved_bytes() > before_resident);
    }
    REQUIRE(admission.reserved_bytes() > before_resident);
    handle.reset();
    REQUIRE(admission.reserved_bytes() == before_resident);
}

TEST_CASE("DOC0371 streams selected partitions into one exact resident set",
          "[vulkan][index][persisted][stream][resident-set]") {
    auto& runtime = ::bioseq::gpu::vulkan::Runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto fixture = make_persisted_gpu_fixture();
    idx_ns::GpuPersistedIndexLocalProof proof{};
    auto resident_directory = idx_ns::upload_authoritative_gpu_directory(
        runtime, fixture.head, fixture.manifest,
        persisted_gpu_expected_compatibility(), fixture.directory, proof);
    REQUIRE(resident_directory.has_value());
    const std::vector<MinimapSketchData> query{
        MinimapSketchData{0x222u, 1u, 0u},
        MinimapSketchData{0x444u, 2u, 1u},
    };
    using query_traits =
        bioseq::gpu::vulkan::minimizer_wire_traits<MinimapSketchData>;
    const auto query_bytes = std::as_bytes(
        std::span<const MinimapSketchData>{query});
    constexpr VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto query_buffer = runtime.create_device_buffer(query_bytes.size(), usage);
    REQUIRE(query_buffer.mapped != nullptr);
    std::memcpy(query_buffer.mapped, query_bytes.data(), query_bytes.size());
    const auto selection = idx_ns::select_gpu_persisted_directory_ordinals(
        runtime,
        idx_ns::GpuDeviceQueryMinimizerView{
            .device_words = &query_buffer,
            .count = static_cast<std::uint32_t>(query.size()),
            .word_kind = static_cast<std::uint32_t>(query_traits::word_kind),
            .words32_per_record = query_traits::words32_per_minimizer,
            .hash_shift = query_traits::hash_shift,
        },
        **resident_directory,
        proof);
    idx_ns::GpuPersistedMinimizerIndexCache cache{1u << 20};
    std::vector<std::shared_ptr<const idx_ns::GpuResidentSortedDensePartition>>
        residents;

    for (std::size_t index = 0; index < 2u; ++index) {
        const auto& entry = fixture.entries[index];
        const idx_ns::GpuPersistedMinimizerIndexCacheKey key{
            .generation_id = fixture.manifest.generation_id,
            .source_identity = fixture.manifest.source_identity,
            .position_segment = entry.position_segment,
            .sub_key = entry.sub_key,
            .device_uuid = runtime.features().device_uuid,
            .sorted_dense_abi_version =
                idx_ns::kMinimizerSortedDenseGpuAbiVersion,
        };
        auto stream = idx_ns::GpuSortedDensePartitionStream::begin(
            runtime, key, selection, **resident_directory,
            fixture.directory, fixture.manifest,
            persisted_gpu_expected_compatibility(), entry.partition_id);
        REQUIRE(stream.has_value());
        const auto& controls = fixture.control_bytes[index];
        const std::size_t split = controls.size() / 2u;
        REQUIRE(stream->accept_control_bytes(
            std::span<const std::byte>{controls}.first(split)).has_value());
        REQUIRE(stream->accept_control_bytes(
            std::span<const std::byte>{controls}.subspan(split)).has_value());
        REQUIRE(stream->accept_position_bytes(
            fixture.position_bytes[index]).has_value());
        auto resident = stream->finish(cache, proof);
        REQUIRE(resident.has_value());
        REQUIRE(cache.find(key).get() == resident->get());
        residents.push_back(*resident);
    }

    REQUIRE(proof.streamed_resident_partitions == 2u);
    REQUIRE(proof.resident_insertions == 2u);
    REQUIRE(proof.forbidden_host_pair_materializations == 0u);
    REQUIRE(proof.forbidden_host_pair_sorts == 0u);
    REQUIRE(proof.forbidden_host_control_rebases == 0u);
    auto changed_generation = residents.front()->key();
    changed_generation.generation_id++;
    REQUIRE(cache.find(changed_generation) == nullptr);
    const auto& duplicate_entry = fixture.entries[0];
    const idx_ns::GpuPersistedMinimizerIndexCacheKey duplicate_key{
        .generation_id = fixture.manifest.generation_id,
        .source_identity = fixture.manifest.source_identity,
        .position_segment = duplicate_entry.position_segment,
        .sub_key = duplicate_entry.sub_key,
        .device_uuid = runtime.features().device_uuid,
        .sorted_dense_abi_version =
            idx_ns::kMinimizerSortedDenseGpuAbiVersion,
    };
    auto duplicate_stream = idx_ns::GpuSortedDensePartitionStream::begin(
        runtime, duplicate_key, selection, **resident_directory,
        fixture.directory, fixture.manifest,
        persisted_gpu_expected_compatibility(), duplicate_entry.partition_id);
    REQUIRE(duplicate_stream.has_value());
    REQUIRE(duplicate_stream->accept_control_bytes(
        fixture.control_bytes[0]).has_value());
    REQUIRE(duplicate_stream->accept_position_bytes(
        fixture.position_bytes[0]).has_value());
    const auto duplicate_finished = duplicate_stream->finish(cache, proof);
    REQUIRE_FALSE(duplicate_finished.has_value());
    REQUIRE(duplicate_finished.error()
            == idx_ns::GpuPersistedIndexFoundationError::duplicate_publication);
    REQUIRE(proof.resident_insertions == 2u);

    auto resident_set = idx_ns::GpuResidentSortedDensePartitionSet::prepare(
        residents, proof);
    REQUIRE(resident_set.has_value());
    REQUIRE(resident_set->device_views().size() == 2u);

    // Test-only oracle: the two dedicated partitions expose the same hit union
    // as their source controls. Production receives only device buffer views.
    std::vector<std::uint64_t> union_hits;
    for (const auto& resident : residents) {
        const auto hits_222 = host_resident_lookup(*resident, 0x222u);
        union_hits.insert(union_hits.end(), hits_222.begin(), hits_222.end());
        const auto hits_444 = host_resident_lookup(*resident, 0x444u);
        union_hits.insert(union_hits.end(), hits_444.begin(), hits_444.end());
    }
    const std::vector<std::uint64_t> expected_hits{20u, 22u, 40u, 42u};
    REQUIRE(union_hits == expected_hits);

    REQUIRE(proof.forbidden_host_anchor_concatenations == 0u);
    REQUIRE_FALSE(proof.has_forbidden_work());
}

TEST_CASE("Format-5 ABI3 cold resident preserves raw SoA and local positions",
          "[vulkan][index][persisted][shape-v5][abi3][cold-load]") {
    STATIC_REQUIRE_FALSE(
        idx_ns::GpuResidentShapeV5Target::permits_host_decode_or_rebuild());
    STATIC_REQUIRE_FALSE(
        idx_ns::GpuResidentShapeV5Target::qualifies_as_all_gpu_execution());
    const auto fixture = make_shape_v5_gpu_resident_fixture();
    auto& runtime = ::bioseq::gpu::vulkan::Runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) return;

    constexpr std::uint64_t cold_load_identity = 0xC01DCAC4EULL;
    auto ingest = idx_ns::GpuShapeV5FullGenerationIngest::begin(
        runtime, fixture.head, fixture.manifest,
        shape_v5_gpu_expected_compatibility(), fixture.directory,
        cold_load_identity);
    REQUIRE(ingest.has_value());
    const auto requests = ingest->requests();
    REQUIRE(requests.size() == 4u);
    REQUIRE(requests[0].sub_key == fixture.entries[0].sub_key);
    REQUIRE(requests[1].sub_key == fixture.entries[1].sub_key);
    REQUIRE(requests[2].sub_key
        == *idx_ns::derive_minimizer_position_extent_sub_key(
            fixture.entries[0].sub_key));
    REQUIRE(requests[3].sub_key
        == *idx_ns::derive_minimizer_position_extent_sub_key(
            fixture.entries[1].sub_key));

    for (std::size_t index = 0u; index < requests.size(); ++index) {
        const bool position = index >= fixture.entries.size();
        const std::size_t ordinal = position
            ? index - fixture.entries.size() : index;
        const auto& bytes = position
            ? fixture.position_bytes[ordinal]
            : fixture.control_bytes[ordinal];
        const idx_ns::IndexChunkAddress address{
                .generation_id = requests[index].generation_id,
                .position_segment = requests[index].position_segment,
                .sub_key = requests[index].sub_key,
                .chunk_no = requests[index].first_chunk_no,
            };
        auto receipt = idx_ns::validate_minimizer_generation_row_receipt(
            idx_ns::MinimizerGenerationRowView{
                .address = address,
                .bytes = bytes,
                .row_checksum = idx_ns::crc64_index_chunk_row(
                    address.generation_id, address.position_segment,
                    address.sub_key, address.chunk_no,
                    static_cast<std::uint32_t>(bytes.size()), bytes),
            });
        REQUIRE(receipt.has_value());
        REQUIRE(ingest->accept_validated_row(*receipt).has_value());
    }
    const auto finished = ingest->finish();
    REQUIRE(finished.has_value());
    const auto& resident = **finished;
    REQUIRE(resident.validated());
    REQUIRE(resident.identity().generation_id
        == fixture.manifest.generation_id);
    REQUIRE(resident.identity().source_identity
        == fixture.manifest.source_identity);
    REQUIRE(resident.identity().source_content_checksum
        == fixture.manifest.source_content_checksum);
    REQUIRE(resident.identity().device_uuid
        == runtime.features().device_uuid);
    REQUIRE(resident.identity().cold_load_identity == cold_load_identity);
    REQUIRE(resident.identity().gpu_abi_version
        == idx_ns::kMinimizerShapeV5GpuAbiVersion);
    REQUIRE(resident.control_count() == fixture.manifest.control_count);
    REQUIRE(resident.position_count() == fixture.manifest.position_count);
    REQUIRE(resident.descriptor_count() == fixture.entries.size());
    REQUIRE(resident.directory_bytes()
        == fixture.entries.size()
            * idx_ns::MinimizerBucketDirEntry::format5_encoded_bytes);

    std::vector<std::byte> expected_controls;
    std::vector<std::byte> expected_positions;
    for (const auto& bytes : fixture.control_bytes) {
        expected_controls.insert(
            expected_controls.end(), bytes.begin(), bytes.end());
    }
    for (const auto& bytes : fixture.position_bytes) {
        expected_positions.insert(
            expected_positions.end(), bytes.begin(), bytes.end());
    }
    const std::span<const std::byte> resident_controls{
        static_cast<const std::byte*>(resident.controls().mapped),
        expected_controls.size()};
    const std::span<const std::byte> resident_positions{
        static_cast<const std::byte*>(resident.positions().mapped),
        expected_positions.size()};
    REQUIRE(std::ranges::equal(resident_controls, expected_controls));
    REQUIRE(std::ranges::equal(resident_positions, expected_positions));

    const auto* descriptors = static_cast<const
        bioseq::gpu::vulkan::GpuShapeV5ResidentPartitionDescriptor*>(
            resident.descriptors().mapped);
    REQUIRE(descriptors != nullptr);
    REQUIRE(descriptors[0].flags
        == bioseq::gpu::vulkan::k_gpu_shape_v5_partition_required_flags);
    REQUIRE(descriptors[0].hash_byte_offset == 0u);
    REQUIRE(descriptors[0].prefix_byte_offset == 8u);
    REQUIRE(descriptors[0].count_byte_offset == 16u);
    REQUIRE(descriptors[0].position_byte_offset == 0u);
    REQUIRE(descriptors[0].control_count == 2u);
    REQUIRE(descriptors[0].position_count == 3u);
    REQUIRE(descriptors[0].hash_width == 4u);
    REQUIRE(descriptors[0].offset_width == 2u);
    REQUIRE(descriptors[0].count_width == 2u);
    REQUIRE(descriptors[0].position_width == 2u);
    REQUIRE(descriptors[0].segment_base_bp == 0u);
    REQUIRE(descriptors[0].segment_end_bp == 32u);
    REQUIRE(descriptors[1].hash_byte_offset
        == fixture.control_bytes[0].size());
    REQUIRE(descriptors[1].position_byte_offset
        == fixture.position_bytes[0].size());
    REQUIRE(descriptors[1].segment_base_bp == 32u);
    REQUIRE(descriptors[1].segment_end_bp == 64u);

    const std::uint64_t actual_allocations =
        resident.controls().allocation_bytes
        + resident.positions().allocation_bytes
        + resident.descriptors().allocation_bytes;
    REQUIRE(resident.admitted_device_bytes() == actual_allocations);
    const auto& proof = resident.cold_load_proof();
    REQUIRE(proof.cold_cache_misses == 1u);
    REQUIRE(proof.full_generations_ingested == 1u);
    REQUIRE(proof.control_bytes_copied == expected_controls.size());
    REQUIRE(proof.position_bytes_copied == expected_positions.size());
    REQUIRE(proof.descriptor_bytes_copied
        == fixture.entries.size()
            * sizeof(bioseq::gpu::vulkan::
                GpuShapeV5ResidentPartitionDescriptor));
    REQUIRE(proof.device_allocation_bytes == actual_allocations);
    REQUIRE(proof.admitted_device_bytes == actual_allocations);
    REQUIRE(proof.validated_receipt_rows == requests.size());
    REQUIRE(proof.raw_rehashed_rows == 0u);
    REQUIRE(proof.payload_hash_byte_visits == 0u);
    REQUIRE_FALSE(proof.has_forbidden_work());
}

TEST_CASE("Format-5 ABI3 cold resident fails closed before publication",
          "[vulkan][index][persisted][shape-v5][abi3][cold-load][fail-closed]") {
    const auto fixture = make_shape_v5_gpu_resident_fixture();
    auto& runtime = ::bioseq::gpu::vulkan::Runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) return;

    const auto missing_cold_identity =
        idx_ns::GpuShapeV5FullGenerationIngest::begin(
            runtime, fixture.head, fixture.manifest,
            shape_v5_gpu_expected_compatibility(), fixture.directory, 0u);
    REQUIRE_FALSE(missing_cold_identity.has_value());
    REQUIRE(missing_cold_identity.error()
        == idx_ns::GpuPersistedIndexFoundationError::invalid_identity);

    auto wrong_cap = shape_v5_gpu_expected_compatibility();
    wrong_cap.max_occ_cap = 11u;
    const auto relabelled = idx_ns::GpuShapeV5FullGenerationIngest::begin(
        runtime, fixture.head, fixture.manifest, wrong_cap,
        fixture.directory, 1u);
    REQUIRE_FALSE(relabelled.has_value());
    REQUIRE(relabelled.error()
        == idx_ns::GpuPersistedIndexFoundationError::metadata_mismatch);

    auto ingest = idx_ns::GpuShapeV5FullGenerationIngest::begin(
        runtime, fixture.head, fixture.manifest,
        shape_v5_gpu_expected_compatibility(), fixture.directory, 2u);
    REQUIRE(ingest.has_value());
    auto corrupt = fixture.control_bytes[0];
    corrupt.front() ^= std::byte{1};
    const auto& request = ingest->requests().front();
    const auto accepted = ingest->accept_raw_chunk_for_test(
        idx_ns::IndexChunkAddress{
            .generation_id = request.generation_id,
            .position_segment = request.position_segment,
            .sub_key = request.sub_key,
            .chunk_no = request.first_chunk_no,
        },
        corrupt);
    REQUIRE_FALSE(accepted.has_value());
    REQUIRE(accepted.error()
        == idx_ns::GpuPersistedIndexFoundationError::checksum);
    const auto unpublished = ingest->finish();
    REQUIRE_FALSE(unpublished.has_value());
    REQUIRE(unpublished.error()
        == idx_ns::GpuPersistedIndexFoundationError::checksum);
}

TEST_CASE("format-4 GPU stream uses bounded raw slots and remains foundation-only",
          "[vulkan][index][persisted][grouped32][stream]") {
    const auto fixture = make_grouped32_gpu_stream_fixture();
    REQUIRE(fixture.directory.profile().wire_format
        == idx_ns::MinimizerGenerationWireFormat::
            grouped_hash32_segment_local32);
    REQUIRE(fixture.directory.profile().control_encoded_byte_width == 12u);
    REQUIRE(fixture.directory.profile().position_encoded_byte_width == 4u);
    REQUIRE_FALSE(
        idx_ns::GpuGrouped32SealedTargetStream::
            qualifies_as_all_gpu_execution());

    auto& runtime = ::bioseq::gpu::vulkan::Runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) return;

    idx_ns::GpuPersistedIndexLocalProof directory_proof{};
    auto resident_directory = idx_ns::upload_authoritative_gpu_directory(
        runtime, fixture.head, fixture.manifest,
        grouped32_gpu_expected_compatibility(), fixture.directory,
        directory_proof);
    REQUIRE(resident_directory.has_value());
    REQUIRE((*resident_directory)->identity().gpu_abi_version
        == idx_ns::kMinimizerGrouped32GpuAbiVersion);

    const std::vector<MinimapSketchData> query{
        MinimapSketchData{0x111u, 1u, 0u},
        MinimapSketchData{0x333u, 3u, 0u},
        MinimapSketchData{0x777u, 5u, 0u},
        MinimapSketchData{0x999u, 7u, 0u},
    };
    using query_traits =
        bioseq::gpu::vulkan::minimizer_wire_traits<MinimapSketchData>;
    const auto query_bytes = std::as_bytes(
        std::span<const MinimapSketchData>{query});
    constexpr VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto query_buffer = runtime.create_device_buffer(query_bytes.size(), usage);
    REQUIRE(query_buffer.mapped != nullptr);
    std::memcpy(query_buffer.mapped, query_bytes.data(), query_bytes.size());
    auto selection = idx_ns::select_gpu_persisted_directory_ordinals(
        runtime,
        idx_ns::GpuDeviceQueryMinimizerView{
            .device_words = &query_buffer,
            .count = static_cast<std::uint32_t>(query.size()),
            .word_kind = static_cast<std::uint32_t>(query_traits::word_kind),
            .words32_per_record = query_traits::words32_per_minimizer,
            .hash_shift = query_traits::hash_shift,
        },
        **resident_directory, directory_proof);
    const auto selected = selection.values_for(**resident_directory);
    REQUIRE(selected.has_value());
    REQUIRE(std::ranges::equal(*selected,
        std::array<std::uint32_t, 4>{0u, 1u, 2u, 3u}));

    const std::vector<bioseq::gpu::vulkan::GpuGrouped32QueryMinimizerWire>
        grouped_query{{
            {.hash = 0x111u, .query_bp_strand = 28u,
             .occurrence_index = 0u, .query_record_index = 0u},
            {.hash = 0x333u, .query_bp_strand = 36u,
             .occurrence_index = 1u, .query_record_index = 0u},
            {.hash = 0x777u, .query_bp_strand = 44u,
             .occurrence_index = 2u, .query_record_index = 0u},
            {.hash = 0x999u, .query_bp_strand = 52u,
             .occurrence_index = 3u, .query_record_index = 0u},
        }};
    const std::vector<bioseq::gpu::vulkan::GpuGrouped32QueryRecordWire>
        grouped_records{{
            {.minimizer_offset = 0u,
             .minimizer_count = static_cast<std::uint32_t>(
                 grouped_query.size()),
             .query_length = 200u,
             .query_state_index = 0u,
             .occurrence_offset = 0u,
             .batch_join_ordinal = 1u},
        }};
    const auto grouped_query_bytes = std::as_bytes(
        std::span<const bioseq::gpu::vulkan::GpuGrouped32QueryMinimizerWire>{
            grouped_query});
    const auto grouped_record_bytes = std::as_bytes(
        std::span<const bioseq::gpu::vulkan::GpuGrouped32QueryRecordWire>{
            grouped_records});
    auto grouped_query_buffer = runtime.create_device_buffer(
        grouped_query_bytes.size(), usage);
    auto grouped_record_buffer = runtime.create_device_buffer(
        grouped_record_bytes.size(), usage);
    REQUIRE(grouped_query_buffer.mapped != nullptr);
    REQUIRE(grouped_record_buffer.mapped != nullptr);
    std::memcpy(grouped_query_buffer.mapped, grouped_query_bytes.data(),
        grouped_query_bytes.size());
    std::memcpy(grouped_record_buffer.mapped, grouped_record_bytes.data(),
        grouped_record_bytes.size());

    auto stream = idx_ns::GpuGrouped32TwoPhaseStream::begin(
        runtime, fixture.head, fixture.manifest,
        grouped32_gpu_expected_compatibility(), fixture.directory,
        *resident_directory, selection,
        idx_ns::GpuGrouped32StreamConfig{
            .ring_slot_count = 2u,
            .slot_payload_capacity_bytes = 24u,
            .slot_partition_capacity = 1u,
            .query_identity = 0xABCDEFu,
            .unique_hash_count = 4u,
            .max_occ = 3u,
            .device_query_records = &grouped_record_buffer,
            .device_query_minimizers = &grouped_query_buffer,
            .query_record_count = static_cast<std::uint32_t>(
                grouped_records.size()),
            .query_minimizer_count = static_cast<std::uint32_t>(
                grouped_query.size()),
        });
    REQUIRE(stream.has_value());
    REQUIRE(stream->control_group_count() == 4u);
    REQUIRE(stream->phase() == idx_ns::GpuGrouped32StreamPhase::controls);
    const auto early_positions = stream->position_requests(0u);
    REQUIRE_FALSE(early_positions.has_value());
    REQUIRE(early_positions.error()
        == idx_ns::GpuPersistedIndexFoundationError::unauthorized_extent);
    REQUIRE(stream->phase() == idx_ns::GpuGrouped32StreamPhase::controls);

    auto load_control_group = [&stream, &fixture](std::uint32_t group) {
        const auto requests = stream->control_requests(group);
        REQUIRE(requests.has_value());
        REQUIRE(requests->size() == 1u);
        const auto opened = stream->open_control_group(group);
        REQUIRE(opened.has_value());
        const auto& request = requests->front();
        const std::size_t ordinal = static_cast<std::size_t>(
            request.sub_key - 300);
        REQUIRE(ordinal < fixture.control_bytes.size());
        REQUIRE(stream->accept_control_chunk(
            *opened,
            idx_ns::IndexChunkAddress{
                .generation_id = request.generation_id,
                .position_segment = request.position_segment,
                .sub_key = request.sub_key,
                .chunk_no = request.first_chunk_no,
            },
            fixture.control_bytes[ordinal]).has_value());
        const auto closed = stream->close_control_group(*opened);
        REQUIRE(closed.has_value());
        REQUIRE(closed->payload_bytes
            == fixture.control_bytes[ordinal].size());
        const std::span<const std::byte> resident_bytes{
            static_cast<const std::byte*>(closed->payload->mapped),
            static_cast<std::size_t>(closed->payload_bytes)};
        REQUIRE(std::ranges::equal(
            resident_bytes, fixture.control_bytes[ordinal]));
        return *closed;
    };
    const auto controls0 = load_control_group(0u);
    const auto controls1 = load_control_group(1u);
    REQUIRE(stream->dispatch_control_group(controls1).has_value());
    REQUIRE(stream->dispatch_control_group(controls0).has_value());
    const auto controls2 = load_control_group(2u);
    const auto controls3 = load_control_group(3u);
    REQUIRE(stream->dispatch_control_group(controls3).has_value());
    REQUIRE(stream->dispatch_control_group(controls2).has_value());
    REQUIRE(stream->phase()
        == idx_ns::GpuGrouped32StreamPhase::count_finalize);

    const auto authorization =
        stream->dispatch_count_finalize_and_bind_positions(
            fixture.directory);
    REQUIRE(authorization.has_value());
    REQUIRE(std::ranges::equal(
        authorization->authorized_directory_ordinals,
        std::array<std::uint32_t, 3>{0u, 2u, 3u}));
    REQUIRE(stream->position_group_count() == 3u);

    auto load_position_group = [&stream, &fixture](std::uint32_t group) {
        const auto requests = stream->position_requests(group);
        REQUIRE(requests.has_value());
        REQUIRE(requests->size() == 1u);
        const auto opened = stream->open_position_group(group);
        REQUIRE(opened.has_value());
        const auto& request = requests->front();
        const auto logical_sub_key = idx_ns::project_minimizer_extent_sub_key(
            request.sub_key, true);
        REQUIRE(logical_sub_key.has_value());
        const std::size_t ordinal = static_cast<std::size_t>(
            *logical_sub_key - 300);
        REQUIRE(ordinal < fixture.position_bytes.size());
        REQUIRE(stream->accept_position_chunk(
            *opened,
            idx_ns::IndexChunkAddress{
                .generation_id = request.generation_id,
                .position_segment = request.position_segment,
                .sub_key = request.sub_key,
                .chunk_no = request.first_chunk_no,
            },
            fixture.position_bytes[ordinal]).has_value());
        const auto closed = stream->close_position_group(*opened);
        REQUIRE(closed.has_value());
        const std::span<const std::byte> resident_bytes{
            static_cast<const std::byte*>(closed->payload->mapped),
            static_cast<std::size_t>(closed->payload_bytes)};
        REQUIRE(std::ranges::equal(
            resident_bytes, fixture.position_bytes[ordinal]));
        return *closed;
    };
    const auto positions0 = load_position_group(0u);
    const auto positions1 = load_position_group(1u);
    REQUIRE(stream->dispatch_position_group(positions1).has_value());
    REQUIRE(stream->dispatch_position_group(positions0).has_value());
    const auto positions2 = load_position_group(2u);
    REQUIRE(stream->dispatch_position_group(positions2).has_value());
    REQUIRE(stream->phase() == idx_ns::GpuGrouped32StreamPhase::target_seal);

    auto sealed = stream->dispatch_target_seal();
    REQUIRE(sealed.has_value());
    REQUIRE(sealed->foundation_validated());
    REQUIRE(sealed->runtime_operation_receipts_complete());
    REQUIRE(sealed->runtime_anchor_buffer() != nullptr);
    REQUIRE(sealed->runtime_anchor_buffer()->bytes
        == 4u * sizeof(
            bioseq::gpu::vulkan::GpuGrouped32GlobalAnchorWire));
    REQUIRE_FALSE(sealed->qualifies_as_all_gpu_execution());
    REQUIRE(sealed->local_proof().grouped32_control_groups_completed == 4u);
    REQUIRE(sealed->local_proof().grouped32_position_groups_completed == 3u);
    REQUIRE(sealed->local_proof().grouped32_slot_reuses >= 5u);
    REQUIRE(sealed->local_proof().grouped32_max_inflight_slots == 2u);
    REQUIRE_FALSE(sealed->local_proof().has_forbidden_work());
}

TEST_CASE("DOC0371 persisted GPU foundation fails closed on unsupported inputs",
          "[vulkan][index][persisted][fail-closed]") {
    auto fixture = make_persisted_gpu_fixture();
    idx_ns::GpuPersistedIndexLocalProof proof{};

    auto bad_manifest = fixture.manifest;
    bad_manifest.source_identity++;
    auto& runtime = ::bioseq::gpu::vulkan::Runtime::instance();
    auto wrong_scheme = persisted_gpu_expected_compatibility();
    wrong_scheme.minimizer_scheme_id = idx_ns::kPaperNtHashU32R7SumV1SchemeId;
    wrong_scheme.minimizer_scheme_version =
        idx_ns::kPaperNtHashU32R7SumV1SchemeVersion;
    auto& admission =
        bioseq::parallelism::MemoryAdmissionController::instance();
    const std::uint64_t admitted_before_cross_scheme =
        admission.reserved_bytes();
    const auto cross_scheme_directory =
        idx_ns::upload_authoritative_gpu_directory(
            runtime, fixture.head, fixture.manifest, wrong_scheme,
            fixture.directory, proof);
    REQUIRE_FALSE(cross_scheme_directory.has_value());
    REQUIRE(cross_scheme_directory.error()
            == idx_ns::GpuPersistedIndexFoundationError::metadata_mismatch);
    REQUIRE(proof.authoritative_directory_uploads == 0u);
    REQUIRE(admission.reserved_bytes() == admitted_before_cross_scheme);

    if (::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        const auto bad_directory = idx_ns::upload_authoritative_gpu_directory(
            runtime, fixture.head, bad_manifest,
            persisted_gpu_expected_compatibility(), fixture.directory, proof);
        REQUIRE_FALSE(bad_directory.has_value());
        REQUIRE(bad_directory.error()
                == idx_ns::GpuPersistedIndexFoundationError::metadata_mismatch);

        auto resident_directory = idx_ns::upload_authoritative_gpu_directory(
            runtime, fixture.head, fixture.manifest,
            persisted_gpu_expected_compatibility(), fixture.directory, proof);
        REQUIRE(resident_directory.has_value());
        const std::vector<MinimapSketchData> query{
            MinimapSketchData{0x222u, 1u, 0u}};
        using query_traits =
            bioseq::gpu::vulkan::minimizer_wire_traits<MinimapSketchData>;
        const auto query_bytes = std::as_bytes(
            std::span<const MinimapSketchData>{query});
        constexpr VkBufferUsageFlags usage =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        auto query_buffer = runtime.create_device_buffer(query_bytes.size(), usage);
        REQUIRE(query_buffer.mapped != nullptr);
        std::memcpy(query_buffer.mapped, query_bytes.data(), query_bytes.size());
        const auto selection = idx_ns::select_gpu_persisted_directory_ordinals(
            runtime,
            idx_ns::GpuDeviceQueryMinimizerView{
                .device_words = &query_buffer,
                .count = static_cast<std::uint32_t>(query.size()),
                .word_kind = static_cast<std::uint32_t>(query_traits::word_kind),
                .words32_per_record = query_traits::words32_per_minimizer,
                .hash_shift = query_traits::hash_shift,
            },
            **resident_directory,
            proof);

        auto entry = fixture.entries[0];
        const idx_ns::GpuPersistedMinimizerIndexCacheKey key{
            .generation_id = fixture.manifest.generation_id,
            .source_identity = fixture.manifest.source_identity,
            .position_segment = entry.position_segment,
            .sub_key = entry.sub_key,
            .device_uuid = runtime.features().device_uuid,
            .sorted_dense_abi_version =
                idx_ns::kMinimizerSortedDenseGpuAbiVersion,
        };
        auto stream = idx_ns::GpuSortedDensePartitionStream::begin(
            runtime, key, selection, **resident_directory,
            fixture.directory, fixture.manifest,
            persisted_gpu_expected_compatibility(), entry.partition_id);
        REQUIRE(stream.has_value());
        auto corrupt_controls = fixture.control_bytes[0];
        corrupt_controls.front() ^= std::byte{1};
        REQUIRE(stream->accept_control_bytes(corrupt_controls).has_value());
        REQUIRE(stream->accept_position_bytes(
            fixture.position_bytes[0]).has_value());
        idx_ns::GpuPersistedMinimizerIndexCache cache{1u << 20};
        const auto rejected = stream->finish(cache, proof);
        REQUIRE_FALSE(rejected.has_value());
        REQUIRE(rejected.error()
                == idx_ns::GpuPersistedIndexFoundationError::checksum);

        auto other_generation = make_persisted_gpu_fixture();
        other_generation.manifest.generation_id++;
        other_generation.manifest.source_identity++;
        refresh_persisted_gpu_fixture_head(other_generation);
        auto other_directory = idx_ns::upload_authoritative_gpu_directory(
            runtime, other_generation.head, other_generation.manifest,
            persisted_gpu_expected_compatibility(),
            other_generation.directory, proof);
        REQUIRE(other_directory.has_value());
        const auto laundered_values = selection.values_for(**other_directory);
        REQUIRE_FALSE(laundered_values.has_value());
        REQUIRE(laundered_values.error()
                == idx_ns::GpuPersistedIndexFoundationError::
                    selection_identity_mismatch);
        const auto& other_entry = other_generation.entries[0];
        const idx_ns::GpuPersistedMinimizerIndexCacheKey other_key{
            .generation_id = other_generation.manifest.generation_id,
            .source_identity = other_generation.manifest.source_identity,
            .position_segment = other_entry.position_segment,
            .sub_key = other_entry.sub_key,
            .device_uuid = runtime.features().device_uuid,
            .sorted_dense_abi_version =
                idx_ns::kMinimizerSortedDenseGpuAbiVersion,
        };
        const auto laundered_stream =
            idx_ns::GpuSortedDensePartitionStream::begin(
                runtime, other_key, selection, **other_directory,
                other_generation.directory, other_generation.manifest,
                persisted_gpu_expected_compatibility(),
                other_entry.partition_id);
        REQUIRE_FALSE(laundered_stream.has_value());
        REQUIRE(laundered_stream.error()
                == idx_ns::GpuPersistedIndexFoundationError::
                    selection_identity_mismatch);

        REQUIRE_THROWS_AS(
            runtime.run_persisted_directory_ordinal_selection(
                bioseq::gpu::vulkan::GpuPersistedDirectorySelectionInputs{
                    .device_minimizer_words = nullptr,
                    .encoded_directory = nullptr,
                    .minimizer_count = 1u,
                    .minimizer_word_kind = static_cast<std::uint32_t>(
                        bioseq::gpu::vulkan::MinimizerWordKind::u64),
                    .minimizer_words32_per_record = 2u,
                    .hash_shift = 1u,
                    .directory_entry_count = 1u,
                }),
            bioseq::gpu::vulkan::VulkanRuntimeError);
    }

    const idx_ns::GpuPersistedMinimizerIndexCacheKey unbound{
        .generation_id = 1u,
        .source_identity = 2u,
        .position_segment = 0,
        .sub_key = 3,
        .device_uuid = {},
        .sorted_dense_abi_version = idx_ns::kMinimizerSortedDenseGpuAbiVersion,
    };
    REQUIRE_FALSE(idx_ns::valid_gpu_persisted_cache_key(unbound));
    REQUIRE_FALSE(proof.has_forbidden_work());
}

struct alignas(16) IneligibleSourceDirectMinimizer128 {
    struct layout_t {
        static constexpr std::uint32_t storage_bits = 128u;
        static constexpr std::uint32_t hash_bits = 64u;
        static constexpr std::uint32_t pos_bits = 64u;
    };

    std::uint64_t hash = 0u;
    std::uint64_t encoded = 0u;

    [[nodiscard]] std::uint64_t kmer() const noexcept { return hash; }
    [[nodiscard]] std::uint64_t pos_strand_encoded() const noexcept {
        return encoded;
    }
};
static_assert(sizeof(IneligibleSourceDirectMinimizer128) == 16u);

TEST_CASE("vulkan source-direct preflight uses exact allocation requirements",
          "[vulkan][index][source-direct][admission]"
          "[vulkan][index][phase-b][source-direct-sorted-dense]") {
    const auto plan = idx_ns::plan_gpu_source_direct_sorted_dense_with_requirements(
        /*minimizer_count=*/5u,
        /*distinct_count=*/3u,
        /*max_storage_buffer_range=*/4096u,
        /*control_allocation_requirement_bytes=*/64u,
        /*position_allocation_requirement_bytes=*/80u);
    REQUIRE(plan.has_value());
    REQUIRE(plan->control_bytes == 48u);
    REQUIRE(plan->position_bytes == 40u);
    REQUIRE(plan->logical_resident_bytes == 88u);
    REQUIRE(plan->control_allocation_allowance_bytes == 16u);
    REQUIRE(plan->position_allocation_allowance_bytes == 40u);
    REQUIRE(plan->allocation_requirement_bytes == 144u);
    REQUIRE(plan->conservative_request_bytes == 144u);
    REQUIRE(idx_ns::gpu_source_direct_preflight_fits(*plan, 144u));
    REQUIRE_FALSE(idx_ns::gpu_source_direct_preflight_fits(*plan, 143u));

    const auto empty =
        idx_ns::plan_gpu_source_direct_sorted_dense_with_requirements(
            0u, 0u, 4096u, 0u, 0u);
    REQUIRE(empty.has_value());
    REQUIRE(empty->canonical_empty);
    REQUIRE(idx_ns::gpu_source_direct_preflight_fits(*empty, 0u));

    const auto undersized =
        idx_ns::plan_gpu_source_direct_sorted_dense_with_requirements(
            5u, 3u, 4096u, 47u, 40u);
    REQUIRE_FALSE(undersized.has_value());
    REQUIRE(undersized.error()
            == idx_ns::GpuSourceDirectMaterializationError::
                device_abi_overflow);

    for (const auto [n, d] : {
            std::pair<std::uint64_t, std::uint64_t>{0u, 1u},
            std::pair<std::uint64_t, std::uint64_t>{1u, 0u},
            std::pair<std::uint64_t, std::uint64_t>{2u, 3u}}) {
        const auto invalid_shape =
            idx_ns::plan_gpu_source_direct_sorted_dense_with_requirements(
                n, d, 4096u, 64u, 64u);
        REQUIRE_FALSE(invalid_shape.has_value());
        REQUIRE(invalid_shape.error()
                == idx_ns::GpuSourceDirectMaterializationError::invalid_shape);
    }
    const auto invalid_empty_requirement =
        idx_ns::plan_gpu_source_direct_sorted_dense_with_requirements(
            0u, 0u, 4096u, 1u, 0u);
    REQUIRE_FALSE(invalid_empty_requirement.has_value());
    REQUIRE(invalid_empty_requirement.error()
            == idx_ns::GpuSourceDirectMaterializationError::invalid_shape);

    constexpr std::uint64_t u32_max =
        (std::numeric_limits<std::uint32_t>::max)();
    const auto abi_overflow =
        idx_ns::plan_gpu_source_direct_sorted_dense_with_requirements(
            u32_max + 1u, 1u,
            (std::numeric_limits<std::uint64_t>::max)(), 16u, 8u);
    REQUIRE_FALSE(abi_overflow.has_value());
    REQUIRE(abi_overflow.error()
            == idx_ns::GpuSourceDirectMaterializationError::
                device_abi_overflow);

    const auto range_equality =
        idx_ns::plan_gpu_source_direct_sorted_dense_with_requirements(
            2u, 1u, /*max_storage_buffer_range=*/16u, 16u, 16u);
    REQUIRE(range_equality.has_value());
    const auto range_one_over =
        idx_ns::plan_gpu_source_direct_sorted_dense_with_requirements(
            2u, 1u, /*max_storage_buffer_range=*/15u, 16u, 16u);
    REQUIRE_FALSE(range_one_over.has_value());
    REQUIRE(range_one_over.error()
            == idx_ns::GpuSourceDirectMaterializationError::
                storage_buffer_range_exceeded);

    constexpr std::uint64_t xxl_n = 93'308'896u;
    constexpr std::uint64_t xxl_d = 70'249'364u;
    constexpr std::uint64_t xxl_control = 1'123'989'824u;
    constexpr std::uint64_t xxl_positions = 746'471'168u;
    constexpr std::uint64_t allowance = 65'536u;
    const auto xxl =
        idx_ns::plan_gpu_source_direct_sorted_dense_with_requirements(
            xxl_n, xxl_d, 4'294'967'295u,
            xxl_control + allowance,
            xxl_positions + allowance);
    REQUIRE(xxl.has_value());
    REQUIRE(xxl->control_bytes == xxl_control);
    REQUIRE(xxl->position_bytes == xxl_positions);
    REQUIRE(xxl->logical_resident_bytes == 1'870'460'992u);
    REQUIRE(xxl->allowance_bytes == 131'072u);
    REQUIRE(xxl->conservative_request_bytes == 1'870'592'064u);

    auto& runtime = bioseq::gpu::vulkan::Runtime::instance();
    bioseq::parallelism::MemoryPermit no_permit{};
    const auto ineligible =
        idx_ns::materialize_sorted_dense_from_presorted_minimizers(
            runtime,
            std::span<const IneligibleSourceDirectMinimizer128>{},
            /*target_len=*/0u,
            idx_ns::GpuSourceDirectPreflight{},
            idx_ns::GpuMinimizerIndexCacheKey{},
            no_permit);
    REQUIRE_FALSE(ineligible.has_value());
    REQUIRE(ineligible.error().error
            == idx_ns::GpuSourceDirectMaterializationError::
                representation_ineligible);
}

TEST_CASE("vulkan source-direct fill consumes each source position once",
          "[vulkan][index][source-direct][checksum]"
          "[vulkan][index][phase-b][source-direct-sorted-dense]") {
    struct SingleReadMinimizer {
        std::uint64_t hash = 0u;
        std::uint64_t encoded = 0u;
        mutable std::uint32_t reads = 0u;

        [[nodiscard]] std::uint64_t kmer() const noexcept { return hash; }
        [[nodiscard]] std::uint64_t pos_strand_encoded() const {
            if (++reads != 1u) throw std::runtime_error("source reread");
            return encoded;
        }
    };
    std::vector<SingleReadMinimizer> source{
        {.hash = 10u, .encoded = 4u},
        {.hash = 10u, .encoded = 7u},
        {.hash = 20u, .encoded = 12u},
    };
    std::array<idx_ns::GpuMinimizerSlot, 2u> controls{};
    std::array<std::uint64_t, 3u> positions{};
    const auto fill_proof =
        idx_ns::direct_fill_gpu_minimizer_index_from_presorted_source(
            std::span<const SingleReadMinimizer>{source},
            /*target_len=*/100u,
            idx_ns::GpuSortedSourceShape{
                .minimizer_count = source.size(),
                .distinct_count = controls.size(),
            },
            std::span{controls},
            std::span{positions});
    REQUIRE(fill_proof.source_checksum == fill_proof.mapped_checksum);
    REQUIRE(fill_proof.complete_source_traversals == 1u);
    REQUIRE(fill_proof.complete_mapped_traversals == 1u);
    REQUIRE(std::all_of(source.begin(), source.end(),
        [](const SingleReadMinimizer& minimizer) {
            return minimizer.reads == 1u;
        }));

    REQUIRE(controls[0].hash == 10u);
    REQUIRE(controls[0].pos_offset == 0u);
    REQUIRE(controls[0].pos_count == 2u);
    REQUIRE(controls[1].hash == 20u);
    REQUIRE(controls[1].pos_offset == 2u);
    REQUIRE(controls[1].pos_count == 1u);
    REQUIRE((positions == std::array<std::uint64_t, 3u>{4u, 7u, 12u}));

    std::vector<std::pair<std::uint64_t, std::uint64_t>> oracle_pairs{
        {10u, 4u}, {10u, 7u}, {20u, 12u}};
    MinimizerHashmapFragmentIndex oracle;
    oracle.build_from_pairs(oracle_pairs, 100u);
    for (std::size_t control_index = 0u;
         control_index < controls.size(); ++control_index) {
        const auto& control = controls[control_index];
        const auto expected = oracle.lookup(control.hash);
        REQUIRE(expected.size() == control.pos_count);
        REQUIRE(std::equal(
            expected.begin(), expected.end(),
            positions.begin() + control.pos_offset));
    }
    REQUIRE(oracle.lookup(30u).empty());

    std::vector<SingleReadMinimizer> unsorted{
        {.hash = 20u, .encoded = 12u},
        {.hash = 10u, .encoded = 4u},
    };
    const auto rejected_shape = idx_ns::inspect_presorted_minimizer_source(
        std::span<const SingleReadMinimizer>{unsorted});
    REQUIRE_FALSE(rejected_shape.has_value());
    REQUIRE(rejected_shape.error()
            == idx_ns::GpuSourceDirectMaterializationError::invalid_shape);

    constexpr std::uint64_t large_position =
        (std::uint64_t{1} << 62u) - 1u;
    std::vector<SingleReadMinimizer> boundary_source{
        {.hash = 99u, .encoded = (large_position << 1u) | 1u},
    };
    std::array<idx_ns::GpuMinimizerSlot, 1u> boundary_control{};
    std::array<std::uint64_t, 1u> boundary_position{};
    const auto boundary =
        idx_ns::direct_fill_gpu_minimizer_index_from_presorted_source(
            std::span<const SingleReadMinimizer>{boundary_source},
            /*target_len=*/large_position + 1u,
            idx_ns::GpuSortedSourceShape{
                .minimizer_count = 1u,
                .distinct_count = 1u,
            },
            std::span{boundary_control},
            std::span{boundary_position});
    REQUIRE(boundary.source_checksum == boundary.mapped_checksum);
    REQUIRE(boundary_position[0] == (large_position << 1u) + 1u);

    std::vector<SingleReadMinimizer> out_of_range{
        {.hash = 1u, .encoded = (100u << 1u)},
    };
    std::array<idx_ns::GpuMinimizerSlot, 1u> rejected_control{};
    std::array<std::uint64_t, 1u> rejected_position{};
    REQUIRE_THROWS_AS(
        idx_ns::direct_fill_gpu_minimizer_index_from_presorted_source(
            std::span<const SingleReadMinimizer>{out_of_range},
            /*target_len=*/100u,
            idx_ns::GpuSortedSourceShape{
                .minimizer_count = 1u,
                .distinct_count = 1u,
            },
            std::span{rejected_control},
            std::span{rejected_position}),
        idx_ns::GpuIndexDirectFillInvariant);
}

TEST_CASE("vulkan source-direct cache keys cannot upload or replace",
          "[vulkan][index][source-direct][cache]"
          "[vulkan][index][phase-b][source-direct-sorted-dense]") {
    using Cache = idx_ns::GpuMinimizerIndexCache;
    Cache cache{0u};
    idx_ns::GpuIndexCacheIdentity identity{
        .table_size = 2u,
        .position_count = 3u,
        .operation_identity = 0x1234u,
        .alphabet_size = 4u,
        .kmer_size = 15u,
        .window_size = 10u,
        .minimizer_size = 8u,
        .minimizer_alignment = 8u,
        .minimizer_hash_bits = 30u,
        .minimizer_position_bits = 33u,
        .sorted_dense_abi_version =
            idx_ns::kMinimizerSortedDenseGpuAbiVersion,
        .representation = idx_ns::GpuIndexRepresentation::sorted_dense,
        .source_direct_sorted_dense = true,
        .canonical_empty = false,
    };
    identity.device_uuid[0] = 1u;
    REQUIRE(idx_ns::gpu_source_direct_identity_well_formed(identity));
    const auto rejects_identity = [&identity](auto mutate) {
        auto candidate = identity;
        mutate(candidate);
        REQUIRE_FALSE(
            idx_ns::gpu_source_direct_identity_well_formed(candidate));
    };
    rejects_identity([](auto& value) { value.operation_identity = 0u; });
    rejects_identity([](auto& value) { value.table_size = 0u; });
    rejects_identity([](auto& value) { value.position_count = 0u; });
    rejects_identity([](auto& value) { value.table_size = 4u; });
    rejects_identity([](auto& value) { value.alphabet_size = 3u; });
    rejects_identity([](auto& value) { value.kmer_size = 14u; });
    rejects_identity([](auto& value) { value.window_size = 9u; });
    rejects_identity([](auto& value) { value.minimizer_size = 16u; });
    rejects_identity([](auto& value) { value.minimizer_alignment = 16u; });
    rejects_identity([](auto& value) { value.minimizer_hash_bits = 29u; });
    rejects_identity([](auto& value) { value.minimizer_position_bits = 32u; });
    rejects_identity([](auto& value) { ++value.sorted_dense_abi_version; });
    rejects_identity([](auto& value) {
        value.representation = idx_ns::GpuIndexRepresentation::
            open_addressed_70;
    });
    rejects_identity([](auto& value) { value.device_uuid = {}; });
    rejects_identity([](auto& value) {
        value.source_direct_sorted_dense = false;
    });
    rejects_identity([](auto& value) { value.canonical_empty = true; });
    rejects_identity([](auto& value) { value.gate_enabled = true; });

    MinimizerHashmapFragmentIndex placeholder;
    placeholder.target_len = 100u;
    const auto key = idx_ns::make_gpu_index_cache_key(
        placeholder, identity);
    const auto handle =
        std::make_shared<idx_ns::GpuResidentMinimizerIndex>();
    handle->key = key;
    handle->gate_identity = idx_ns::gpu_index_gate_identity_from_key(key);
    REQUIRE(cache.insert_built(key, handle).has_value());
    const auto duplicate = cache.insert_built(key, handle);
    REQUIRE_FALSE(duplicate.has_value());
    REQUIRE(duplicate.error() == Cache::BuiltInsertError::duplicate_direct_key);

    auto& runtime = bioseq::gpu::vulkan::Runtime::instance();
    const auto cpu_index = build_demo_index();
    REQUIRE_THROWS_AS(
        cache.get_or_upload(
            runtime, cpu_index, key, /*cache_enabled=*/true),
        bioseq::gpu::vulkan::VulkanRuntimeError);

    auto zero_uuid_key = key;
    zero_uuid_key.device_uuid = {};
    const auto invalid_lookup = cache.find_required_preinserted(
        runtime, zero_uuid_key);
    REQUIRE_FALSE(invalid_lookup.has_value());
    REQUIRE(invalid_lookup.error()
            == Cache::RequiredPreinsertLookupError::invalid_key);

    auto runtime_identity = identity;
    runtime_identity.operation_identity = 0x5678u;
    runtime_identity.device_uuid = runtime.features().device_uuid;
    if (idx_ns::gpu_source_direct_identity_well_formed(runtime_identity)) {
        Cache required_cache{0u};
        const auto runtime_key = idx_ns::make_gpu_index_cache_key(
            placeholder, runtime_identity);
        const auto miss = required_cache.find_required_preinserted(
            runtime, runtime_key);
        REQUIRE_FALSE(miss.has_value());
        REQUIRE(miss.error() == Cache::RequiredPreinsertLookupError::miss);
        REQUIRE(required_cache.resident_bytes() == 0u);

        const auto corrupt =
            std::make_shared<idx_ns::GpuResidentMinimizerIndex>();
        corrupt->key = runtime_key;
        corrupt->gate_identity =
            idx_ns::gpu_index_gate_identity_from_key(runtime_key);
        REQUIRE(required_cache.insert_built(runtime_key, corrupt).has_value());
        const auto rejected = required_cache.find_required_preinserted(
            runtime, runtime_key);
        REQUIRE_FALSE(rejected.has_value());
        REQUIRE(rejected.error()
                == Cache::RequiredPreinsertLookupError::resident_mismatch);
    }
}

}  // namespace

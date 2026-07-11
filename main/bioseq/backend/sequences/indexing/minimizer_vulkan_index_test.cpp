/**
 * @file minimizer_vulkan_index_test.cpp
 * @brief DOC0307 §13 — GPU minimizer index view + cache identity tests.
 *
 * Tests 5/9: the open-addressed GPU slot table derived from
 * `MinimizerHashmapFragmentIndex` resolves the same (hash -> positions) hits as
 * the CPU `lookup`, and distinct `assembly_identity` values (e.g. different
 * `add_position_offset`) produce distinct cache keys / hashes. The slot/key
 * tests are pure-host; the resident upload test skips when no device exists.
 */
#include "common_tests.h"
#include "common.h"

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
    proof.resident_bytes = resident_bytes;
    proof.gate_count_dispatches = gate_enabled ? 1u : 0u;
    proof.gated_emit_dispatches = gate_enabled && !empty ? 1u : 0u;
    proof.ungated_emit_dispatches = gate_enabled ? 0u : 1u;
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
            .position_first_chunk = 1u,
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
        std::span<const idx_ns::MinimizerBucketDirEntry>{fixture.entries});
    if (!encoded_directory) {
        throw std::runtime_error("failed to encode persisted GPU fixture directory");
    }
    auto directory = idx_ns::DirView::decode(*encoded_directory);
    if (!directory) {
        throw std::runtime_error("failed to decode persisted GPU fixture directory");
    }
    fixture.directory = std::move(*directory);

    fixture.manifest = idx_ns::MinimizerGenerationManifest{
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
    };
    refresh_persisted_gpu_fixture_head(fixture);
    return fixture;
}

[[nodiscard]] static PersistedGpuFixture make_empty_persisted_gpu_fixture() {
    PersistedGpuFixture fixture;
    const std::span<const std::byte> no_directory_bytes{};
    auto directory = idx_ns::DirView::decode(no_directory_bytes);
    if (!directory) {
        throw std::runtime_error("failed to decode empty persisted GPU directory");
    }
    fixture.directory = std::move(*directory);
    fixture.manifest = idx_ns::MinimizerGenerationManifest{
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
    };
    refresh_persisted_gpu_fixture_head(fixture);
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
        runtime, fixture.head, fixture.manifest, fixture.directory, proof);
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
        runtime, fixture.head, fixture.manifest, fixture.directory, proof);
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
        runtime, fixture.head, fixture.manifest, fixture.directory, proof);
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
            fixture.directory, fixture.manifest, entry.partition_id);
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
        runtime, fixture.head, fixture.manifest, fixture.directory, proof);
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
            fixture.directory, fixture.manifest, entry.partition_id);
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
        fixture.directory, fixture.manifest, duplicate_entry.partition_id);
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

TEST_CASE("DOC0371 persisted GPU foundation fails closed on unsupported inputs",
          "[vulkan][index][persisted][fail-closed]") {
    auto fixture = make_persisted_gpu_fixture();
    idx_ns::GpuPersistedIndexLocalProof proof{};

    auto bad_manifest = fixture.manifest;
    bad_manifest.source_identity++;
    auto& runtime = ::bioseq::gpu::vulkan::Runtime::instance();
    if (::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        const auto bad_directory = idx_ns::upload_authoritative_gpu_directory(
            runtime, fixture.head, bad_manifest, fixture.directory, proof);
        REQUIRE_FALSE(bad_directory.has_value());
        REQUIRE(bad_directory.error()
                == idx_ns::GpuPersistedIndexFoundationError::metadata_mismatch);

        auto resident_directory = idx_ns::upload_authoritative_gpu_directory(
            runtime, fixture.head, fixture.manifest, fixture.directory, proof);
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
            fixture.directory, fixture.manifest, entry.partition_id);
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

}  // namespace

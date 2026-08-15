/**
 * @file gpu_minimizer_index_test.cpp
 * @brief GPU minimizer index view and cache identity tests.
 *
 * The open-addressed and hash-sorted dense GPU control representations derived
 * from `minimizer_hashmap_fragment_index` resolve the same (hash -> positions)
 * hits as CPU `lookup`, and distinct `assembly_identity` values produce
 * distinct cache keys / hashes. Pure-host tests cover both layouts. Resident
 * upload tests skip when no device exists.
 */
#include <tests/support/common.hpp>

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

import bioseq.common.logging;
import bioseq.sequences.kmers;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.indexing.storage;
import bioseq.indexing.type;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;

namespace {

using minimap_sketch_data = bioseq::indexing::minimap_sketch_data;
using bioseq::indexing::minimizer_hashmap_fragment_index;
namespace idx_ns = bioseq::indexing;

/// Host simulation of the shader's open-addressed `find_slot`.
/// Host simulation of open-addressed lookup over an explicit slot span.
[[nodiscard]] static std::uint32_t host_find_slot(
    const idx_ns::gpu_minimizer_index_upload& upload, std::uint64_t hash) {
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
    std::span<const idx_ns::gpu_minimizer_slot> slots,
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
    std::span<const idx_ns::gpu_minimizer_slot> slots,
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

/// Build the repeated-hash CPU index used by upload tests.
[[nodiscard]] static minimizer_hashmap_fragment_index build_demo_index() {
    std::vector<minimap_sketch_data> minimizers;
    // Several distinct kmers; one with multiple positions (a repeat).
    minimizers.emplace_back(0x111ULL, 5u, 0u);
    minimizers.emplace_back(0x222ULL, 9u, 1u);
    minimizers.emplace_back(0x333ULL, 13u, 0u);
    minimizers.emplace_back(0x222ULL, 40u, 0u);   // repeat kmer 0x222
    minimizers.emplace_back(0x444ULL, 77u, 1u);
    minimizers.emplace_back(0x222ULL, 100u, 1u);  // repeat kmer 0x222 again
    minimizer_hashmap_fragment_index idx;
    idx.build(minimizers, 256);
    return idx;
}

/// Restore the process-wide index-memory cap at scope exit.
class index_memory_cap_scope {
public:
  /// Install a temporary index-memory cap.
  explicit index_memory_cap_scope(std::uint64_t cap)
      : controller_(bioseq::parallelism::memory_admission_controller::instance()),
        previous_(controller_.index_memory_cap_bytes()) {
      controller_.set_index_memory_cap_bytes(cap);
  }
  /// Restore the cap observed by the constructor.
  ~index_memory_cap_scope() {
      controller_.set_index_memory_cap_bytes(previous_);
  }

private:
    bioseq::parallelism::memory_admission_controller& controller_;
    std::uint64_t previous_ = 0u;
};

/// Allocate a valid adoptable device index with optional query gating.
[[nodiscard]] static bioseq::gpu::vulkan::gpu_built_index
make_adoptable_gpu_built_index(
    bioseq::gpu::vulkan::runtime& runtime,
    bool gate_enabled,
    bool empty = false)
{
    constexpr VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bioseq::gpu::vulkan::gpu_built_index built{};
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
    auto permit = bioseq::parallelism::memory_admission_controller::instance()
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
            bioseq::gpu::vulkan::k_gpu_query_gate_word_count, 0u);
        (*gate_words)[0] = 1u;
        built.gate_identity =
            bioseq::gpu::vulkan::gpu_index_gate_identity{
                .enabled = true,
                .word_count =
                    bioseq::gpu::vulkan::k_gpu_query_gate_word_count,
                .mask = bioseq::gpu::vulkan::k_gpu_query_gate_mask,
                .digest_lo = 11u,
                .digest_hi = 12u,
                .canonical_words = std::move(gate_words),
            };
    }
    built.materialization = bioseq::parallelism::memory_materialization_result{
        .reflected_delta_bytes = 0u,
        .admitted_bytes = resident_bytes,
        .allocation_snapshot = bioseq::parallelism::memory_materialization_snapshot{
            .actual_allocation_bytes = resident_bytes,
            .fully_touched_bytes = resident_bytes,
            .is_host_visible = true,
            .is_host_coherent = true,
            .is_process_footprint_reflected = false,
        },
        .is_permit_released = false,
    };
    auto& stats = built.ownership;
    stats.count_submit_admissions = 1u;
    stats.execution_peak_admissions = 1u;
    stats.exact_count_dispatches = 1u;
    stats.exact_count_prefix_scan_dispatches = 3u;
    stats.exact_count_control_readbacks = 1u;
    stats.deterministic_tile_emit_dispatches = empty ? 0u : 1u;
    stats.atomic_append_dispatches = 0u;
    stats.emit_validation_control_readbacks = empty ? 0u : 1u;
    stats.counted_minimizer_count = built.position_count;
    stats.verified_emit_count = built.position_count;
    stats.count_emit_parity_verified = 1u;
    stats.target_minimizer_capacity_estimate = 4u;
    stats.capacity_estimate_underestimated = 0u;
    stats.idle_arena_trim_attempts_before_count = 1u;
    stats.idle_arena_trim_successes_before_count = 1u;
    stats.direct_sequence_arena_copies = 1u;
    stats.direct_tile_arena_fills = 1u;
    stats.host_tile_staging_elided_bytes = 24u;
    stats.count_submit_host_transient_bytes = 0u;
    stats.execution_submit_host_transient_bytes = 0u;
    stats.host_transient_released_before_execution_bytes = 24u;
    stats.arena_trimmed_before_permit_shrink = 1u;
    stats.resident_bytes = resident_bytes;
    stats.gate_count_dispatches = gate_enabled ? 1u : 0u;
    stats.gated_emit_dispatches = gate_enabled && !empty ? 1u : 0u;
    stats.ungated_emit_dispatches = !gate_enabled && !empty ? 1u : 0u;
    stats.query_gate_enabled = gate_enabled ? 1u : 0u;
    stats.query_gate_word_count = gate_enabled
        ? bioseq::gpu::vulkan::k_gpu_query_gate_word_count : 0u;
    stats.query_gate_mask = gate_enabled
        ? bioseq::gpu::vulkan::k_gpu_query_gate_mask : 0u;
    stats.query_gate_digest_lo = gate_enabled ? 11u : 0u;
    stats.query_gate_digest_hi = gate_enabled ? 12u : 0u;
    stats.retained_minimizer_count = built.position_count;
    stats.distinct_minimizer_count = built.occupied_slots;
    stats.peak_admitted_bytes = resident_bytes;
    stats.peak_observed_bytes = resident_bytes;
    stats.materialization_admitted_bytes = resident_bytes;
    stats.materialization_actual_allocation_bytes = resident_bytes;
    stats.materialization_fully_touched_bytes = resident_bytes;
    stats.materialization_host_visible = 1u;
    stats.materialization_host_coherent = 1u;
    return built;
}

/// Encode one unsigned fixture scalar in little-endian order.
template <class p_unsigned_integer>
requires (std::is_unsigned_v<p_unsigned_integer>)
static void store_test_le(
    std::span<std::byte> bytes,
    std::size_t offset,
    p_unsigned_integer value)
{
    if (offset > bytes.size() || sizeof(p_unsigned_integer) > bytes.size() - offset) {
        throw std::runtime_error("grouped-SoA fixture scalar write overflow");
    }
    for (std::size_t index = 0u; index < sizeof(p_unsigned_integer); ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            (value >> (index * 8u)) & static_cast<p_unsigned_integer>(0xffu));
    }
}

/// Own encoded grouped-SoA rows and their authenticated metadata.
struct grouped_soa_gpu_resident_fixture {
    std::array<std::vector<std::byte>, 2> control_bytes;
    std::array<std::vector<std::byte>, 2> position_bytes;
    std::array<idx_ns::grouped_soa_bucket_directory_entry, 2> entries;
    idx_ns::minimizer_generation_head head;
    idx_ns::minimizer_generation_manifest manifest;
    idx_ns::grouped_soa_directory_view directory;
};

/// Construct the identity expected by grouped-SoA GPU ingestion.
[[nodiscard]] constexpr idx_ns::expected_minimizer_generation_identity
grouped_soa_gpu_expected_identity() noexcept {
    return {
        .layout = idx_ns::minimizer_generation_layout::
            grouped_soa_segment_local,
        .alphabet_code = idx_ns::k_minimizer_persisted_dna4_alphabet_code,
        .storage_unit_byte_width = 8u,
        .kmer_size = 15u,
        .window_size = 10u,
        .gpu_abi_revision = idx_ns::k_grouped_soa_gpu_abi_revision,
        .minimizer_scheme_id = idx_ns::k_minimap_scheme_id,
        .minimizer_scheme_revision = idx_ns::k_minimap_scheme_revision,
        .producer_backend_id = idx_ns::minimizer_producer_backend::exact_simd,
        .producer_identity_revision =
            idx_ns::k_minimizer_producer_identity_revision,
        .max_occ_cap = 10u,
    };
}

/// Build two authenticated grouped-SoA partitions for GPU ingestion tests.
[[nodiscard]] static grouped_soa_gpu_resident_fixture
make_grouped_soa_gpu_resident_fixture() {
    grouped_soa_gpu_resident_fixture fixture;
    constexpr std::uint64_t generation_id = 505u;
    const auto profile = idx_ns::derive_grouped_soa_minimizer_profile(
        idx_ns::k_minimap_scheme_identity,
        idx_ns::k_minimizer_persisted_dna4_alphabet_code,
        15u, 32u, 10u);
    if (!profile) {
        throw std::runtime_error("failed to derive grouped-SoA GPU profile");
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
        const auto layout = idx_ns::plan_grouped_soa_control_plane(
            hashes[ordinal].size(),
            idx_ns::minimizer_scalar_width::u32,
            idx_ns::minimizer_scalar_width::u16,
            idx_ns::minimizer_scalar_width::u16);
        if (!layout) {
            throw std::runtime_error("failed to derive grouped-SoA control layout");
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
        fixture.entries[ordinal] = idx_ns::grouped_soa_bucket_directory_entry{
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
            .shape_offset_width = idx_ns::minimizer_scalar_width::u16,
        };
        control_count += hashes[ordinal].size();
        position_count += positions[ordinal].size();
        payload_bytes += controls.size() + encoded_positions.size();
    }

    const auto encoded_directory = idx_ns::encode_minimizer_bucket_directory(
        std::span<const idx_ns::grouped_soa_bucket_directory_entry>{fixture.entries},
        *profile);
    if (!encoded_directory) {
        throw std::runtime_error("failed to encode grouped-SoA GPU directory");
    }
    auto directory = idx_ns::grouped_soa_directory_view::decode(*encoded_directory, *profile);
    if (!directory) {
        throw std::runtime_error("failed to decode grouped-SoA GPU directory");
    }
    fixture.directory = std::move(*directory);

    idx_ns::crc64_ecma182_accumulator payload_row_checksums{};
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
            throw std::runtime_error("failed to derive grouped-SoA position key");
        }
        payload_row_checksums.update_little_endian(
            idx_ns::crc64_index_chunk_row(
                generation_id, entry.position_segment, *position_sub_key, 0u,
                static_cast<std::uint32_t>(
                    fixture.position_bytes[ordinal].size()),
                fixture.position_bytes[ordinal]));
    }

    fixture.manifest = idx_ns::minimizer_generation_manifest{
        .layout = idx_ns::minimizer_generation_layout::
            grouped_soa_segment_local,
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
        .alphabet_code = idx_ns::k_minimizer_persisted_dna4_alphabet_code,
        .storage_unit_byte_width = 8u,
        .kmer_size = 15u,
        .window_size = 10u,
        .gpu_abi_revision = idx_ns::k_grouped_soa_gpu_abi_revision,
        .position_segment_span_bp = 32u,
        .segment_geometry_revision = idx_ns::k_minimizer_segment_geometry_revision,
        .minimizer_scheme_id = idx_ns::k_minimap_scheme_id,
        .minimizer_scheme_revision = idx_ns::k_minimap_scheme_revision,
        .producer_backend_id = idx_ns::minimizer_producer_backend::exact_simd,
        .producer_identity_revision =
            idx_ns::k_minimizer_producer_identity_revision,
        .hash_width = profile->hash_width,
        .position_width = profile->position_width,
        .count_width = profile->count_width,
        .offset_policy = profile->offset_policy,
        .control_layout = profile->control_layout,
        .count_semantics = profile->count_semantics,
        .position_semantics = profile->position_semantics,
        .max_hash = profile->max_hash,
        .max_occ_cap = profile->max_occ_cap,
    };
    const auto encoded_manifest =
        idx_ns::encode_minimizer_generation_manifest(fixture.manifest);
    if (!encoded_manifest) {
        throw std::runtime_error("failed to encode grouped-SoA GPU manifest");
    }
    fixture.head = idx_ns::minimizer_generation_head{
        .layout = fixture.manifest.layout,
        .generation_id = fixture.manifest.generation_id,
        .source_identity = fixture.manifest.source_identity,
        .storage_contract_checksum =
            idx_ns::compute_minimizer_generation_storage_contract_checksum(
                fixture.manifest),
        .manifest_checksum = idx_ns::crc64_ecma182(
            std::span<const std::byte>{*encoded_manifest}),
        .source_content_checksum = fixture.manifest.source_content_checksum,
    };
    return fixture;
}

/// Construct a deterministic synthetic Vulkan device UUID.
[[nodiscard]] static bioseq::gpu::vulkan::gpu_device_uuid synthetic_device_uuid(
    std::uint8_t seed) noexcept
{
    bioseq::gpu::vulkan::gpu_device_uuid uuid{};
    for (std::size_t index = 0; index < uuid.size(); ++index) {
        uuid[index] = static_cast<std::uint8_t>(seed + index);
    }
    return uuid;
}

TEST_CASE("vulkan index: GPU slot table resolves the same hits as CPU lookup",
          "[component:bioseq][layer:unit][feature:index]") {
    const auto idx = build_demo_index();
    const auto upload = idx_ns::build_gpu_minimizer_upload(idx);

    idx.for_each_table_entry([&idx, &upload](std::uint64_t kmer, std::uint64_t) {
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
          "[component:bioseq][layer:unit][feature:index]") {
    constexpr std::uint64_t positions = 93'308'896u;
    constexpr std::uint64_t expected_capacity = std::uint64_t{1} << 27;
    const std::uint64_t capacity = idx_ns::gpu_slot_capacity(positions);
    REQUIRE(capacity == expected_capacity);
    REQUIRE(idx_ns::gpu_index_load_ppm(positions, capacity) <= 700'000u);
    REQUIRE(capacity * sizeof(idx_ns::gpu_minimizer_slot) == 2'147'483'648u);
    REQUIRE(positions * sizeof(std::uint64_t) == 746'471'168u);
    REQUIRE(capacity * sizeof(idx_ns::gpu_minimizer_slot)
            + positions * sizeof(std::uint64_t) == 2'893'954'816u);

    constexpr std::uint64_t max_distinct_for_2p27 =
        ((std::uint64_t{1} << 27) * 7u) / 10u;
    REQUIRE(idx_ns::gpu_slot_capacity(max_distinct_for_2p27)
            == expected_capacity);
    REQUIRE(idx_ns::gpu_slot_capacity(max_distinct_for_2p27 + 1u)
            == (std::uint64_t{1} << 28));
}

TEST_CASE("vulkan index: storage-range preflight accepts equality and rejects +1/unknown",
          "[component:bioseq][layer:unit][feature:index]") {
    constexpr std::uint64_t limit = 4'294'967'295u;
    REQUIRE(idx_ns::gpu_index_storage_ranges_fit(limit, 8u, limit));
    REQUIRE_FALSE(idx_ns::gpu_index_storage_ranges_fit(limit + 1u, 8u, limit));
    REQUIRE_FALSE(idx_ns::gpu_index_storage_ranges_fit(16u, limit + 1u, limit));
    REQUIRE_FALSE(idx_ns::gpu_index_storage_ranges_fit(16u, 8u, 0u));
}

TEST_CASE("vulkan index: direct fill preserves collision and wraparound parity",
          "[component:bioseq][layer:unit][feature:index]") {
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
    minimizer_hashmap_fragment_index idx;
    idx.build_from_pairs(pairs, 100u);
    const std::uint32_t capacity = idx_ns::gpu_slot_capacity(idx.table_size());
    REQUIRE(capacity == 8u);
    std::vector<idx_ns::gpu_minimizer_slot> final_slots(capacity);
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
            std::span<const idx_ns::gpu_minimizer_slot>{final_slots},
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
          "[component:bioseq][layer:unit][feature:index]") {
    auto idx = build_demo_index();
    std::vector<idx_ns::gpu_minimizer_slot> final_slots(idx.table_size());
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
          "[component:bioseq][layer:unit][feature:index]") {
    constexpr std::uint64_t unique_keys = 70'249'364u;
    constexpr std::uint64_t positions = 93'308'896u;
    const auto open = idx_ns::plan_gpu_index_materialization(
        unique_keys, positions,
        idx_ns::gpu_index_representation::open_addressed_70);
    const auto dense = idx_ns::plan_gpu_index_materialization(
        unique_keys, positions,
        idx_ns::gpu_index_representation::source_direct_sorted_dense);

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
          "[component:bioseq][layer:unit][feature:index]") {
    auto idx_a = build_demo_index();
    auto idx_b = build_demo_index();  // identical content, different object

    const auto key_a = idx_ns::make_gpu_index_cache_key(idx_a, idx_ns::gpu_index_cache_identity{});
    const auto key_a2 = idx_ns::make_gpu_index_cache_key(idx_a, idx_ns::gpu_index_cache_identity{});
    const auto key_b = idx_ns::make_gpu_index_cache_key(idx_b, idx_ns::gpu_index_cache_identity{});

    ASSERT(key_a == key_a2, "same in-memory index must produce the same key", 0);
    ASSERT(!(key_a == key_b),
           "distinct in-memory index objects must produce distinct keys", 0);
}

TEST_CASE("vulkan index: distinct assembly identities do not share a cache key",
          "[component:bioseq][layer:unit][feature:index]") {
    // Two identical local segment indexes with different add_position_offset:
    // they must carry distinct explicit assembly_identity values and key apart.
    auto seg_lo = build_demo_index();
    auto seg_hi = build_demo_index();
    seg_hi.add_position_offset(1'000'000);  // global offset for a later segment

    const auto key_lo = idx_ns::make_gpu_index_cache_key(
        seg_lo, idx_ns::gpu_index_cache_identity{.assembly_identity = 0xA11CEULL});
    const auto key_hi = idx_ns::make_gpu_index_cache_key(
        seg_hi, idx_ns::gpu_index_cache_identity{.assembly_identity = 0xB0B0ULL});

    ASSERT(!(key_lo == key_hi),
           "different assembly identities must not share a cache key", 0);
    // The same identity on the same index must be reproducible.
    const auto key_lo2 = idx_ns::make_gpu_index_cache_key(
        seg_lo, idx_ns::gpu_index_cache_identity{.assembly_identity = 0xA11CEULL});
    ASSERT(key_lo == key_lo2, "assembly key must be reproducible", 0);

    idx_ns::gpu_minimizer_index_cache_key_hash hasher;
    ASSERT(hasher(key_lo) != hasher(key_hi) || !(key_lo == key_hi),
           "distinct assembly keys collide on both equality and hash", 0);
}

TEST_CASE("vulkan index: query-gate cache identity compares canonical words after digest",
          "[component:bioseq][layer:unit][feature:index]") {
    using cache_type = idx_ns::gpu_minimizer_index_cache;
    cache_type cache{std::uint64_t{64} << 20};
    auto words_a_mut = std::make_shared<std::vector<std::uint32_t>>(
        bioseq::gpu::vulkan::k_gpu_query_gate_word_count, 0u);
    (*words_a_mut)[0] = 0x1u;
    (*words_a_mut)[1] = 0x20u;
    auto words_b_mut =
        std::make_shared<std::vector<std::uint32_t>>(*words_a_mut);
    (*words_b_mut)[2] = 0x301u;
    const std::shared_ptr<const std::vector<std::uint32_t>> words_a =
        std::move(words_a_mut);
    const std::shared_ptr<const std::vector<std::uint32_t>> words_b =
        std::move(words_b_mut);
    const cache_type::gate_identity first{
        .enabled = true,
        .word_count = bioseq::gpu::vulkan::k_gpu_query_gate_word_count,
        .mask = bioseq::gpu::vulkan::k_gpu_query_gate_mask,
        .digest_lo = 0x1111222233334444ULL,
        .digest_hi = 0x5555666677778888ULL,
        .canonical_words = words_a,
    };
    auto forced_collision = first;
    forced_collision.canonical_words = words_b;
    REQUIRE(cache.bind_gate_once(first).has_value());
    const auto rejected = cache.bind_gate_once(forced_collision);
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error() == cache_type::gate_bind_error::identity_mismatch);

    cache_type zero_cache{std::uint64_t{64} << 20};
    const auto zero_words =
        std::make_shared<const std::vector<std::uint32_t>>(
            bioseq::gpu::vulkan::k_gpu_query_gate_word_count, 0u);
    REQUIRE(zero_cache.bind_gate_once(cache_type::gate_identity{
        .enabled = true,
        .word_count = bioseq::gpu::vulkan::k_gpu_query_gate_word_count,
        .mask = bioseq::gpu::vulkan::k_gpu_query_gate_mask,
        .digest_lo = 9u,
        .digest_hi = 10u,
        .canonical_words = zero_words,
    }).has_value());
    const auto disabled = zero_cache.bind_gate_once(cache_type::gate_identity{});
    REQUIRE_FALSE(disabled.has_value());
    REQUIRE(disabled.error() == cache_type::gate_bind_error::identity_mismatch);
}

TEST_CASE("vulkan index: every cache API rejects an unbound gated key",
          "[component:bioseq][layer:unit][feature:index]") {
    using cache_type = idx_ns::gpu_minimizer_index_cache;
    cache_type cache{std::uint64_t{64} << 20};
    idx_ns::gpu_minimizer_index_cache_key gated_key{};
    gated_key.gate_enabled = true;
    gated_key.gate_word_count = 1u;
    gated_key.gate_mask = 31u;
    gated_key.gate_digest_lo = 11u;
    gated_key.gate_digest_hi = 12u;

    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    auto idx = build_demo_index();
    REQUIRE_THROWS_AS(
        cache.get_or_upload(
            runtime, idx, gated_key, /*cache_enabled=*/false),
        bioseq::gpu::vulkan::vulkan_runtime_error);
    REQUIRE_THROWS_AS(
        cache.insert_upload(
            runtime, idx_ns::gpu_minimizer_index_upload{}, gated_key,
            /*cache_enabled=*/false),
        bioseq::gpu::vulkan::vulkan_runtime_error);
    auto empty_handle = std::make_shared<idx_ns::gpu_resident_minimizer_index>();
    const auto built_insert = cache.insert_built(gated_key, empty_handle);
    REQUIRE_FALSE(built_insert.has_value());
    REQUIRE(built_insert.error()
            == cache_type::built_insert_error::gate_identity_mismatch);

    const auto truncated_words =
        std::make_shared<const std::vector<std::uint32_t>>(1u, 1u);
    const auto malformed = cache.bind_gate_once(cache_type::gate_identity{
        .enabled = true,
        .word_count = 1u,
        .mask = 31u,
        .digest_lo = 1u,
        .digest_hi = 2u,
        .canonical_words = truncated_words,
    });
    REQUIRE_FALSE(malformed.has_value());
    REQUIRE(malformed.error() == cache_type::gate_bind_error::invalid_identity);
}

TEST_CASE("vulkan index: typed upload decline preserves every predicate and admission field",
          "[component:bioseq][layer:unit][feature:index]") {
    const bioseq::parallelism::memory_admission_failure admission{
        .reason = bioseq::parallelism::memory_admission_failure_reason::
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
            idx_ns::gpu_index_upload_decline_reason::
                resident_memory_admission_denied,
            idx_ns::gpu_index_upload_decline_reason::
                upload_peak_memory_admission_denied}) {
        const idx_ns::gpu_index_upload_decline decline{
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
            idx_ns::gpu_index_upload_decline_reason::device_abi_overflow,
            idx_ns::gpu_index_upload_decline_reason::
                non_host_visible_device_memory}) {
        const idx_ns::gpu_index_upload_decline decline{
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
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    auto idx = build_demo_index();
    idx_ns::gpu_minimizer_index_cache cache{std::uint64_t{64} << 20};
    const auto key = idx_ns::make_gpu_index_cache_key(idx, idx_ns::gpu_index_cache_identity{});

    idx_ns::gpu_index_upload_decline diagnostic{};
    auto handle1 = cache.get_or_upload(
        runtime, idx, key, /*cache_enabled=*/true, &diagnostic);
    ASSERT(handle1 != nullptr, "resident upload returned nullptr on a usable device", 0);
    ASSERT(handle1->target_len == idx.target_len, "resident target_len mismatch", 0);
    ASSERT(handle1->position_count == idx.positions.size(),
           "resident position_count mismatch", 0);
    REQUIRE(diagnostic.reason == idx_ns::gpu_index_upload_decline_reason::none);
    REQUIRE(diagnostic.unique_key_count == idx.table_size());
    REQUIRE(diagnostic.slot_capacity == idx_ns::gpu_slot_capacity(idx.table_size()));
    REQUIRE(diagnostic.load_ppm <= 700'000u);
    REQUIRE(diagnostic.max_probe == handle1->max_probe);
    REQUIRE(diagnostic.staging_bytes == 0u);
    REQUIRE(diagnostic.upload_peak_bytes == 0u);
    REQUIRE(handle1->representation
            == idx_ns::gpu_index_representation::open_addressed_70);
    REQUIRE(handle1->unique_key_count == idx.table_size());
    REQUIRE(handle1->slot_capacity == diagnostic.slot_capacity);
    REQUIRE(handle1->materialization.admitted_bytes
            == handle1->slots.allocation_bytes
                + handle1->positions.allocation_bytes);
    REQUIRE(handle1->materialization.allocation_snapshot.actual_allocation_bytes
            == handle1->materialization.admitted_bytes);
    REQUIRE(diagnostic.actual_allocation_bytes
            == handle1->materialization.allocation_snapshot.actual_allocation_bytes);
    REQUIRE(diagnostic.fully_touched_bytes
            == handle1->materialization.allocation_snapshot.fully_touched_bytes);
    REQUIRE(diagnostic.reflected_footprint_delta_bytes
            == handle1->materialization.reflected_delta_bytes);
    REQUIRE(diagnostic.materialization_permit_released
            == handle1->materialization.is_permit_released);
    REQUIRE(diagnostic.process_footprint_reflected
            == handle1->materialization.allocation_snapshot.is_process_footprint_reflected);
    if (handle1->materialization.is_permit_released) {
        REQUIRE(handle1->permit.bytes() == 0u);
        REQUIRE(handle1->materialization.reflected_delta_bytes
                >= handle1->materialization.admitted_bytes);
    } else {
        REQUIRE(handle1->permit.bytes()
                == handle1->materialization.admitted_bytes);
    }

    idx_ns::gpu_index_upload_decline cache_hit_diagnostic{};
    auto handle2 = cache.get_or_upload(
        runtime, idx, key, /*cache_enabled=*/true, &cache_hit_diagnostic);
    ASSERT(handle2.get() == handle1.get(),
           "cache must return the same resident handle for a repeated key", 0);
    REQUIRE(cache_hit_diagnostic.reason
            == idx_ns::gpu_index_upload_decline_reason::none);
    REQUIRE(cache_hit_diagnostic.slot_capacity == 0u);
    REQUIRE(cache_hit_diagnostic.actual_allocation_bytes == 0u);

    cache.clear();
}

TEST_CASE("vulkan index: admission cliff selects and attests sorted-dense device layout",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
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
    minimizer_hashmap_fragment_index idx;
    idx.build_from_pairs(pairs, 1'000'000u);
    const auto open = idx_ns::plan_gpu_index_materialization(
        idx.table_size(), idx.positions.size(),
        idx_ns::gpu_index_representation::open_addressed_70);
    const auto dense = idx_ns::plan_gpu_index_materialization(
        idx.table_size(), idx.positions.size(),
        idx_ns::gpu_index_representation::source_direct_sorted_dense);
    REQUIRE(dense.resident_bytes + (std::uint64_t{1} << 20)
            < open.resident_bytes);

    index_memory_cap_scope compact_only_cap{
        dense.resident_bytes + (std::uint64_t{1} << 20)};
    idx_ns::gpu_minimizer_index_cache cache{std::uint64_t{16} << 20};
    const auto key = idx_ns::make_gpu_index_cache_key(
        idx, idx_ns::gpu_index_cache_identity{});
    idx_ns::gpu_index_upload_decline diagnostic{};
    auto handle = cache.get_or_upload(
        runtime, idx, key, /*cache_enabled=*/true, &diagnostic);

    REQUIRE(handle != nullptr);
    REQUIRE(diagnostic.reason == idx_ns::gpu_index_upload_decline_reason::none);
    REQUIRE(diagnostic.representation
            == idx_ns::gpu_index_representation::source_direct_sorted_dense);
    REQUIRE(diagnostic.resident_bytes == dense.resident_bytes);
    REQUIRE(diagnostic.slot_capacity == distinct);
    REQUIRE(diagnostic.load_ppm == 1'000'000u);
    REQUIRE(diagnostic.max_probe == 0u);
    REQUIRE(diagnostic.staging_bytes == 0u);
    REQUIRE(handle->representation
            == idx_ns::gpu_index_representation::source_direct_sorted_dense);
    REQUIRE(handle->slot_mask == 0u);
    REQUIRE(handle->bucket_count == distinct);
    REQUIRE(handle->unique_key_count == distinct);
    REQUIRE(handle->slot_capacity == distinct);

    const auto mapped_slots = std::span<const idx_ns::gpu_minimizer_slot>{
        static_cast<const idx_ns::gpu_minimizer_slot*>(handle->slots.mapped),
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
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t before = admission.reserved_bytes();
    auto built = make_adoptable_gpu_built_index(
        runtime, /*gate_enabled=*/false);
    const std::uint64_t with_built = admission.reserved_bytes();
    REQUIRE(with_built > before);
    index_memory_cap_scope tiny_cap{1u};
    auto adopted = idx_ns::adopt_gpu_built_index(
        runtime, std::move(built), idx_ns::gpu_minimizer_index_cache_key{});
    REQUIRE(adopted.has_value());
    REQUIRE((*adopted)->ownership.resident_transfers == 1u);
    REQUIRE((*adopted)->ownership.adoption_rereserve_attempts == 0u);
    REQUIRE((*adopted)->ownership.adoption_successes == 1u);
    REQUIRE(admission.reserved_bytes() == with_built);
    REQUIRE_FALSE(built.valid);
    REQUIRE_FALSE(built.slots.is_valid());
    REQUIRE_FALSE(built.positions.is_valid());
    adopted->reset();
    REQUIRE(admission.reserved_bytes() == before);
}

TEST_CASE("vulkan index: GPU-built move assignment releases destination after buffers",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<
        bioseq::gpu::vulkan::gpu_built_index>);
    STATIC_REQUIRE(std::is_nothrow_move_assignable_v<
        bioseq::gpu::vulkan::gpu_built_index>);
    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
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
        REQUIRE_FALSE(source.slots.is_valid());
        REQUIRE_FALSE(source.positions.is_valid());
        REQUIRE_FALSE(static_cast<bool>(source.permit));
        REQUIRE(admission.reserved_bytes() == before + one_resident);
    }
    REQUIRE(admission.reserved_bytes() == before);
}

TEST_CASE("vulkan index: adoption rejects inconsistent stats without consuming source",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto built = make_adoptable_gpu_built_index(
        runtime, /*gate_enabled=*/false);
    built.materialization.allocation_snapshot.fully_touched_bytes--;
    built.ownership.materialization_fully_touched_bytes--;
    auto adopted = idx_ns::adopt_gpu_built_index(
        runtime, std::move(built), idx_ns::gpu_minimizer_index_cache_key{});
    REQUIRE_FALSE(adopted.has_value());
    REQUIRE(adopted.error()
            == idx_ns::gpu_built_index_adoption_error::stats_mismatch);
    REQUIRE(built.valid);
    REQUIRE(built.slots.is_valid());
    REQUIRE(built.positions.is_valid());
    REQUIRE(static_cast<bool>(built.permit));

    built.materialization.allocation_snapshot.fully_touched_bytes++;
    built.ownership.materialization_fully_touched_bytes++;
    built.occupied_slots = built.position_count + 1u;
    built.ownership.distinct_minimizer_count = built.occupied_slots;
    adopted = idx_ns::adopt_gpu_built_index(
        runtime, std::move(built), idx_ns::gpu_minimizer_index_cache_key{});
    REQUIRE_FALSE(adopted.has_value());
    REQUIRE(adopted.error() == idx_ns::gpu_built_index_adoption_error::invalid_build);
    REQUIRE(built.valid);
    REQUIRE(built.slots.is_valid());
    REQUIRE(built.positions.is_valid());

    built.occupied_slots = built.position_count;
    built.ownership.distinct_minimizer_count = built.occupied_slots;
    built.target_len = built.position_count - 1u;
    adopted = idx_ns::adopt_gpu_built_index(
        runtime, std::move(built), idx_ns::gpu_minimizer_index_cache_key{});
    REQUIRE_FALSE(adopted.has_value());
    REQUIRE(adopted.error() == idx_ns::gpu_built_index_adoption_error::invalid_build);
    REQUIRE(built.valid);
}

TEST_CASE("vulkan index: adoption rejects tampered deterministic target-build stats",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    constexpr std::uint32_t stats_field_count = 12u;
    for (std::uint32_t field = 0u; field < stats_field_count; ++field) {
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
            default: FAIL("unreachable stats field");
        }
        INFO("tampered stats field ordinal=" << field);
        auto adopted = idx_ns::adopt_gpu_built_index(
            runtime, std::move(built), idx_ns::gpu_minimizer_index_cache_key{});
        REQUIRE_FALSE(adopted.has_value());
        REQUIRE(adopted.error()
                == idx_ns::gpu_built_index_adoption_error::stats_mismatch);
        REQUIRE(built.valid);
        REQUIRE(built.slots.is_valid());
        REQUIRE(built.positions.is_valid());
    }
}

TEST_CASE("vulkan index: ungated T-zero sentinel adopts without an emit dispatch",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto built = make_adoptable_gpu_built_index(
        runtime, /*gate_enabled=*/false, /*empty=*/true);
    REQUIRE(built.ownership.ungated_emit_dispatches == 0u);
    REQUIRE(built.ownership.deterministic_tile_emit_dispatches == 0u);
    auto adopted = idx_ns::adopt_gpu_built_index(
        runtime, std::move(built), idx_ns::gpu_minimizer_index_cache_key{});
    REQUIRE(adopted.has_value());
    REQUIRE((*adopted)->position_count == 0u);
    REQUIRE((*adopted)->bucket_count == 0u);
    REQUIRE((*adopted)->ownership.counted_minimizer_count == 0u);
    REQUIRE((*adopted)->ownership.verified_emit_count == 0u);
}

TEST_CASE("vulkan index: released materialization and gated T-zero sentinel adopt",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto built = make_adoptable_gpu_built_index(
        runtime, /*gate_enabled=*/true, /*empty=*/true);
    const auto bytes = built.ownership.resident_bytes;
    built.permit.release();
    built.materialization.is_permit_released = true;
    built.materialization.allocation_snapshot.is_process_footprint_reflected = true;
    built.materialization.reflected_delta_bytes = bytes;
    built.ownership.materialization_permit_released = 1u;
    built.ownership.materialization_process_footprint_reflected = 1u;
    built.ownership.materialization_reflected_delta_bytes = bytes;
    const auto gate_identity = built.gate_identity;
    idx_ns::gpu_minimizer_index_cache_key gated_key{};
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
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto built = make_adoptable_gpu_built_index(
        runtime, /*gate_enabled=*/true);
    auto colliding_words = std::make_shared<std::vector<std::uint32_t>>(
        *built.gate_identity.canonical_words);
    (*colliding_words)[7] ^= 1u;
    idx_ns::gpu_minimizer_index_cache_key colliding_key{
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
            == idx_ns::gpu_built_index_adoption_error::stats_mismatch);
    REQUIRE(built.valid);
    REQUIRE(built.slots.is_valid());
    REQUIRE(built.positions.is_valid());
    REQUIRE(static_cast<bool>(built.permit));
}

TEST_CASE("vulkan index: Gate-B-bound cache rejects adopted Gate-A resident on digest collision",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto built = make_adoptable_gpu_built_index(
        runtime, /*gate_enabled=*/true);
    const auto gate_a = built.gate_identity;
    idx_ns::gpu_minimizer_index_cache_key key_a{
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
    using cache_type = idx_ns::gpu_minimizer_index_cache;
    cache_type cache{std::uint64_t{64} << 20};
    REQUIRE(cache.bind_gate_once(cache_type::gate_identity{
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
    REQUIRE(inserted.error() == cache_type::built_insert_error::gate_identity_mismatch);
}

TEST_CASE("vulkan index: cache cannot publish a resident under another target key",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto built = make_adoptable_gpu_built_index(
        runtime, /*gate_enabled=*/false);
    idx_ns::gpu_minimizer_index_cache_key key_a{.source_id = 101u};
    auto adopted = idx_ns::adopt_gpu_built_index(
        runtime, std::move(built), key_a);
    REQUIRE(adopted.has_value());
    auto resident = std::move(*adopted);
    auto key_b = key_a;
    key_b.source_id = 202u;
    idx_ns::gpu_minimizer_index_cache cache{std::uint64_t{64} << 20};
    const auto inserted = cache.insert_built(key_b, resident);
    REQUIRE_FALSE(inserted.has_value());
    REQUIRE(inserted.error()
            == idx_ns::gpu_minimizer_index_cache::built_insert_error::
                resident_key_mismatch);
}

TEST_CASE("grouped-SoA cold resident preserves raw SoA and local positions",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    STATIC_REQUIRE_FALSE(
        idx_ns::gpu_grouped_soa_resident_target::permits_host_decode_or_rebuild());
    STATIC_REQUIRE_FALSE(
        idx_ns::gpu_grouped_soa_resident_target::qualifies_as_all_gpu_execution());
    const auto fixture = make_grouped_soa_gpu_resident_fixture();
    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) return;

    constexpr std::uint64_t cold_load_identity = 0xC01DCAC4EULL;
    auto ingest = idx_ns::gpu_grouped_soa_generation_ingest::begin(
        runtime, fixture.head, fixture.manifest,
        grouped_soa_gpu_expected_identity(), fixture.directory,
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
        const idx_ns::index_chunk_address address{
                .generation_id = requests[index].generation_id,
                .position_segment = requests[index].position_segment,
                .sub_key = requests[index].sub_key,
                .chunk_no = requests[index].first_chunk_no,
            };
        auto receipt = idx_ns::validate_minimizer_generation_row_receipt(
            idx_ns::minimizer_generation_row_view{
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
    REQUIRE(resident.identity().gpu_abi_revision
        == idx_ns::k_grouped_soa_gpu_abi_revision);
    REQUIRE(resident.control_count() == fixture.manifest.control_count);
    REQUIRE(resident.position_count() == fixture.manifest.position_count);
    REQUIRE(resident.descriptor_count() == fixture.entries.size());
    REQUIRE(resident.directory_bytes()
        == fixture.entries.size()
            * idx_ns::grouped_soa_bucket_directory_entry::encoded_bytes);

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
        bioseq::gpu::vulkan::gpu_grouped_soa_resident_partition_descriptor*>(
            resident.descriptors().mapped);
    REQUIRE(descriptors != nullptr);
    REQUIRE(descriptors[0].flags
        == bioseq::gpu::vulkan::k_gpu_grouped_soa_partition_required_flags);
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
    const auto& stats = resident.cold_load_stats();
    REQUIRE(stats.cold_cache_misses == 1u);
    REQUIRE(stats.full_generations_ingested == 1u);
    REQUIRE(stats.control_bytes_copied == expected_controls.size());
    REQUIRE(stats.position_bytes_copied == expected_positions.size());
    REQUIRE(stats.descriptor_bytes_copied
        == fixture.entries.size()
            * sizeof(bioseq::gpu::vulkan::
                gpu_grouped_soa_resident_partition_descriptor));
    REQUIRE(stats.device_allocation_bytes == actual_allocations);
    REQUIRE(stats.admitted_device_bytes == actual_allocations);
    REQUIRE(stats.validated_receipt_rows == requests.size());
    REQUIRE(stats.raw_rehashed_rows == 0u);
    REQUIRE(stats.payload_hash_byte_visits == 0u);
    REQUIRE_FALSE(stats.has_forbidden_work());
}

TEST_CASE("grouped-SoA cold resident fails closed before publication",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    const auto fixture = make_grouped_soa_gpu_resident_fixture();
    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) return;

    const auto missing_cold_identity =
        idx_ns::gpu_grouped_soa_generation_ingest::begin(
            runtime, fixture.head, fixture.manifest,
            grouped_soa_gpu_expected_identity(), fixture.directory, 0u);
    REQUIRE_FALSE(missing_cold_identity.has_value());
    REQUIRE(missing_cold_identity.error()
        == idx_ns::gpu_grouped_soa_ingest_error::invalid_identity);

    auto wrong_cap = grouped_soa_gpu_expected_identity();
    wrong_cap.max_occ_cap = 11u;
    const auto relabelled = idx_ns::gpu_grouped_soa_generation_ingest::begin(
        runtime, fixture.head, fixture.manifest, wrong_cap,
        fixture.directory, 1u);
    REQUIRE_FALSE(relabelled.has_value());
    REQUIRE(relabelled.error()
        == idx_ns::gpu_grouped_soa_ingest_error::metadata_mismatch);

    auto ingest = idx_ns::gpu_grouped_soa_generation_ingest::begin(
        runtime, fixture.head, fixture.manifest,
        grouped_soa_gpu_expected_identity(), fixture.directory, 2u);
    REQUIRE(ingest.has_value());
    auto corrupt = fixture.control_bytes[0];
    corrupt.front() ^= std::byte{1};
    const auto& request = ingest->requests().front();
    const auto accepted = ingest->accept_raw_chunk_for_test(
        idx_ns::index_chunk_address{
            .generation_id = request.generation_id,
            .position_segment = request.position_segment,
            .sub_key = request.sub_key,
            .chunk_no = request.first_chunk_no,
        },
        corrupt);
    REQUIRE_FALSE(accepted.has_value());
    REQUIRE(accepted.error()
        == idx_ns::gpu_grouped_soa_ingest_error::checksum);
    const auto unpublished = ingest->finish();
    REQUIRE_FALSE(unpublished.has_value());
    REQUIRE(unpublished.error()
        == idx_ns::gpu_grouped_soa_ingest_error::checksum);
}

TEST_CASE("vulkan source-direct preflight uses exact allocation requirements",
          "[component:bioseq][layer:unit][feature:index]") {
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
            == idx_ns::gpu_source_direct_materialization_error::
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
                == idx_ns::gpu_source_direct_materialization_error::invalid_shape);
    }
    const auto invalid_empty_requirement =
        idx_ns::plan_gpu_source_direct_sorted_dense_with_requirements(
            0u, 0u, 4096u, 1u, 0u);
    REQUIRE_FALSE(invalid_empty_requirement.has_value());
    REQUIRE(invalid_empty_requirement.error()
            == idx_ns::gpu_source_direct_materialization_error::invalid_shape);

    constexpr std::uint64_t u32_max =
        (std::numeric_limits<std::uint32_t>::max)();
    const auto range_overflow =
        idx_ns::plan_gpu_source_direct_sorted_dense_with_requirements(
            u32_max + 1u, 1u,
            (std::numeric_limits<std::uint64_t>::max)(), 16u, 8u);
    REQUIRE_FALSE(range_overflow.has_value());
    REQUIRE(range_overflow.error()
            == idx_ns::gpu_source_direct_materialization_error::
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
            == idx_ns::gpu_source_direct_materialization_error::
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

    auto& runtime = bioseq::gpu::vulkan::runtime::instance();
    bioseq::parallelism::memory_permit no_permit{};
    const auto ineligible =
        idx_ns::materialize_sorted_dense_from_presorted_minimizers(
            runtime,
            std::span<const bioseq::minimizers::
                minimizer_representation<28u, 30u>>{},
            /*target_len=*/0u,
            idx_ns::gpu_source_direct_preflight{},
            idx_ns::gpu_minimizer_index_cache_key{},
            no_permit);
    REQUIRE_FALSE(ineligible.has_value());
    REQUIRE(ineligible.error().error
            == idx_ns::gpu_source_direct_materialization_error::
                representation_ineligible);
}

TEST_CASE("vulkan source-direct fill consumes each source position once",
          "[component:bioseq][layer:unit][feature:index]") {
    struct single_read_minimizer {
        std::uint64_t hash = 0u;
        std::uint64_t encoded = 0u;
        mutable std::uint32_t reads = 0u;

        [[nodiscard]] std::uint64_t kmer() const noexcept { return hash; }
        [[nodiscard]] std::uint64_t pos_strand_encoded() const {
            if (++reads != 1u) throw std::runtime_error("source reread");
            return encoded;
        }
    };
    std::vector<single_read_minimizer> source{
        {.hash = 10u, .encoded = 4u},
        {.hash = 10u, .encoded = 7u},
        {.hash = 20u, .encoded = 12u},
    };
    std::array<idx_ns::gpu_minimizer_slot, 2u> controls{};
    std::array<std::uint64_t, 3u> positions{};
    const auto fill_stats =
        idx_ns::direct_fill_gpu_minimizer_index_from_presorted_source(
            std::span<const single_read_minimizer>{source},
            /*target_len=*/100u,
            idx_ns::gpu_source_direct_sorted_shape{
                .minimizer_count = source.size(),
                .distinct_count = controls.size(),
            },
            std::span{controls},
            std::span{positions});
    REQUIRE(fill_stats.source_checksum == fill_stats.mapped_checksum);
    REQUIRE(fill_stats.complete_source_traversals == 1u);
    REQUIRE(fill_stats.complete_mapped_traversals == 1u);
    REQUIRE(std::all_of(source.begin(), source.end(),
        [](const single_read_minimizer& minimizer) {
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
    minimizer_hashmap_fragment_index oracle;
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

    std::vector<single_read_minimizer> unsorted{
        {.hash = 20u, .encoded = 12u},
        {.hash = 10u, .encoded = 4u},
    };
    const auto rejected_shape = idx_ns::inspect_presorted_minimizer_source(
        std::span<const single_read_minimizer>{unsorted});
    REQUIRE_FALSE(rejected_shape.has_value());
    REQUIRE(rejected_shape.error()
            == idx_ns::gpu_source_direct_materialization_error::invalid_shape);

    constexpr std::uint64_t large_position =
        (std::uint64_t{1} << 62u) - 1u;
    std::vector<single_read_minimizer> boundary_source{
        {.hash = 99u, .encoded = (large_position << 1u) | 1u},
    };
    std::array<idx_ns::gpu_minimizer_slot, 1u> boundary_control{};
    std::array<std::uint64_t, 1u> boundary_position{};
    const auto boundary =
        idx_ns::direct_fill_gpu_minimizer_index_from_presorted_source(
            std::span<const single_read_minimizer>{boundary_source},
            /*target_len=*/large_position + 1u,
            idx_ns::gpu_source_direct_sorted_shape{
                .minimizer_count = 1u,
                .distinct_count = 1u,
            },
            std::span{boundary_control},
            std::span{boundary_position});
    REQUIRE(boundary.source_checksum == boundary.mapped_checksum);
    REQUIRE(boundary_position[0] == (large_position << 1u) + 1u);

    std::vector<single_read_minimizer> out_of_range{
        {.hash = 1u, .encoded = (100u << 1u)},
    };
    std::array<idx_ns::gpu_minimizer_slot, 1u> rejected_control{};
    std::array<std::uint64_t, 1u> rejected_position{};
    REQUIRE_THROWS_AS(
        idx_ns::direct_fill_gpu_minimizer_index_from_presorted_source(
            std::span<const single_read_minimizer>{out_of_range},
            /*target_len=*/100u,
            idx_ns::gpu_source_direct_sorted_shape{
                .minimizer_count = 1u,
                .distinct_count = 1u,
            },
            std::span{rejected_control},
            std::span{rejected_position}),
        idx_ns::gpu_index_direct_fill_invariant);
}

TEST_CASE("vulkan source-direct cache keys cannot upload or replace",
          "[component:bioseq][layer:unit][feature:index]") {
    using cache_type = idx_ns::gpu_minimizer_index_cache;
    cache_type cache{0u};
    idx_ns::gpu_index_cache_identity identity{
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
        .sorted_dense_abi_revision =
            idx_ns::k_minimizer_sorted_dense_gpu_abi_revision,
        .representation = idx_ns::gpu_index_representation::
            source_direct_sorted_dense,
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
    rejects_identity([](auto& value) { ++value.sorted_dense_abi_revision; });
    rejects_identity([](auto& value) {
        value.representation = idx_ns::gpu_index_representation::
            open_addressed_70;
    });
    rejects_identity([](auto& value) { value.device_uuid = {}; });
    rejects_identity([](auto& value) {
        value.source_direct_sorted_dense = false;
    });
    rejects_identity([](auto& value) { value.canonical_empty = true; });
    rejects_identity([](auto& value) { value.gate_enabled = true; });

    minimizer_hashmap_fragment_index placeholder;
    placeholder.target_len = 100u;
    const auto key = idx_ns::make_gpu_index_cache_key(
        placeholder, identity);
    const auto handle =
        std::make_shared<idx_ns::gpu_resident_minimizer_index>();
    handle->key = key;
    handle->gate_identity = idx_ns::gpu_index_gate_identity_from_key(key);
    REQUIRE(cache.insert_built(key, handle).has_value());
    const auto duplicate = cache.insert_built(key, handle);
    REQUIRE_FALSE(duplicate.has_value());
    REQUIRE(duplicate.error() == cache_type::built_insert_error::duplicate_direct_key);

    auto& runtime = bioseq::gpu::vulkan::runtime::instance();
    const auto cpu_index = build_demo_index();
    REQUIRE_THROWS_AS(
        cache.get_or_upload(
            runtime, cpu_index, key, /*cache_enabled=*/true),
        bioseq::gpu::vulkan::vulkan_runtime_error);

    auto zero_uuid_key = key;
    zero_uuid_key.device_uuid = {};
    const auto invalid_lookup = cache.find_required_preinserted(
        runtime, zero_uuid_key);
    REQUIRE_FALSE(invalid_lookup.has_value());
    REQUIRE(invalid_lookup.error()
            == cache_type::required_preinsert_lookup_error::invalid_key);

    auto runtime_identity = identity;
    runtime_identity.operation_identity = 0x5678u;
    runtime_identity.device_uuid = runtime.features().device_uuid;
    if (idx_ns::gpu_source_direct_identity_well_formed(runtime_identity)) {
        cache_type required_cache{0u};
        const auto runtime_key = idx_ns::make_gpu_index_cache_key(
            placeholder, runtime_identity);
        const auto miss = required_cache.find_required_preinserted(
            runtime, runtime_key);
        REQUIRE_FALSE(miss.has_value());
        REQUIRE(miss.error() == cache_type::required_preinsert_lookup_error::miss);
        REQUIRE(required_cache.resident_bytes() == 0u);

        const auto corrupt =
            std::make_shared<idx_ns::gpu_resident_minimizer_index>();
        corrupt->key = runtime_key;
        corrupt->gate_identity =
            idx_ns::gpu_index_gate_identity_from_key(runtime_key);
        REQUIRE(required_cache.insert_built(runtime_key, corrupt).has_value());
        const auto rejected = required_cache.find_required_preinserted(
            runtime, runtime_key);
        REQUIRE_FALSE(rejected.has_value());
        REQUIRE(rejected.error()
                == cache_type::required_preinsert_lookup_error::resident_mismatch);
    }
}

}  // namespace

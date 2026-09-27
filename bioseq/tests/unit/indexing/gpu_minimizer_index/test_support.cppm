/**
 * @file test_support.cppm
 * @brief Builds GPU-index fixtures and CPU reference lookups for unit tests.
 */
module;

#include <cstring>
#include <new>

#include <vulkan/vulkan_core.h>

export module bioseq.tests.unit.indexing.gpu_minimizer_index_support;

import std;
import bioseq.pipeline.runtime_contract;
import bioseq.common.seqan_adapter;
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

export namespace bioseq::tests::unit::indexing::gpu_minimizer_index_test_support {

using minimap_sketch_data = bioseq::indexing::minimap_sketch_data;
using bioseq::indexing::minimizer_hashmap_fragment_index;
namespace idx_ns = bioseq::indexing;

/// Finds a hash in the upload table using the shader's linear-probing rules.
[[nodiscard]] std::uint32_t host_find_slot(
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

/// Finds a hash in sorted GPU slots using binary search.
[[nodiscard]] std::uint32_t host_find_sorted_dense_slot(
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

[[nodiscard]] std::uint32_t host_find_slot(
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

/// Builds a CPU index with four hashes, including one with three positions.
[[nodiscard]] minimizer_hashmap_fragment_index build_demo_index() {
    std::vector<minimap_sketch_data> minimizers;

    minimizers.emplace_back(0x111ULL, 5u, 0u);
    minimizers.emplace_back(0x222ULL, 9u, 1u);
    minimizers.emplace_back(0x333ULL, 13u, 0u);
    minimizers.emplace_back(0x222ULL, 40u, 0u);   // Another occurrence of hash 0x222.
    minimizers.emplace_back(0x444ULL, 77u, 1u);
    minimizers.emplace_back(0x222ULL, 100u, 1u);  // Third occurrence of hash 0x222.
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

/// Allocates GPU buffers and matching ownership counters for adoption tests.
/// The optional query gate and empty-target flag  control the fixture metadata.
[[nodiscard]] bioseq::gpu::vulkan::gpu_built_index
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

/// Writes an unsigned little-endian value, rejecting writes outside the fixture buffer.
template <class p_unsigned_integer>
requires (std::is_unsigned_v<p_unsigned_integer>)
void store_test_le(
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

/// Owns grouped index control and position bytes with their directory and checksums.
struct grouped_soa_gpu_resident_fixture {
    std::array<std::vector<std::byte>, 2> control_bytes;
    std::array<std::vector<std::byte>, 2> position_bytes;
    std::array<idx_ns::grouped_soa_bucket_directory_entry, 2> entries;
    idx_ns::minimizer_generation_head head;
    idx_ns::minimizer_generation_manifest manifest;
    idx_ns::grouped_soa_directory_view directory;
};

/// Builds the expected DNA4, k=15, window=10 identity for GPU index-loading tests.
[[nodiscard]] constexpr idx_ns::expected_minimizer_generation_identity
grouped_soa_gpu_expected_identity() noexcept {
    constexpr auto physical_layout =
        bioseq::minimizers::select_minimizer_physical_layout<seq::dna4, std::uint64_t>(
            bioseq::simd::minimizer_algorithm::two_stack_nthash,
            idx_ns::k_minimizer_fragment_algorithm_position_extent_bp,
            15u,
            10u);
    constexpr auto algorithm_identity =
        idx_ns::make_minimizer_generation_algorithm_identity(physical_layout);
    static_assert(algorithm_identity.has_value());
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
        .algorithm_identity = *algorithm_identity,
        .max_occ_cap = 10u,
    };
}

/// Builds two grouped index partitions with matching row, directory and manifest checksums.
[[nodiscard]] grouped_soa_gpu_resident_fixture
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
    constexpr auto physical_layout =
        bioseq::minimizers::select_minimizer_physical_layout<seq::dna4, std::uint64_t>(
            bioseq::simd::minimizer_algorithm::two_stack_nthash,
            idx_ns::k_minimizer_fragment_algorithm_position_extent_bp,
            15u,
            10u);
    constexpr auto algorithm_identity =
        idx_ns::make_minimizer_generation_algorithm_identity(physical_layout);
    static_assert(algorithm_identity.has_value());

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
        .algorithm_identity = *algorithm_identity,
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

/// Builds a repeatable test device UUID from the supplied byte.
[[nodiscard]] bioseq::gpu::vulkan::gpu_device_uuid synthetic_device_uuid(
    std::uint8_t seed) noexcept
{
    bioseq::gpu::vulkan::gpu_device_uuid uuid{};
    for (std::size_t index = 0; index < uuid.size(); ++index) {
        uuid[index] = static_cast<std::uint8_t>(seed + index);
    }
    return uuid;
}

} // namespace bioseq::tests::unit::indexing::gpu_minimizer_index_test_support

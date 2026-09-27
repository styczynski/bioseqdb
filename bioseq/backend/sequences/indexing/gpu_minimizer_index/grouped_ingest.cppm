/**
 * @file grouped_ingest.cppm
 * @brief Copy validated grouped minimizer rows iito GPU buffers.
 */
module;

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <exception>
#include <expected>
#include <fmt/format.h>
#include <libassert/assert.hpp>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <vulkan/vulkan.h>

export module bioseq.indexing.gpu_minimizer_index:grouped_ingest;

import bioseq.common.checked_arithmetic;
import bioseq.common.logging;
import bioseq.common.radix_sort;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.storage;
import bioseq.indexing.type;
import bioseq.parallelism.memory_admission;
import bioseq.storage.size;

export namespace bioseq::indexing {

/// Test whether a device UUID contains a nonzero identity byte.
[[nodiscard]] inline bool
is_gpu_device_uuid_bound(const bioseq::gpu::vulkan::gpu_device_uuid& uuid) noexcept {
    return std::any_of(
        uuid.begin(), uuid.end(), [](std::uint8_t byte) noexcept { return byte != 0u; });
}

/** @brief Errors while planning, copying, or publishing grouped minimizer data on the GPU. */
enum class gpu_grouped_soa_ingest_error : std::uint8_t {
    invalid_identity,
    device_identity_mismatch,
    unsupported_endianness,
    metadata_mismatch,
    arithmetic_overflow,
    admission,
    non_mappable_device_memory,
    partition_mismatch,
    byte_count,
    checksum,
    completion_mismatch,
    unauthorized_extent,
    poisoned_stream,
    invalid_stream_limits,
    extent_exceeds_slot,
    wrong_stream_phase,
    slot_busy,
    stale_slot,
    authorization_pending,
    capture_mismatch,
};

static_assert(k_grouped_soa_gpu_abi_revision ==
              bioseq::gpu::vulkan::k_gpu_grouped_soa_abi_revision);

/// Identifies a grouped minimizer generation loaded onto one GPU.
/// The caller supplies cold_load_identity to  distinguish the load. Storage
/// checksums and the device UUID identify the data and its destination.
struct gpu_grouped_soa_resident_identity {
    minimizer_generation_layout layout = minimizer_generation_layout::invalid;
    std::uint64_t generation_id = 0u;
    std::uint64_t source_identity = 0u;
    std::uint64_t source_content_checksum = 0u;
    std::uint64_t storage_contract_checksum = 0u;
    std::uint64_t manifest_checksum = 0u;
    std::uint64_t directory_checksum = 0u;
    std::uint64_t payload_checksum = 0u;
    std::uint64_t cold_load_identity = 0u;
    std::uint64_t directory_bytes = 0u;
    std::uint64_t control_bytes = 0u;
    std::uint64_t position_bytes = 0u;
    std::uint64_t descriptor_bytes = 0u;
    std::uint64_t control_count = 0u;
    std::uint64_t position_count = 0u;
    std::uint32_t descriptor_count = 0u;
    bioseq::gpu::vulkan::gpu_device_uuid device_uuid{};
    std::uint32_t gpu_abi_revision = 0u;

    [[nodiscard]] bool operator==(const gpu_grouped_soa_resident_identity&) const = default;
};

/// Check the required nonzero IDs, grouped layout, device UUID, and GPU ABI version.
[[nodiscard]] inline bool is_gpu_grouped_soa_resident_identity_valid(
    const gpu_grouped_soa_resident_identity& identity) noexcept {
    return identity.layout == minimizer_generation_layout::grouped_soa_segment_local &&
           identity.generation_id != 0u && identity.source_identity != 0u &&
           identity.cold_load_identity != 0u && is_gpu_device_uuid_bound(identity.device_uuid) &&
           identity.gpu_abi_revision == k_grouped_soa_gpu_abi_revision;
}

/// Records rows checked, bytes copied, and memory used while loading a, generation
/// Separate counters detect host decoding or index rebuilding, which this loading
/// path must avoid because the stored arrays are copied directly to GPU memory.
struct gpu_grouped_soa_cold_load_stats {
    std::uint64_t cold_cache_misses = 0u;
    std::uint64_t full_generations_ingested = 0u;
    std::uint64_t control_extents_validated = 0u;
    std::uint64_t position_extents_validated = 0u;
    std::uint64_t control_bytes_copied = 0u;
    std::uint64_t position_bytes_copied = 0u;
    std::uint64_t descriptor_bytes_copied = 0u;
    std::uint64_t device_allocation_bytes = 0u;
    std::uint64_t admitted_device_bytes = 0u;
    std::uint64_t validated_receipt_rows = 0u;
    std::uint64_t raw_rehashed_rows = 0u;
    std::uint64_t payload_hash_byte_visits = 0u;
    std::uint64_t forbidden_host_control_decodes = 0u;
    std::uint64_t forbidden_host_position_decodes = 0u;
    std::uint64_t forbidden_host_position_globalizations = 0u;
    std::uint64_t forbidden_host_index_rebuilds = 0u;
    std::uint64_t forbidden_cpu_or_simd_fallbacks = 0u;

    /// Check whether any  counter reports host decoding, coordinate conversion, rebuilding, or fallback.
    [[nodiscard]] bool has_forbidden_work() const noexcept {
        return forbidden_host_control_decodes != 0u || forbidden_host_position_decodes != 0u ||
               forbidden_host_position_globalizations != 0u ||
               forbidden_host_index_rebuilds != 0u || forbidden_cpu_or_simd_fallbacks != 0u;
    }
};

inline constexpr std::uint32_t k_gpu_grouped_soa_min_stream_slots = 2u;
inline constexpr std::uint32_t k_gpu_grouped_soa_max_stream_slots = 4u;

/** @brief Slot count and per-slot buffer limits fixed before reading payload rows. */
struct gpu_grouped_soa_stream_limits {
    std::uint32_t slot_count = k_gpu_grouped_soa_min_stream_slots;
    std::uint32_t payload_capacity_bytes = 0u;
    std::uint32_t descriptor_capacity = 0u;

    [[nodiscard]] bool valid(std::uint64_t max_storage_buffer_range) const noexcept {
        const std::uint64_t descriptor_bytes = bioseq::common::saturating_mul(
            descriptor_capacity, sizeof(bioseq::gpu::vulkan::gpu_shape_v5_partition_wire));
        return slot_count >= k_gpu_grouped_soa_min_stream_slots &&
               slot_count <= k_gpu_grouped_soa_max_stream_slots && payload_capacity_bytes != 0u &&
               descriptor_capacity != 0u && payload_capacity_bytes <= max_storage_buffer_range &&
               descriptor_bytes != (std::numeric_limits<std::uint64_t>::max)() &&
               descriptor_bytes <= (std::numeric_limits<std::uint32_t>::max)() &&
               descriptor_bytes <= max_storage_buffer_range;
    }

    [[nodiscard]] bool operator==(const gpu_grouped_soa_stream_limits&) const = default;
};

/** @brief Where one complete stored extent belongs in a stream group. */
struct gpu_grouped_soa_stream_extent_plan {
    index_sub_key_request request{};
    bioseq::gpu::vulkan::gpu_shape_v5_partition_wire descriptor{};
    bioseq::gpu::vulkan::gpu_shape_v5_group_phase phase =
        bioseq::gpu::vulkan::gpu_shape_v5_group_phase::controls;
    std::uint32_t group_ordinal = 0u;
    std::uint32_t payload_offset = 0u;
};

/** @brief Extents, byte count, and partition range assigned to one stream group. */
struct gpu_grouped_soa_stream_group_plan {
    bioseq::gpu::vulkan::gpu_shape_v5_group_phase phase =
        bioseq::gpu::vulkan::gpu_shape_v5_group_phase::controls;
    std::uint32_t group_ordinal = 0u;
    std::uint32_t first_extent = 0u;
    std::uint32_t extent_count = 0u;
    std::uint32_t payload_bytes = 0u;
    std::uint32_t descriptor_count = 0u;
    std::uint32_t first_directory_ordinal = 0u;
    std::uint32_t end_directory_ordinal = 0u;
    std::int32_t position_segment = 0;
    std::uint64_t logical_item_count = 0u;

    [[nodiscard]] bool operator==(const gpu_grouped_soa_stream_group_plan&) const = default;
};

/** @brief Generation IDs and checksums used to keep a ring bound to its stream plan. */
struct gpu_grouped_soa_stream_plan_identity {
    std::uint64_t generation_id = 0u;
    std::uint64_t source_identity = 0u;
    std::uint64_t source_content_checksum = 0u;
    std::uint64_t storage_contract_checksum = 0u;
    std::uint64_t manifest_checksum = 0u;
    std::uint64_t directory_checksum = 0u;
    std::uint64_t payload_checksum = 0u;

    [[nodiscard]] bool operator==(const gpu_grouped_soa_stream_plan_identity&) const = default;
};

/**
 * @brief Split a Format-5 generation into groups that fit the stream buffers.
 *
 * The plan stores metadata only. Each group contains complete extents from one
 * pouition segment, and its payload and descriptors fit  the configured slot
 * capacities. Controls and positions have separate group lists so the caller
 * can finish control processing before sending positions.
 */
class gpu_grouped_soa_stream_plan {
  public:
    template <class p_directory_view>
    [[nodiscard]] static std::expected<gpu_grouped_soa_stream_plan, gpu_grouped_soa_ingest_error>
    make(const minimizer_generation_head& head,
         const minimizer_generation_manifest& manifest,
         const expected_minimizer_generation_identity& expected,
         const p_directory_view& directory,
         gpu_grouped_soa_stream_limits limits,
         std::uint64_t max_storage_buffer_range) {
        if constexpr (std::endian::native != std::endian::little) {
            return std::unexpected(gpu_grouped_soa_ingest_error::unsupported_endianness);
        }
        const auto profile = derive_grouped_soa_minimizer_profile(manifest);
        const bool supported_stream_profile =
            profile && manifest.position_width == minimizer_scalar_width::u32 &&
            manifest.count_width == minimizer_scalar_width::u16 &&
            (manifest.hash_width == minimizer_scalar_width::u16 ||
             manifest.hash_width == minimizer_scalar_width::u32 ||
             manifest.hash_width == minimizer_scalar_width::u64);
        if (!limits.valid(max_storage_buffer_range) || !profile || !supported_stream_profile ||
            manifest.layout != minimizer_generation_layout::grouped_soa_segment_local ||
            manifest.gpu_abi_revision != k_grouped_soa_gpu_abi_revision ||
            directory.profile() != *profile ||
            !validate_minimizer_generation_storage_contract(manifest, expected) ||
            !validate_minimizer_generation_metadata(head, manifest, directory)) {
            return std::unexpected(!limits.valid(max_storage_buffer_range)
                                       ? gpu_grouped_soa_ingest_error::invalid_stream_limits
                                       : gpu_grouped_soa_ingest_error::metadata_mismatch);
        }
        const auto entries = directory.entries();
        if (entries.size() > (std::numeric_limits<std::uint32_t>::max)()) {
            return std::unexpected(gpu_grouped_soa_ingest_error::arithmetic_overflow);
        }

        gpu_grouped_soa_stream_plan result{};
        result.limits_ = limits;
        result.identity_ = gpu_grouped_soa_stream_plan_identity{
            .generation_id = manifest.generation_id,
            .source_identity = manifest.source_identity,
            .source_content_checksum = manifest.source_content_checksum,
            .storage_contract_checksum = head.storage_contract_checksum,
            .manifest_checksum = head.manifest_checksum,
            .directory_checksum = manifest.directory_checksum,
            .payload_checksum = manifest.payload_checksum,
        };
        result.payload_checksum_expected_ = manifest.payload_checksum;
        result.control_extents_.reserve(entries.size());
        result.position_extents_.reserve(entries.size());
        result.control_groups_.reserve(entries.size());
        result.position_groups_.reserve(entries.size());

        const auto append_phase =
            [&entries, &manifest, &limits, &profile](
                bioseq::gpu::vulkan::gpu_shape_v5_group_phase phase,
                auto& extents,
                auto& groups,
                std::uint64_t& total_bytes,
                std::uint64_t& total_items,
                std::uint64_t& total_rows) -> std::expected<void, gpu_grouped_soa_ingest_error> {
            const bool positions =
                phase == bioseq::gpu::vulkan::gpu_shape_v5_group_phase::positions;
            for (std::size_t ordinal = 0u; ordinal < entries.size(); ++ordinal) {
                const grouped_soa_bucket_directory_entry& entry = entries[ordinal];
                if (validate_minimizer_bucket_dir_entry(entry, *profile).has_value() ||
                    entry.partition_id != ordinal ||
                    entry.shape_offset_width != minimizer_scalar_width::u16 ||
                    entry.control_element_count > (std::numeric_limits<std::uint32_t>::max)() ||
                    entry.position_element_count > (std::numeric_limits<std::uint32_t>::max)()) {
                    return std::unexpected(gpu_grouped_soa_ingest_error::partition_mismatch);
                }
                const auto segment = derive_minimizer_position_segment_bounds(
                    manifest, static_cast<std::uint32_t>(entry.position_segment));
                const auto control_layout =
                    plan_grouped_soa_control_plane(entry.control_element_count,
                                                   manifest.hash_width,
                                                   entry.shape_offset_width,
                                                   manifest.count_width);
                const std::uint64_t encoded_bytes =
                    positions ? entry.position_encoded_bytes : entry.control_encoded_bytes;
                const std::uint64_t logical_items =
                    positions ? entry.position_element_count : entry.control_element_count;
                if (!segment || !control_layout || encoded_bytes == 0u ||
                    encoded_bytes > limits.payload_capacity_bytes) {
                    return std::unexpected(!segment || !control_layout
                                               ? gpu_grouped_soa_ingest_error::partition_mismatch
                                               : gpu_grouped_soa_ingest_error::extent_exceeds_slot);
                }

                const bool needs_group =
                    groups.empty() || groups.back().position_segment != entry.position_segment ||
                    groups.back().descriptor_count == limits.descriptor_capacity ||
                    encoded_bytes > limits.payload_capacity_bytes - groups.back().payload_bytes;
                if (needs_group) {
                    if (groups.size() >= (std::numeric_limits<std::uint32_t>::max)() ||
                        extents.size() > (std::numeric_limits<std::uint32_t>::max)()) {
                        return std::unexpected(gpu_grouped_soa_ingest_error::arithmetic_overflow);
                    }
                    groups.push_back(gpu_grouped_soa_stream_group_plan{
                        .phase = phase,
                        .group_ordinal = static_cast<std::uint32_t>(groups.size()),
                        .first_extent = static_cast<std::uint32_t>(extents.size()),
                        .first_directory_ordinal = static_cast<std::uint32_t>(ordinal),
                        .end_directory_ordinal = static_cast<std::uint32_t>(ordinal),
                        .position_segment = entry.position_segment,
                    });
                }
                auto& group = groups.back();
                if (encoded_bytes > limits.payload_capacity_bytes - group.payload_bytes ||
                    group.descriptor_count >= limits.descriptor_capacity ||
                    ordinal >= (std::numeric_limits<std::uint32_t>::max)()) {
                    return std::unexpected(gpu_grouped_soa_ingest_error::arithmetic_overflow);
                }
                const std::uint64_t new_group_bytes = group.payload_bytes + encoded_bytes;
                const std::uint64_t new_group_items =
                    bioseq::common::saturating_add(group.logical_item_count, logical_items);
                if (new_group_bytes > (std::numeric_limits<std::uint32_t>::max)() ||
                    new_group_items == (std::numeric_limits<std::uint64_t>::max)()) {
                    return std::unexpected(gpu_grouped_soa_ingest_error::arithmetic_overflow);
                }
                const std::uint32_t payload_offset = group.payload_bytes;
                const auto position_sub_key =
                    derive_minimizer_position_extent_sub_key(entry.sub_key);
                if (!position_sub_key) {
                    return std::unexpected(gpu_grouped_soa_ingest_error::partition_mismatch);
                }
                const index_sub_key_request request{
                    .generation_id = manifest.generation_id,
                    .position_segment = entry.position_segment,
                    .sub_key = positions ? *position_sub_key : entry.sub_key,
                    .first_chunk_no =
                        positions ? entry.position_first_chunk : entry.control_first_chunk,
                    .expected_chunk_count =
                        positions ? entry.position_chunk_count : entry.control_chunk_count,
                    .expected_total_bytes = encoded_bytes,
                    .expected_checksum =
                        positions ? entry.position_checksum : entry.control_checksum,
                };
                const auto checked_offset =
                    [payload_offset](std::uint64_t local) -> std::optional<std::uint32_t> {
                    const std::uint64_t value = payload_offset + local;
                    if (value > (std::numeric_limits<std::uint32_t>::max)()) {
                        return std::nullopt;
                    }
                    return static_cast<std::uint32_t>(value);
                };
                const auto hash_offset =
                    checked_offset(positions ? 0u : control_layout->hash_offset);
                const auto prefix_offset = checked_offset(control_layout->prefix_offset);
                const auto count_offset = checked_offset(control_layout->count_offset);
                if (!hash_offset || (!positions && (!prefix_offset || !count_offset))) {
                    return std::unexpected(gpu_grouped_soa_ingest_error::arithmetic_overflow);
                }
                const bioseq::gpu::vulkan::gpu_shape_v5_partition_wire descriptor{
                    .partition_id = entry.partition_id,
                    .directory_ordinal = static_cast<std::uint32_t>(ordinal),
                    .position_segment = static_cast<std::uint32_t>(entry.position_segment),
                    .control_count = static_cast<std::uint32_t>(entry.control_element_count),
                    .position_count = static_cast<std::uint32_t>(entry.position_element_count),
                    .hash_array_offset = *hash_offset,
                    .prefix_offset_array_offset = positions ? 0u : *prefix_offset,
                    .count_array_offset = positions ? 0u : *count_offset,
                    .hash_width = static_cast<std::uint32_t>(manifest.hash_width),
                    .offset_width = static_cast<std::uint32_t>(entry.shape_offset_width),
                    .count_width = static_cast<std::uint32_t>(manifest.count_width),
                    .position_width = static_cast<std::uint32_t>(manifest.position_width),
                    .segment_base = segment->begin_bp,
                    .segment_end = segment->end_bp,
                    .max_hash = manifest.max_hash,
                    .max_occ_cap = manifest.max_occ_cap,
                };
                if (!bioseq::gpu::vulkan::is_gpu_shape_v5_partition_wire_valid(
                        descriptor, phase, static_cast<std::uint32_t>(new_group_bytes))) {
                    return std::unexpected(gpu_grouped_soa_ingest_error::partition_mismatch);
                }
                extents.push_back(gpu_grouped_soa_stream_extent_plan{
                    .request = request,
                    .descriptor = descriptor,
                    .phase = phase,
                    .group_ordinal = group.group_ordinal,
                    .payload_offset = payload_offset,
                });
                ++group.extent_count;
                ++group.descriptor_count;
                group.payload_bytes = static_cast<std::uint32_t>(new_group_bytes);
                group.end_directory_ordinal = static_cast<std::uint32_t>(ordinal + 1u);
                group.logical_item_count = new_group_items;
                total_bytes = bioseq::common::saturating_add(total_bytes, encoded_bytes);
                total_items = bioseq::common::saturating_add(total_items, logical_items);
                total_rows =
                    bioseq::common::saturating_add(total_rows, request.expected_chunk_count);
                if (total_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                    total_items == (std::numeric_limits<std::uint64_t>::max)() ||
                    total_rows == (std::numeric_limits<std::uint64_t>::max)()) {
                    return std::unexpected(gpu_grouped_soa_ingest_error::arithmetic_overflow);
                }
            }
            return {};
        };

        if (auto planned = append_phase(bioseq::gpu::vulkan::gpu_shape_v5_group_phase::controls,
                                        result.control_extents_,
                                        result.control_groups_,
                                        result.control_bytes_,
                                        result.control_count_,
                                        result.expected_payload_rows_);
            !planned) {
            return std::unexpected(planned.error());
        }
        if (auto planned = append_phase(bioseq::gpu::vulkan::gpu_shape_v5_group_phase::positions,
                                        result.position_extents_,
                                        result.position_groups_,
                                        result.position_bytes_,
                                        result.position_count_,
                                        result.expected_payload_rows_);
            !planned) {
            return std::unexpected(planned.error());
        }
        const std::uint64_t payload_bytes =
            bioseq::common::saturating_add(result.control_bytes_, result.position_bytes_);
        if (payload_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            payload_bytes != manifest.payload_bytes ||
            result.control_count_ != manifest.control_count ||
            result.position_count_ != manifest.position_count) {
            return std::unexpected(gpu_grouped_soa_ingest_error::metadata_mismatch);
        }
        return result;
    }

    [[nodiscard]] const gpu_grouped_soa_stream_limits& limits() const noexcept {
        return limits_;
    }
    [[nodiscard]] const gpu_grouped_soa_stream_plan_identity& identity() const noexcept {
        return identity_;
    }
    [[nodiscard]] std::span<const gpu_grouped_soa_stream_group_plan>
    control_groups() const noexcept {
        return control_groups_;
    }
    [[nodiscard]] std::span<const gpu_grouped_soa_stream_group_plan>
    position_groups() const noexcept {
        return position_groups_;
    }
    [[nodiscard]] std::span<const gpu_grouped_soa_stream_extent_plan>
    control_extents() const noexcept {
        return control_extents_;
    }
    [[nodiscard]] std::span<const gpu_grouped_soa_stream_extent_plan>
    position_extents() const noexcept {
        return position_extents_;
    }
    [[nodiscard]] std::span<const gpu_grouped_soa_stream_extent_plan>
    extents(const gpu_grouped_soa_stream_group_plan& group) const noexcept {
        if (!bioseq::gpu::vulkan::is_gpu_shape_v5_group_phase_valid(group.phase)) {
            return {};
        }
        const auto source = group.phase == bioseq::gpu::vulkan::gpu_shape_v5_group_phase::controls
                                ? control_extents()
                                : position_extents();
        if (group.first_extent > source.size() ||
            group.extent_count > source.size() - group.first_extent) {
            return {};
        }
        return source.subspan(group.first_extent, group.extent_count);
    }
    [[nodiscard]] std::uint64_t control_bytes() const noexcept {
        return control_bytes_;
    }
    [[nodiscard]] std::uint64_t position_bytes() const noexcept {
        return position_bytes_;
    }
    [[nodiscard]] std::uint64_t control_count() const noexcept {
        return control_count_;
    }
    [[nodiscard]] std::uint64_t position_count() const noexcept {
        return position_count_;
    }
    [[nodiscard]] std::uint64_t expected_payload_rows() const noexcept {
        return expected_payload_rows_;
    }
    [[nodiscard]] std::uint64_t payload_checksum_expected() const noexcept {
        return payload_checksum_expected_;
    }

  private:
    gpu_grouped_soa_stream_limits limits_{};
    gpu_grouped_soa_stream_plan_identity identity_{};
    std::vector<gpu_grouped_soa_stream_extent_plan> control_extents_{};
    std::vector<gpu_grouped_soa_stream_extent_plan> position_extents_{};
    std::vector<gpu_grouped_soa_stream_group_plan> control_groups_{};
    std::vector<gpu_grouped_soa_stream_group_plan> position_groups_{};
    std::uint64_t control_bytes_ = 0u;
    std::uint64_t position_bytes_ = 0u;
    std::uint64_t control_count_ = 0u;
    std::uint64_t position_count_ = 0u;
    std::uint64_t expected_payload_rows_ = 0u;
    std::uint64_t payload_checksum_expected_ = 0u;
};

/** @brief States a slot passes through while receiving and submitting one group. */
enum class gpu_grouped_soa_stream_slot_state : std::uint8_t {
    free,
    host_filling,
    ready_to_submit,
    submitted,
    poisoned,
};

/** @brief Control processing, authorization, and position processing for the whole stream. */
enum class gpu_grouped_soa_stream_state : std::uint8_t {
    controls,
    authorization_pending,
    positions,
    draining,
    complete,
    poisoned,
};

/** @brief Identifies one use of a ring slot so an older token cannot refer to reused buffers. */
struct gpu_grouped_soa_stream_slot_token {
    std::uint64_t stream_identity = 0u;
    std::uint32_t slot_ordinal = 0u;
    std::uint32_t group_ordinal = 0u;
    bioseq::gpu::vulkan::gpu_shape_v5_group_phase phase =
        bioseq::gpu::vulkan::gpu_shape_v5_group_phase::controls;
    std::uint64_t allocation_generation = 0u;
    std::uint64_t receipt_identity = 0u;

    [[nodiscard]] bool operator==(const gpu_grouped_soa_stream_slot_token&) const = default;
};

/** @brief Runtime submission and signal IDs used to match a group with its completion. */
struct gpu_grouped_soa_stream_completion {
    std::uint64_t runtime_generation = 0u;
    std::uint64_t submission_identity = 0u;
    std::uint64_t signal_value = 0u;

    [[nodiscard]] bool valid() const noexcept {
        return runtime_generation != 0u && submission_identity != 0u && signal_value != 0u;
    }
    [[nodiscard]] bool operator==(const gpu_grouped_soa_stream_completion&) const = default;
};

/** @brief Counts copies, submissions, and slot reuse within this ring. */
struct gpu_grouped_soa_stream_progress {
    std::uint64_t control_groups_submitted = 0u;
    std::uint64_t position_groups_submitted = 0u;
    std::uint64_t control_groups_retired = 0u;
    std::uint64_t position_groups_retired = 0u;
    std::uint64_t control_bytes_copied = 0u;
    std::uint64_t position_bytes_copied = 0u;
    std::uint64_t validated_receipt_rows = 0u;
    std::uint64_t slot_reuses = 0u;
    std::uint64_t allocated_device_bytes = 0u;
    std::uint64_t historically_admitted_bytes = 0u;
};

/** @brief Owns the payload and descriptor buffers for one reusable stream slot */
class gpu_grouped_soa_stream_slot {
  public:
    gpu_grouped_soa_stream_slot() = default;
    gpu_grouped_soa_stream_slot(const gpu_grouped_soa_stream_slot&) = delete;
    gpu_grouped_soa_stream_slot& operator=(const gpu_grouped_soa_stream_slot&) = delete;
    gpu_grouped_soa_stream_slot(gpu_grouped_soa_stream_slot&&) noexcept = default;
    gpu_grouped_soa_stream_slot& operator=(gpu_grouped_soa_stream_slot&&) noexcept = default;

    [[nodiscard]] const bioseq::gpu::vulkan::buffer& payload() const noexcept {
        return payload_;
    }
    [[nodiscard]] const bioseq::gpu::vulkan::buffer& descriptors() const noexcept {
        return descriptors_;
    }
    [[nodiscard]] gpu_grouped_soa_stream_slot_state state() const noexcept {
        return state_;
    }
    [[nodiscard]] std::uint64_t allocation_generation() const noexcept {
        return allocation_generation_;
    }
    [[nodiscard]] const auto& materializations() const noexcept {
        return materializations_;
    }

  private:
    std::array<bioseq::parallelism::memory_materialization_result, 2> materializations_{};
    bioseq::gpu::vulkan::buffer payload_{};
    bioseq::gpu::vulkan::buffer descriptors_{};
    gpu_grouped_soa_stream_slot_state state_ = gpu_grouped_soa_stream_slot_state::free;
    bioseq::gpu::vulkan::gpu_shape_v5_group_phase phase_ =
        bioseq::gpu::vulkan::gpu_shape_v5_group_phase::controls;
    gpu_grouped_soa_stream_completion completion_{};
    std::uint64_t allocation_generation_ = 0u;
    std::uint64_t receipt_identity_ = 0u;
    std::uint64_t bytes_received_ = 0u;
    std::uint64_t logical_item_count_ = 0u;
    std::uint32_t group_ordinal_ = 0u;
    std::uint32_t descriptor_count_ = 0u;
    std::uint32_t first_directory_ordinal_ = 0u;
    std::uint32_t end_directory_ordinal_ = 0u;
    std::uint32_t extent_cursor_ = 0u;
    std::uint32_t chunk_cursor_ = 0u;

    friend class gpu_grouped_soa_stream_ring;
};

/**
 * @brief Reuse a fixed set of GPU buffers while streaming a grouped generation.
 *
 * The  caller submits the GPU work. The ring accepts controls first and requires
 * authorization before accepting positions. A slot can be reused only after its
 * completion capture matches the submitted group. The ring checks ordering and
 * lifetimes, but does not establish that an entire query ran on the GPU.
 */
class gpu_grouped_soa_stream_ring {
  public:
    gpu_grouped_soa_stream_ring(const gpu_grouped_soa_stream_ring&) = delete;
    gpu_grouped_soa_stream_ring& operator=(const gpu_grouped_soa_stream_ring&) = delete;
    gpu_grouped_soa_stream_ring(gpu_grouped_soa_stream_ring&&) noexcept = default;
    gpu_grouped_soa_stream_ring& operator=(gpu_grouped_soa_stream_ring&&) = delete;

    [[nodiscard]] static std::expected<gpu_grouped_soa_stream_ring, gpu_grouped_soa_ingest_error>
    make(bioseq::gpu::vulkan::runtime& runtime,
         const gpu_grouped_soa_stream_plan& plan,
         std::uint64_t stream_identity) {
        const auto& limits = plan.limits();
        if (stream_identity == 0u || !runtime.is_usable() ||
            !is_gpu_device_uuid_bound(runtime.features().device_uuid) ||
            !limits.valid(runtime.features().max_storage_buffer_range)) {
            return std::unexpected(gpu_grouped_soa_ingest_error::invalid_stream_limits);
        }
        const std::uint64_t group_count = bioseq::common::saturating_add(
            plan.control_groups().size(), plan.position_groups().size());
        const std::uint64_t ledger_bytes = bioseq::common::saturating_mul(
            group_count, sizeof(bioseq::gpu::vulkan::gpu_persisted_group_capture_wire));
        const std::uint64_t slot_metadata_bytes =
            bioseq::common::saturating_mul(limits.slot_count, sizeof(gpu_grouped_soa_stream_slot));
        const std::uint64_t host_state_bytes =
            bioseq::common::saturating_add(ledger_bytes, slot_metadata_bytes);
        const std::uint64_t host_state_peak_bytes = bioseq::common::saturating_mul(
            host_state_bytes, bioseq::parallelism::k_vector_preallocation_headroom_factor);
        const std::uint64_t descriptor_bytes = bioseq::common::saturating_mul(
            limits.descriptor_capacity, sizeof(bioseq::gpu::vulkan::gpu_shape_v5_partition_wire));
        if (group_count == (std::numeric_limits<std::uint64_t>::max)() ||
            group_count > (std::numeric_limits<std::size_t>::max)() ||
            ledger_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            slot_metadata_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            host_state_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            host_state_peak_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            descriptor_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(gpu_grouped_soa_ingest_error::arithmetic_overflow);
        }

        // Declare the permit first so captures and Vulkan buffers are destroyed
        // before ther reserved memory budget is released.
        bioseq::parallelism::memory_permit retained_permit{};
        std::vector<gpu_grouped_soa_stream_slot> slots;
        std::vector<bioseq::gpu::vulkan::gpu_persisted_group_capture_wire> captures;
        auto& admission = bioseq::parallelism::memory_admission_controller::instance();
        if (host_state_peak_bytes != 0u) {
            auto host_state_permit = admission.try_reserve(host_state_peak_bytes);
            if (!host_state_permit) {
                return std::unexpected(gpu_grouped_soa_ingest_error::admission);
            }
            retained_permit = std::move(*host_state_permit);
        }
        slots.reserve(limits.slot_count);
        captures.reserve(static_cast<std::size_t>(group_count));
        const std::uint64_t retained_slot_bytes =
            bioseq::common::saturating_mul(slots.capacity(), sizeof(gpu_grouped_soa_stream_slot));
        const std::uint64_t retained_capture_bytes = bioseq::common::saturating_mul(
            captures.capacity(), sizeof(bioseq::gpu::vulkan::gpu_persisted_group_capture_wire));
        const std::uint64_t retained_host_state_bytes =
            bioseq::common::saturating_add(retained_slot_bytes, retained_capture_bytes);
        if (retained_slot_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            retained_capture_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            retained_host_state_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            retained_host_state_bytes > retained_permit.bytes()) {
            return std::unexpected(gpu_grouped_soa_ingest_error::admission);
        }
        retained_permit.shrink_to(retained_host_state_bytes);

        constexpr VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        const std::uint64_t payload_request =
            std::max<std::uint64_t>(limits.payload_capacity_bytes, 16u);
        const std::uint64_t descriptor_request = std::max<std::uint64_t>(descriptor_bytes, 16u);
        const std::uint64_t payload_requirement =
            runtime.buffer_allocation_requirement(payload_request, usage);
        const std::uint64_t descriptor_requirement =
            runtime.buffer_allocation_requirement(descriptor_request, usage);
        if (payload_requirement == 0u || descriptor_requirement == 0u) {
            return std::unexpected(gpu_grouped_soa_ingest_error::admission);
        }

        std::uint64_t allocated_device_bytes = 0u;
        std::uint64_t historically_admitted_bytes = host_state_peak_bytes;
        const auto materialize_buffer =
            [&runtime, &admission, &retained_permit, usage](
                std::uint64_t logical_bytes,
                std::uint64_t allocation_requirement,
                bioseq::gpu::vulkan::buffer& destination,
                bioseq::parallelism::memory_materialization_result& materialization)
            -> std::expected<void, gpu_grouped_soa_ingest_error> {
            auto permit = admission.try_reserve(allocation_requirement);
            if (!permit) {
                return std::unexpected(gpu_grouped_soa_ingest_error::admission);
            }
            bool allocation_mismatch = false;
            materialization = admission.materialize(
                *permit,
                [&runtime,
                 &destination,
                 logical_bytes,
                 allocation_requirement,
                 usage,
                 &allocation_mismatch]() -> bioseq::parallelism::memory_materialization_snapshot {
                    destination = runtime.create_device_buffer(logical_bytes, usage);
                    if (destination.allocation_bytes != allocation_requirement) {
                        allocation_mismatch = true;
                        destination.reset();
                        return {};
                    }
                    if (destination.mapped == nullptr || !destination.is_host_visible_coherent()) {
                        return {
                            .actual_allocation_bytes = destination.allocation_bytes,
                            .is_host_visible = destination.mapped != nullptr,
                            .is_host_coherent = destination.is_host_visible_coherent(),
                            .is_process_footprint_reflected = false,
                        };
                    }
                    std::memset(destination.mapped,
                                0,
                                static_cast<std::size_t>(destination.allocation_bytes));
                    return {
                        .actual_allocation_bytes = destination.allocation_bytes,
                        .fully_touched_bytes = destination.allocation_bytes,
                        .is_host_visible = true,
                        .is_host_coherent = true,
                        .is_process_footprint_reflected = destination.process_footprint_reflected,
                    };
                });
            if (allocation_mismatch) {
                return std::unexpected(gpu_grouped_soa_ingest_error::admission);
            }
            if (destination.mapped == nullptr || !destination.is_host_visible_coherent()) {
                destination.reset();
                return std::unexpected(gpu_grouped_soa_ingest_error::non_mappable_device_memory);
            }
            if (*permit) {
                if (!retained_permit) {
                    retained_permit = std::move(*permit);
                } else if (!retained_permit.absorb(std::move(*permit))) {
                    destination.reset();
                    return std::unexpected(gpu_grouped_soa_ingest_error::admission);
                }
            }
            return {};
        };

        for (std::uint32_t ordinal = 0u; ordinal < limits.slot_count; ++ordinal) {
            slots.emplace_back();
            auto& slot = slots.back();
            if (auto made = materialize_buffer(
                    payload_request, payload_requirement, slot.payload_, slot.materializations_[0]);
                !made) {
                return std::unexpected(made.error());
            }
            if (auto made = materialize_buffer(descriptor_request,
                                               descriptor_requirement,
                                               slot.descriptors_,
                                               slot.materializations_[1]);
                !made) {
                return std::unexpected(made.error());
            }
            allocated_device_bytes = bioseq::common::saturating_add(
                allocated_device_bytes,
                bioseq::common::saturating_add(slot.payload_.allocation_bytes,
                                               slot.descriptors_.allocation_bytes));
            historically_admitted_bytes = bioseq::common::saturating_add(
                historically_admitted_bytes,
                bioseq::common::saturating_add(slot.materializations_[0].admitted_bytes,
                                               slot.materializations_[1].admitted_bytes));
            if (allocated_device_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                historically_admitted_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
                return std::unexpected(gpu_grouped_soa_ingest_error::arithmetic_overflow);
            }
        }
        return gpu_grouped_soa_stream_ring{std::move(retained_permit),
                                           std::move(slots),
                                           std::move(captures),
                                           stream_identity,
                                           runtime.features().device_uuid,
                                           plan,
                                           allocated_device_bytes,
                                           historically_admitted_bytes};
    }

    [[nodiscard]] std::expected<gpu_grouped_soa_stream_slot_token, gpu_grouped_soa_ingest_error>
    acquire_group(const gpu_grouped_soa_stream_plan& plan,
                  const gpu_grouped_soa_stream_group_plan& group,
                  std::uint64_t receipt_identity) noexcept {
        if (state_ == gpu_grouped_soa_stream_state::poisoned) {
            return std::unexpected(gpu_grouped_soa_ingest_error::poisoned_stream);
        }
        if (plan.identity() != plan_identity_ ||
            plan.limits().slot_count != static_cast<std::uint32_t>(slots_.size()) ||
            plan.limits().payload_capacity_bytes != payload_capacity_bytes_ ||
            plan.limits().descriptor_capacity != descriptor_capacity_) {
            return fail<gpu_grouped_soa_stream_slot_token>(
                gpu_grouped_soa_ingest_error::invalid_identity);
        }
        const bool controls =
            group.phase == bioseq::gpu::vulkan::gpu_shape_v5_group_phase::controls;
        const auto planned_groups = controls ? plan.control_groups() : plan.position_groups();
        if (!bioseq::gpu::vulkan::is_gpu_shape_v5_group_phase_valid(group.phase) ||
            group.group_ordinal >= planned_groups.size() ||
            planned_groups[group.group_ordinal] != group) {
            return fail<gpu_grouped_soa_stream_slot_token>(
                gpu_grouped_soa_ingest_error::unauthorized_extent);
        }
        const bool expected_phase = controls ? state_ == gpu_grouped_soa_stream_state::controls
                                             : state_ == gpu_grouped_soa_stream_state::positions;
        const std::uint32_t expected_group =
            controls ? next_control_group_to_submit_ : next_position_group_to_submit_;
        if (!expected_phase || group.group_ordinal != expected_group || receipt_identity == 0u ||
            group.payload_bytes > payload_capacity_bytes_ ||
            group.descriptor_count > descriptor_capacity_) {
            return fail<gpu_grouped_soa_stream_slot_token>(
                expected_phase ? gpu_grouped_soa_ingest_error::unauthorized_extent
                               : gpu_grouped_soa_ingest_error::wrong_stream_phase);
        }
        const auto planned_extents = plan.extents(group);
        if (planned_extents.size() != group.extent_count || planned_extents.empty()) {
            return fail<gpu_grouped_soa_stream_slot_token>(
                gpu_grouped_soa_ingest_error::completion_mismatch);
        }
        const std::uint32_t slot_ordinal =
            group.group_ordinal % static_cast<std::uint32_t>(slots_.size());
        auto& slot = slots_[slot_ordinal];
        if (slot.state_ != gpu_grouped_soa_stream_slot_state::free) {
            return std::unexpected(gpu_grouped_soa_ingest_error::slot_busy);
        }
        if (slot.allocation_generation_ == (std::numeric_limits<std::uint64_t>::max)()) {
            slot.state_ = gpu_grouped_soa_stream_slot_state::poisoned;
            return fail<gpu_grouped_soa_stream_slot_token>(
                gpu_grouped_soa_ingest_error::arithmetic_overflow);
        }
        ++slot.allocation_generation_;
        if (slot.allocation_generation_ > 1u) {
            ++progress_.slot_reuses;
        }
        const std::uint64_t descriptor_bytes = bioseq::common::saturating_mul(
            group.descriptor_count, sizeof(bioseq::gpu::vulkan::gpu_shape_v5_partition_wire));
        if (descriptor_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            descriptor_bytes > slot.descriptors_.bytes ||
            group.payload_bytes > slot.payload_.bytes) {
            return fail<gpu_grouped_soa_stream_slot_token>(
                gpu_grouped_soa_ingest_error::byte_count);
        }
        std::memset(slot.payload_.mapped, 0, group.payload_bytes);
        std::memset(slot.descriptors_.mapped, 0, static_cast<std::size_t>(descriptor_bytes));
        auto* descriptor_output = static_cast<std::byte*>(slot.descriptors_.mapped);
        for (std::size_t index = 0u; index < planned_extents.size(); ++index) {
            const auto& extent = planned_extents[index];
            if (extent.phase != group.phase || extent.group_ordinal != group.group_ordinal ||
                !bioseq::gpu::vulkan::is_gpu_shape_v5_partition_wire_valid(
                    extent.descriptor, group.phase, group.payload_bytes)) {
                return fail<gpu_grouped_soa_stream_slot_token>(
                    gpu_grouped_soa_ingest_error::partition_mismatch);
            }
            std::memcpy(descriptor_output +
                            index * sizeof(bioseq::gpu::vulkan::gpu_shape_v5_partition_wire),
                        &extent.descriptor,
                        sizeof(extent.descriptor));
        }
        slot.phase_ = group.phase;
        slot.group_ordinal_ = group.group_ordinal;
        slot.descriptor_count_ = group.descriptor_count;
        slot.receipt_identity_ = receipt_identity;
        slot.bytes_received_ = 0u;
        slot.logical_item_count_ = group.logical_item_count;
        slot.first_directory_ordinal_ = group.first_directory_ordinal;
        slot.end_directory_ordinal_ = group.end_directory_ordinal;
        slot.extent_cursor_ = 0u;
        slot.chunk_cursor_ = 0u;
        slot.completion_ = {};
        slot.state_ = gpu_grouped_soa_stream_slot_state::host_filling;
        return gpu_grouped_soa_stream_slot_token{
            .stream_identity = stream_identity_,
            .slot_ordinal = slot_ordinal,
            .group_ordinal = group.group_ordinal,
            .phase = group.phase,
            .allocation_generation = slot.allocation_generation_,
            .receipt_identity = receipt_identity,
        };
    }

    [[nodiscard]] std::expected<void, gpu_grouped_soa_ingest_error>
    accept_validated_row(const gpu_grouped_soa_stream_plan& plan,
                         const gpu_grouped_soa_stream_slot_token& token,
                         const validated_minimizer_generation_row_receipt& receipt) noexcept {
        std::expected<void, gpu_grouped_soa_ingest_error> accepted{};
        const bool visited = visit_validated_minimizer_generation_row(
            receipt,
            [this, &plan, &token, &accepted](const index_chunk_address& address,
                                             std::span<const std::byte> bytes,
                                             std::uint64_t row_checksum) noexcept {
                accepted = accept_chunk_(plan, token, address, bytes, row_checksum);
            });
        if (!visited) {
            return fail<void>(gpu_grouped_soa_ingest_error::unauthorized_extent);
        }
        return accepted;
    }

    [[nodiscard]] std::expected<bioseq::gpu::vulkan::gpu_shape_v5_raw_group_inputs,
                                gpu_grouped_soa_ingest_error>
    ready_inputs(const gpu_grouped_soa_stream_slot_token& token) const noexcept {
        const auto* slot = find_slot_(token);
        if (slot == nullptr || slot->state_ != gpu_grouped_soa_stream_slot_state::ready_to_submit) {
            return std::unexpected(gpu_grouped_soa_ingest_error::stale_slot);
        }
        const bioseq::gpu::vulkan::gpu_shape_v5_raw_group_inputs inputs{
            .descriptors = &slot->descriptors_,
            .payload = &slot->payload_,
            .descriptor_count = slot->descriptor_count_,
            .payload_bytes = static_cast<std::uint32_t>(slot->bytes_received_),
            .group_ordinal = slot->group_ordinal_,
            .phase = slot->phase_,
            .receipt_identity = slot->receipt_identity_,
        };
        if (!bioseq::gpu::vulkan::is_gpu_shape_v5_raw_group_inputs_valid(inputs)) {
            return std::unexpected(gpu_grouped_soa_ingest_error::completion_mismatch);
        }
        return inputs;
    }

    [[nodiscard]] std::expected<void, gpu_grouped_soa_ingest_error>
    mark_submitted(const gpu_grouped_soa_stream_slot_token& token,
                   gpu_grouped_soa_stream_completion completion) noexcept {
        auto* slot = find_slot_(token);
        if (slot == nullptr || slot->state_ != gpu_grouped_soa_stream_slot_state::ready_to_submit ||
            !completion.valid()) {
            return fail<void>(gpu_grouped_soa_ingest_error::stale_slot);
        }
        if ((runtime_generation_ != 0u && completion.runtime_generation != runtime_generation_) ||
            completion.signal_value <= last_submission_signal_ ||
            completion.submission_identity <= last_submission_identity_) {
            return fail<void>(gpu_grouped_soa_ingest_error::stale_slot);
        }
        const bool controls =
            token.phase == bioseq::gpu::vulkan::gpu_shape_v5_group_phase::controls;
        std::uint32_t& next =
            controls ? next_control_group_to_submit_ : next_position_group_to_submit_;
        const std::uint32_t total = controls ? control_group_count_ : position_group_count_;
        if (token.group_ordinal != next || next >= total) {
            return fail<void>(gpu_grouped_soa_ingest_error::completion_mismatch);
        }
        slot->completion_ = completion;
        slot->state_ = gpu_grouped_soa_stream_slot_state::submitted;
        runtime_generation_ = completion.runtime_generation;
        last_submission_signal_ = completion.signal_value;
        last_submission_identity_ = completion.submission_identity;
        ++next;
        if (controls) {
            ++progress_.control_groups_submitted;
            if (next == total) {
                state_ = gpu_grouped_soa_stream_state::authorization_pending;
            }
        } else {
            ++progress_.position_groups_submitted;
            if (next == total) {
                state_ = gpu_grouped_soa_stream_state::draining;
            }
        }
        return {};
    }

    [[nodiscard]] std::expected<void, gpu_grouped_soa_ingest_error> retire_completed(
        const gpu_grouped_soa_stream_slot_token& token,
        const bioseq::gpu::vulkan::gpu_persisted_group_capture_wire& capture) noexcept {
        auto* slot = find_slot_(token);
        if (slot == nullptr || slot->state_ != gpu_grouped_soa_stream_slot_state::submitted) {
            return fail<void>(gpu_grouped_soa_ingest_error::stale_slot);
        }
        const bool controls =
            token.phase == bioseq::gpu::vulkan::gpu_shape_v5_group_phase::controls;
        std::uint32_t& next =
            controls ? next_control_group_to_retire_ : next_position_group_to_retire_;
        const auto digest_nonzero = [](const auto& digest) noexcept {
            return std::any_of(
                digest.begin(), digest.end(), [](std::byte byte) { return byte != std::byte{0}; });
        };
        if (token.group_ordinal != next ||
            (!controls && next_control_group_to_retire_ != control_group_count_) ||
            capture.phase != static_cast<std::uint32_t>(token.phase) ||
            capture.group_ordinal != token.group_ordinal ||
            capture.slot_ordinal != token.slot_ordinal ||
            capture.descriptor_count != slot->descriptor_count_ ||
            capture.allocation_generation != token.allocation_generation ||
            capture.receipt_identity != token.receipt_identity ||
            capture.runtime_generation != slot->completion_.runtime_generation ||
            capture.submission_identity != slot->completion_.submission_identity ||
            capture.signal_value != slot->completion_.signal_value ||
            capture.logical_item_count != slot->logical_item_count_ ||
            capture.payload_bytes != slot->bytes_received_ ||
            capture.first_directory_ordinal != slot->first_directory_ordinal_ ||
            capture.end_directory_ordinal != slot->end_directory_ordinal_ || capture.status != 0u ||
            capture.reserved_zero0 != 0u || capture.reserved_zero1 != 0u ||
            !digest_nonzero(capture.payload_sha256) || !digest_nonzero(capture.capture_sha256) ||
            captures_.size() >= captures_.capacity()) {
            return fail<void>(gpu_grouped_soa_ingest_error::capture_mismatch);
        }
        captures_.push_back(capture);
        ++next;
        if (controls) {
            ++progress_.control_groups_retired;
        } else {
            ++progress_.position_groups_retired;
        }
        slot->state_ = gpu_grouped_soa_stream_slot_state::free;
        slot->completion_ = {};
        slot->receipt_identity_ = 0u;
        slot->bytes_received_ = 0u;
        slot->logical_item_count_ = 0u;
        slot->descriptor_count_ = 0u;
        slot->first_directory_ordinal_ = 0u;
        slot->end_directory_ordinal_ = 0u;
        slot->extent_cursor_ = 0u;
        slot->chunk_cursor_ = 0u;
        return {};
    }

    [[nodiscard]] std::expected<void, gpu_grouped_soa_ingest_error>
    complete_authorization(gpu_grouped_soa_stream_completion completion,
                           std::uint64_t receipt_identity) noexcept {
        if (state_ != gpu_grouped_soa_stream_state::authorization_pending ||
            next_control_group_to_submit_ != control_group_count_ ||
            next_control_group_to_retire_ != control_group_count_ || !completion.valid() ||
            receipt_identity == 0u ||
            (runtime_generation_ != 0u && completion.runtime_generation != runtime_generation_) ||
            completion.signal_value <= last_submission_signal_ ||
            completion.submission_identity <= last_submission_identity_) {
            return fail<void>(gpu_grouped_soa_ingest_error::authorization_pending);
        }
        authorization_completion_ = completion;
        authorization_receipt_identity_ = receipt_identity;
        runtime_generation_ = completion.runtime_generation;
        last_submission_signal_ = completion.signal_value;
        last_submission_identity_ = completion.submission_identity;
        state_ = position_group_count_ == 0u ? gpu_grouped_soa_stream_state::draining
                                             : gpu_grouped_soa_stream_state::positions;
        return {};
    }

    [[nodiscard]] std::expected<void, gpu_grouped_soa_ingest_error> seal() noexcept {
        if (state_ != gpu_grouped_soa_stream_state::draining ||
            next_position_group_to_submit_ != position_group_count_ ||
            next_position_group_to_retire_ != position_group_count_ ||
            captures_.size() !=
                static_cast<std::size_t>(control_group_count_) + position_group_count_ ||
            !authorization_completion_.valid() || authorization_receipt_identity_ == 0u ||
            progress_.validated_receipt_rows != expected_payload_rows_ ||
            payload_row_crc_.checksum() != payload_checksum_expected_ ||
            std::any_of(slots_.begin(), slots_.end(), [](const auto& slot) {
                return slot.state_ != gpu_grouped_soa_stream_slot_state::free;
            })) {
            return fail<void>(gpu_grouped_soa_ingest_error::completion_mismatch);
        }
        state_ = gpu_grouped_soa_stream_state::complete;
        return {};
    }

    void poison() noexcept {
        state_ = gpu_grouped_soa_stream_state::poisoned;
        for (auto& slot : slots_) {
            if (slot.state_ == gpu_grouped_soa_stream_slot_state::host_filling ||
                slot.state_ == gpu_grouped_soa_stream_slot_state::ready_to_submit) {
                slot.state_ = gpu_grouped_soa_stream_slot_state::poisoned;
            }
        }
    }

    [[nodiscard]] gpu_grouped_soa_stream_state state() const noexcept {
        return state_;
    }
    [[nodiscard]] std::span<const gpu_grouped_soa_stream_slot> slots() const noexcept {
        return slots_;
    }
    [[nodiscard]] std::span<const bioseq::gpu::vulkan::gpu_persisted_group_capture_wire>
    captures() const noexcept {
        return captures_;
    }
    [[nodiscard]] const gpu_grouped_soa_stream_progress& progress() const noexcept {
        return progress_;
    }
    [[nodiscard]] std::uint64_t reserved_bytes() const noexcept {
        return permit_.bytes();
    }
    [[nodiscard]] std::uint64_t stream_identity() const noexcept {
        return stream_identity_;
    }
    [[nodiscard]] const bioseq::gpu::vulkan::gpu_device_uuid& device_uuid() const noexcept {
        return device_uuid_;
    }
    [[nodiscard]] const gpu_grouped_soa_stream_completion&
    authorization_completion() const noexcept {
        return authorization_completion_;
    }
    [[nodiscard]] std::uint64_t authorization_receipt_identity() const noexcept {
        return authorization_receipt_identity_;
    }
    [[nodiscard]] static constexpr bool qualifies_as_all_gpu_execution() noexcept {
        return false;
    }

  private:
    gpu_grouped_soa_stream_ring(
        bioseq::parallelism::memory_permit permit,
        std::vector<gpu_grouped_soa_stream_slot> slots,
        std::vector<bioseq::gpu::vulkan::gpu_persisted_group_capture_wire> captures,
        std::uint64_t stream_identity,
        bioseq::gpu::vulkan::gpu_device_uuid device_uuid,
        const gpu_grouped_soa_stream_plan& plan,
        std::uint64_t allocated_device_bytes,
        std::uint64_t historically_admitted_bytes) noexcept
        : permit_(std::move(permit)), slots_(std::move(slots)), captures_(std::move(captures)),
          plan_identity_(plan.identity()), stream_identity_(stream_identity),
          device_uuid_(device_uuid), payload_capacity_bytes_(plan.limits().payload_capacity_bytes),
          descriptor_capacity_(plan.limits().descriptor_capacity),
          control_group_count_(static_cast<std::uint32_t>(plan.control_groups().size())),
          position_group_count_(static_cast<std::uint32_t>(plan.position_groups().size())),
          expected_payload_rows_(plan.expected_payload_rows()),
          payload_checksum_expected_(plan.payload_checksum_expected()),
          state_(control_group_count_ == 0u ? gpu_grouped_soa_stream_state::authorization_pending
                                            : gpu_grouped_soa_stream_state::controls) {
        progress_.allocated_device_bytes = allocated_device_bytes;
        progress_.historically_admitted_bytes = historically_admitted_bytes;
    }

    [[nodiscard]] gpu_grouped_soa_stream_slot*
    find_slot_(const gpu_grouped_soa_stream_slot_token& token) noexcept {
        if (token.stream_identity != stream_identity_ || token.slot_ordinal >= slots_.size())
            return nullptr;
        auto& slot = slots_[token.slot_ordinal];
        return slot.allocation_generation_ == token.allocation_generation &&
                       slot.group_ordinal_ == token.group_ordinal && slot.phase_ == token.phase &&
                       slot.receipt_identity_ == token.receipt_identity
                   ? &slot
                   : nullptr;
    }
    [[nodiscard]] const gpu_grouped_soa_stream_slot*
    find_slot_(const gpu_grouped_soa_stream_slot_token& token) const noexcept {
        if (token.stream_identity != stream_identity_ || token.slot_ordinal >= slots_.size())
            return nullptr;
        const auto& slot = slots_[token.slot_ordinal];
        return slot.allocation_generation_ == token.allocation_generation &&
                       slot.group_ordinal_ == token.group_ordinal && slot.phase_ == token.phase &&
                       slot.receipt_identity_ == token.receipt_identity
                   ? &slot
                   : nullptr;
    }

    [[nodiscard]] std::expected<void, gpu_grouped_soa_ingest_error>
    accept_chunk_(const gpu_grouped_soa_stream_plan& plan,
                  const gpu_grouped_soa_stream_slot_token& token,
                  const index_chunk_address& address,
                  std::span<const std::byte> bytes,
                  std::uint64_t admitted_row_checksum) noexcept {
        if (plan.identity() != plan_identity_) {
            return fail<void>(gpu_grouped_soa_ingest_error::invalid_identity);
        }
        auto* slot = find_slot_(token);
        if (slot == nullptr || slot->state_ != gpu_grouped_soa_stream_slot_state::host_filling) {
            return fail<void>(gpu_grouped_soa_ingest_error::stale_slot);
        }
        const auto& groups = token.phase == bioseq::gpu::vulkan::gpu_shape_v5_group_phase::controls
                                 ? plan.control_groups()
                                 : plan.position_groups();
        if (token.group_ordinal >= groups.size()) {
            return fail<void>(gpu_grouped_soa_ingest_error::unauthorized_extent);
        }
        const auto& group = groups[token.group_ordinal];
        const auto extents = plan.extents(group);
        if (slot->extent_cursor_ >= extents.size()) {
            return fail<void>(gpu_grouped_soa_ingest_error::byte_count);
        }
        const auto& extent = extents[slot->extent_cursor_];
        const auto& request = extent.request;
        if (slot->chunk_cursor_ >= request.expected_chunk_count ||
            address.generation_id != request.generation_id ||
            address.position_segment != request.position_segment ||
            address.sub_key != request.sub_key ||
            address.chunk_no != request.first_chunk_no + slot->chunk_cursor_) {
            return fail<void>(gpu_grouped_soa_ingest_error::unauthorized_extent);
        }
        const std::uint64_t chunk_offset =
            static_cast<std::uint64_t>(slot->chunk_cursor_) * k_grouped_soa_extent_payload_bytes;
        if (chunk_offset >= request.expected_total_bytes) {
            return fail<void>(gpu_grouped_soa_ingest_error::byte_count);
        }
        const std::uint64_t expected_bytes = std::min<std::uint64_t>(
            k_grouped_soa_extent_payload_bytes, request.expected_total_bytes - chunk_offset);
        const std::uint64_t destination_offset =
            bioseq::common::saturating_add(extent.payload_offset, chunk_offset);
        if (bytes.size() != expected_bytes ||
            destination_offset == (std::numeric_limits<std::uint64_t>::max)() ||
            destination_offset > group.payload_bytes ||
            bytes.size() > group.payload_bytes - destination_offset) {
            return fail<void>(gpu_grouped_soa_ingest_error::byte_count);
        }
        if (progress_.validated_receipt_rows >= expected_payload_rows_) {
            return fail<void>(gpu_grouped_soa_ingest_error::completion_mismatch);
        }
        std::memcpy(static_cast<std::byte*>(slot->payload_.mapped) + destination_offset,
                    bytes.data(),
                    bytes.size());
        slot->bytes_received_ = bioseq::common::saturating_add(slot->bytes_received_, bytes.size());
        if (slot->bytes_received_ == (std::numeric_limits<std::uint64_t>::max)()) {
            return fail<void>(gpu_grouped_soa_ingest_error::arithmetic_overflow);
        }
        payload_row_crc_.update_little_endian(admitted_row_checksum);
        ++progress_.validated_receipt_rows;
        if (token.phase == bioseq::gpu::vulkan::gpu_shape_v5_group_phase::controls) {
            progress_.control_bytes_copied += bytes.size();
        } else {
            progress_.position_bytes_copied += bytes.size();
        }
        ++slot->chunk_cursor_;
        if (slot->chunk_cursor_ == request.expected_chunk_count) {
            ++slot->extent_cursor_;
            slot->chunk_cursor_ = 0u;
            if (slot->extent_cursor_ == extents.size()) {
                if (slot->bytes_received_ != group.payload_bytes) {
                    return fail<void>(gpu_grouped_soa_ingest_error::byte_count);
                }
                slot->state_ = gpu_grouped_soa_stream_slot_state::ready_to_submit;
            }
        }
        return {};
    }

    template <class p_value>
    [[nodiscard]] std::expected<p_value, gpu_grouped_soa_ingest_error>
    fail(gpu_grouped_soa_ingest_error error) noexcept {
        state_ = gpu_grouped_soa_stream_state::poisoned;
        return std::unexpected(error);
    }

    // Destroy buffers and captures before releasing their reserved, memory budget.
    bioseq::parallelism::memory_permit permit_{};
    std::vector<gpu_grouped_soa_stream_slot> slots_{};
    std::vector<bioseq::gpu::vulkan::gpu_persisted_group_capture_wire> captures_{};
    crc64_ecma182_accumulator payload_row_crc_{};
    gpu_grouped_soa_stream_progress progress_{};
    gpu_grouped_soa_stream_completion authorization_completion_{};
    gpu_grouped_soa_stream_plan_identity plan_identity_{};
    std::uint64_t stream_identity_ = 0u;
    bioseq::gpu::vulkan::gpu_device_uuid device_uuid_{};
    std::uint64_t runtime_generation_ = 0u;
    std::uint64_t last_submission_identity_ = 0u;
    std::uint64_t last_submission_signal_ = 0u;
    std::uint64_t authorization_receipt_identity_ = 0u;
    std::uint64_t expected_payload_rows_ = 0u;
    std::uint64_t payload_checksum_expected_ = 0u;
    std::uint32_t payload_capacity_bytes_ = 0u;
    std::uint32_t descriptor_capacity_ = 0u;
    std::uint32_t control_group_count_ = 0u;
    std::uint32_t position_group_count_ = 0u;
    std::uint32_t next_control_group_to_submit_ = 0u;
    std::uint32_t next_control_group_to_retire_ = 0u;
    std::uint32_t next_position_group_to_submit_ = 0u;
    std::uint32_t next_position_group_to_retire_ = 0u;
    gpu_grouped_soa_stream_state state_ = gpu_grouped_soa_stream_state::poisoned;
};

/// Loads one grouped minimizer generation into its final GPU buffers.
class gpu_grouped_soa_generation_ingest;

/// A grouped minimizer  generation kept in GPU memory for reuse.
/// Controls and segment-local positions keep their stored bytes. The extra
/// descriptor buffer tells shaders where each partition and its arrays begin.
class gpu_grouped_soa_resident_target {
  private:
    /// Allow only the generation loader to construct this target.
    struct construction_token {};

  public:
    /// Take ownership of the device buffers, their memory permits, and generation metadata.
    /// Only the generation loader can supply the private construction token.
    gpu_grouped_soa_resident_target(
        construction_token,
        bioseq::parallelism::memory_permit permit,
        std::array<bioseq::parallelism::memory_materialization_result, 3> materializations,
        bioseq::gpu::vulkan::buffer controls,
        bioseq::gpu::vulkan::buffer positions,
        bioseq::gpu::vulkan::buffer descriptors,
        gpu_grouped_soa_resident_identity identity,
        std::uint64_t directory_bytes,
        std::uint64_t control_bytes,
        std::uint64_t position_bytes,
        std::uint64_t descriptor_bytes,
        std::uint64_t control_count,
        std::uint64_t position_count,
        std::uint32_t descriptor_count) noexcept
        : permit_(std::move(permit)), materializations_(std::move(materializations)),
          controls_(std::move(controls)), positions_(std::move(positions)),
          descriptors_(std::move(descriptors)), identity_(identity),
          directory_bytes_(directory_bytes), control_bytes_(control_bytes),
          position_bytes_(position_bytes), descriptor_bytes_(descriptor_bytes),
          control_count_(control_count), position_count_(position_count),
          descriptor_count_(descriptor_count) {}

    /// Borrow the GPU buffer containing the stored control arrays.
    [[nodiscard]] const bioseq::gpu::vulkan::buffer& controls() const noexcept {
        return controls_;
    }
    /// Borrow the GPU buffer containing stored positions relative to  their segments.
    [[nodiscard]] const bioseq::gpu::vulkan::buffer& positions() const noexcept {
        return positions_;
    }
    /// Borrow the GPU buffer that describes each partition and its array offsets.
    [[nodiscard]] const bioseq::gpu::vulkan::buffer& descriptors() const noexcept {
        return descriptors_;
    }
    /// Borrow the generation and device identity recorded for these buffers.
    [[nodiscard]] const gpu_grouped_soa_resident_identity& identity() const noexcept {
        return identity_;
    }
    /// Borrow the copy, validation, and memory counters recorded during loading
    [[nodiscard]] const gpu_grouped_soa_cold_load_stats& cold_load_stats() const noexcept {
        return stats_;
    }
    /// Return the total deqice memory admitted when these buffers were allocated.
    [[nodiscard]] std::uint64_t admitted_device_bytes() const noexcept {
        return bioseq::common::saturating_add(
            materializations_[0].admitted_bytes,
            bioseq::common::saturating_add(materializations_[1].admitted_bytes,
                                           materializations_[2].admitted_bytes));
    }
    /// Return the memory budget still reserved for allocations not covered by the measured process, footprint.
    [[nodiscard]] std::uint64_t reserved_device_bytes() const noexcept {
        return permit_.bytes();
    }
    /// Borrow the allocation and process-footprint records for the three buffers.
    [[nodiscard]] const auto& materializations() const noexcept {
        return materializations_;
    }
    /// Return  the validated directory size in bytes.
    [[nodiscard]] std::uint64_t directory_bytes() const noexcept {
        return directory_bytes_;
    }
    /// Return the logical control payload size in bytes.
    [[nodiscard]] std::uint64_t control_bytes() const noexcept {
        return control_bytes_;
    }
    /// Return the logical position payload size in bytes.
    [[nodiscard]] std::uint64_t position_bytes() const noexcept {
        return position_bytes_;
    }
    /// Return the partition descriptor size in bytes.
    [[nodiscard]] std::uint64_t descriptor_bytes() const noexcept {
        return descriptor_bytes_;
    }
    /// Return the total number of hash controls across the partitions.
    [[nodiscard]] std::uint64_t control_count() const noexcept {
        return control_count_;
    }
    /// Return the number of persisted segment-local positions.
    [[nodiscard]] std::uint64_t position_count() const noexcept {
        return position_count_;
    }
    /// Return the number of resident partition descriptors.
    [[nodiscard]] std::uint32_t descriptor_count() const noexcept {
        return descriptor_count_;
    }

    /// A loaded target alone cannot prove that a query ran entirely on the GPU.
    [[nodiscard]] static constexpr bool qualifies_as_all_gpu_execution() noexcept {
        return false;
    }
    /// This loading path requires stored arrays to reach the GPU without host decoding or rebuilding.
    [[nodiscard]] static constexpr bool permits_host_decode_or_rebuild() noexcept {
        return false;
    }

    /// Check that identities, sizes, memory records, and load counters agree.
    /// This checks the recorded state without rereading the payload bytes.
    [[nodiscard]] bool validated() const noexcept {
        const std::uint64_t payload_bytes =
            bioseq::common::saturating_add(control_bytes_, position_bytes_);
        const std::uint64_t expected_directory_bytes = bioseq::common::saturating_mul(
            descriptor_count_, grouped_soa_bucket_directory_entry::encoded_bytes);
        const std::uint64_t expected_descriptor_bytes = bioseq::common::saturating_mul(
            descriptor_count_,
            sizeof(bioseq::gpu::vulkan::gpu_grouped_soa_resident_partition_descriptor));
        const std::uint64_t allocation_bytes = bioseq::common::saturating_add(
            controls_.allocation_bytes,
            bioseq::common::saturating_add(positions_.allocation_bytes,
                                           descriptors_.allocation_bytes));
        std::uint64_t retained_allocation_bytes = 0u;
        bool materializations_valid = true;
        const std::array<std::uint64_t, 3> buffer_allocation_bytes{
            controls_.allocation_bytes,
            positions_.allocation_bytes,
            descriptors_.allocation_bytes,
        };
        for (std::size_t index = 0u; index < materializations_.size(); ++index) {
            const auto& materialization = materializations_[index];
            const auto& snapshot = materialization.allocation_snapshot;
            const bool exact = materialization.admitted_bytes == buffer_allocation_bytes[index] &&
                               snapshot.actual_allocation_bytes == buffer_allocation_bytes[index] &&
                               snapshot.fully_touched_bytes >= buffer_allocation_bytes[index];
            const bool reflected = snapshot.is_host_visible && snapshot.is_host_coherent &&
                                   snapshot.is_process_footprint_reflected &&
                                   bioseq::parallelism::footprint_delta_covers_admission(
                                       materialization.footprint_before_bytes,
                                       materialization.footprint_after_bytes,
                                       materialization.admitted_bytes);
            materializations_valid =
                materializations_valid && exact && materialization.is_permit_released == reflected;
            if (!materialization.is_permit_released) {
                retained_allocation_bytes = bioseq::common::saturating_add(
                    retained_allocation_bytes, materialization.admitted_bytes);
            }
        }
        return is_gpu_grouped_soa_resident_identity_valid(identity_) && controls_.is_valid() &&
               positions_.is_valid() && descriptors_.is_valid() &&
               payload_bytes != (std::numeric_limits<std::uint64_t>::max)() &&
               expected_directory_bytes == directory_bytes_ &&
               expected_descriptor_bytes == descriptor_bytes_ &&
               identity_.directory_bytes == directory_bytes_ &&
               identity_.control_bytes == control_bytes_ &&
               identity_.position_bytes == position_bytes_ &&
               identity_.descriptor_bytes == descriptor_bytes_ &&
               identity_.control_count == control_count_ &&
               identity_.position_count == position_count_ &&
               identity_.descriptor_count == descriptor_count_ &&
               control_bytes_ <= controls_.bytes && position_bytes_ <= positions_.bytes &&
               descriptor_bytes_ <= descriptors_.bytes &&
               allocation_bytes != (std::numeric_limits<std::uint64_t>::max)() &&
               materializations_valid &&
               retained_allocation_bytes != (std::numeric_limits<std::uint64_t>::max)() &&
               retained_allocation_bytes == permit_.bytes() && stats_.cold_cache_misses == 1u &&
               stats_.full_generations_ingested == 1u &&
               stats_.control_extents_validated == descriptor_count_ &&
               stats_.position_extents_validated == descriptor_count_ &&
               stats_.control_bytes_copied == control_bytes_ &&
               stats_.position_bytes_copied == position_bytes_ &&
               stats_.descriptor_bytes_copied == descriptor_bytes_ &&
               stats_.device_allocation_bytes == allocation_bytes &&
               stats_.admitted_device_bytes == allocation_bytes &&
               (stats_.validated_receipt_rows != 0u ||
                (control_bytes_ == 0u && position_bytes_ == 0u && descriptor_count_ == 0u)) &&
               stats_.raw_rehashed_rows == 0u &&
               stats_.payload_hash_byte_visits == 0u && !stats_.has_forbidden_work();
    }

  private:
    // Destroy the buffers before releasing their reserved memory budget.
    bioseq::parallelism::memory_permit permit_{};
    std::array<bioseq::parallelism::memory_materialization_result, 3> materializations_{};
    bioseq::gpu::vulkan::buffer controls_{};
    bioseq::gpu::vulkan::buffer positions_{};
    bioseq::gpu::vulkan::buffer descriptors_{};
    gpu_grouped_soa_resident_identity identity_{};
    gpu_grouped_soa_cold_load_stats stats_{};
    std::uint64_t directory_bytes_ = 0u;
    std::uint64_t control_bytes_ = 0u;
    std::uint64_t position_bytes_ = 0u;
    std::uint64_t descriptor_bytes_ = 0u;
    std::uint64_t control_count_ = 0u;
    std::uint64_t position_count_ = 0u;
    std::uint32_t descriptor_count_ = 0u;

    friend class gpu_grouped_soa_generation_ingest;
};

/// Copy a complete grouped  minimizer generation into its final GPU buffers.
/// The receiver expects all control extents befor all position extents. Each
/// row arrives with a validation receipt, so the loader can check the sequence
/// of row checksums without hashing the payload again. Raw test input can check
/// corruption handling, but cannot produce a published target.
class gpu_grouped_soa_generation_ingest {
  public:
    gpu_grouped_soa_generation_ingest(const gpu_grouped_soa_generation_ingest&) = delete;
    gpu_grouped_soa_generation_ingest& operator=(const gpu_grouped_soa_generation_ingest&) = delete;
    gpu_grouped_soa_generation_ingest(gpu_grouped_soa_generation_ingest&&) noexcept = default;
    gpu_grouped_soa_generation_ingest& operator=(gpu_grouped_soa_generation_ingest&&) = delete;

    /// Allocate the final GPU buffers and plan which stored rows fill each one.
    ///
    /// The directory is borrowed only for this call Its sizes and array widths
    /// determine partition descriptors without decoding control or position  data.
    ///
    /// @tparam p_directory_view Validated directory with a profile, entries, and encoded bytes.
    template <class p_directory_view>
    [[nodiscard]] static std::expected<gpu_grouped_soa_generation_ingest,
                                       gpu_grouped_soa_ingest_error>
    begin(bioseq::gpu::vulkan::runtime& runtime,
          const minimizer_generation_head& head,
          const minimizer_generation_manifest& manifest,
          const expected_minimizer_generation_identity& expected,
          const p_directory_view& directory,
          std::uint64_t cold_load_identity) {
        if constexpr (std::endian::native != std::endian::little) {
            return std::unexpected(gpu_grouped_soa_ingest_error::unsupported_endianness);
        }
        if (!runtime.is_usable() || !is_gpu_device_uuid_bound(runtime.features().device_uuid) ||
            cold_load_identity == 0u) {
            return std::unexpected(gpu_grouped_soa_ingest_error::invalid_identity);
        }
        const auto manifest_profile = derive_grouped_soa_minimizer_profile(manifest);
        const bool compatible =
            manifest_profile &&
            manifest.layout == minimizer_generation_layout::grouped_soa_segment_local &&
            manifest.gpu_abi_revision == k_grouped_soa_gpu_abi_revision &&
            directory.profile().layout == minimizer_generation_layout::grouped_soa_segment_local &&
            directory.profile().gpu_abi_revision == k_grouped_soa_gpu_abi_revision &&
            directory.profile() == *manifest_profile;
        if (!compatible || !validate_minimizer_generation_storage_contract(manifest, expected) ||
            !validate_minimizer_generation_metadata(head, manifest, directory)) {
            return std::unexpected(gpu_grouped_soa_ingest_error::metadata_mismatch);
        }
        const auto entries = directory.entries();
        if (entries.size() > (std::numeric_limits<std::uint32_t>::max)() ||
            entries.size() > (std::numeric_limits<std::size_t>::max)() / 2u) {
            return std::unexpected(gpu_grouped_soa_ingest_error::arithmetic_overflow);
        }

        std::vector<std::uint64_t> control_offsets(entries.size());
        std::vector<std::uint64_t> position_offsets(entries.size());
        std::vector<bioseq::gpu::vulkan::gpu_grouped_soa_resident_partition_descriptor> descriptors;
        descriptors.reserve(entries.size());
        std::uint64_t control_bytes = 0u;
        std::uint64_t position_bytes = 0u;
        std::uint64_t control_count = 0u;
        std::uint64_t position_count = 0u;
        for (std::size_t ordinal = 0u; ordinal < entries.size(); ++ordinal) {
            const grouped_soa_bucket_directory_entry& entry = entries[ordinal];
            if (validate_minimizer_bucket_dir_entry(entry, directory.profile()).has_value() ||
                entry.partition_id != ordinal ||
                entry.control_element_count > (std::numeric_limits<std::uint32_t>::max)() ||
                entry.position_element_count > (std::numeric_limits<std::uint32_t>::max)()) {
                return std::unexpected(gpu_grouped_soa_ingest_error::partition_mismatch);
            }
            const auto core = derive_minimizer_position_segment_bounds(
                manifest, static_cast<std::uint32_t>(entry.position_segment));
            const auto layout = plan_grouped_soa_control_plane(entry.control_element_count,
                                                               manifest.hash_width,
                                                               entry.shape_offset_width,
                                                               manifest.count_width);
            if (!core || !layout ||
                control_bytes % static_cast<std::uint8_t>(manifest.hash_width) != 0u ||
                position_bytes % static_cast<std::uint8_t>(manifest.position_width) != 0u ||
                control_bytes >
                    (std::numeric_limits<std::uint64_t>::max)() - entry.control_encoded_bytes ||
                position_bytes >
                    (std::numeric_limits<std::uint64_t>::max)() - entry.position_encoded_bytes ||
                control_count >
                    (std::numeric_limits<std::uint64_t>::max)() - entry.control_element_count ||
                position_count >
                    (std::numeric_limits<std::uint64_t>::max)() - entry.position_element_count) {
                return std::unexpected(gpu_grouped_soa_ingest_error::arithmetic_overflow);
            }
            control_offsets[ordinal] = control_bytes;
            position_offsets[ordinal] = position_bytes;
            descriptors.push_back(
                bioseq::gpu::vulkan::gpu_grouped_soa_resident_partition_descriptor{
                    .partition_id = entry.partition_id,
                    .directory_ordinal = static_cast<std::uint32_t>(ordinal),
                    .position_segment = static_cast<std::uint32_t>(entry.position_segment),
                    .flags = bioseq::gpu::vulkan::k_gpu_grouped_soa_partition_required_flags,
                    .hash_byte_offset = control_bytes + layout->hash_offset,
                    .prefix_byte_offset = control_bytes + layout->prefix_offset,
                    .count_byte_offset = control_bytes + layout->count_offset,
                    .position_byte_offset = position_bytes,
                    .control_count = static_cast<std::uint32_t>(entry.control_element_count),
                    .position_count = static_cast<std::uint32_t>(entry.position_element_count),
                    .hash_width = static_cast<std::uint32_t>(manifest.hash_width),
                    .offset_width = static_cast<std::uint32_t>(entry.shape_offset_width),
                    .count_width = static_cast<std::uint32_t>(manifest.count_width),
                    .position_width = static_cast<std::uint32_t>(manifest.position_width),
                    .low_hash = entry.lo_hash,
                    .high_hash = entry.hi_hash,
                    .segment_base_bp = core->begin_bp,
                    .segment_end_bp = core->end_bp,
                });
            control_bytes += entry.control_encoded_bytes;
            position_bytes += entry.position_encoded_bytes;
            control_count += entry.control_element_count;
            position_count += entry.position_element_count;
        }
        const std::uint64_t payload_bytes =
            bioseq::common::saturating_add(control_bytes, position_bytes);
        const std::uint64_t descriptor_bytes = bioseq::common::saturating_mul(
            descriptors.size(),
            sizeof(bioseq::gpu::vulkan::gpu_grouped_soa_resident_partition_descriptor));
        if (payload_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            descriptor_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            payload_bytes != manifest.payload_bytes || control_count != manifest.control_count ||
            position_count != manifest.position_count ||
            directory.encoded_bytes().size() != manifest.directory_bytes ||
            manifest.directory_bytes != static_cast<std::uint64_t>(entries.size()) *
                                            grouped_soa_bucket_directory_entry::encoded_bytes) {
            return std::unexpected(gpu_grouped_soa_ingest_error::metadata_mismatch);
        }

        // A buffer may exceed the maximum shader binding range. Check that each
        // binding window can cover complete partitions within that limit.
        constexpr std::uint64_t word_limit =
            std::uint64_t{(std::numeric_limits<std::uint32_t>::max)()} * 4u;
        if (control_bytes > word_limit || position_bytes > word_limit) {
            return std::unexpected(gpu_grouped_soa_ingest_error::arithmetic_overflow);
        }
        const std::uint64_t control_request = std::max<std::uint64_t>((control_bytes + 3u) & ~std::uint64_t{3u}, 16u);
        const std::uint64_t position_request = std::max<std::uint64_t>((position_bytes + 3u) & ~std::uint64_t{3u}, 16u);
        const std::uint64_t descriptor_request = std::max<std::uint64_t>(descriptor_bytes, 16u);
        if (descriptor_request > runtime.features().max_storage_buffer_range) {
            return std::unexpected(gpu_grouped_soa_ingest_error::arithmetic_overflow);
        }
        for (std::uint32_t first = 0u; first < descriptors.size();) {
            const auto window = bioseq::gpu::vulkan::plan_gpu_grouped_soa_buffer_window(
                std::as_bytes(std::span{descriptors}), first, control_bytes, position_bytes,
                runtime.features().max_storage_buffer_range,
                runtime.features().min_storage_buffer_offset_alignment);
            if (!window) {
                return std::unexpected(gpu_grouped_soa_ingest_error::arithmetic_overflow);
            }
            first = window->partition_end;
        }
        constexpr VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        const std::uint64_t control_admission =
            runtime.buffer_allocation_requirement(control_request, usage);
        const std::uint64_t position_admission =
            runtime.buffer_allocation_requirement(position_request, usage);
        const std::uint64_t descriptor_admission =
            runtime.buffer_allocation_requirement(descriptor_request, usage);
        const std::uint64_t admitted_bytes = bioseq::common::saturating_add(
            control_admission,
            bioseq::common::saturating_add(position_admission, descriptor_admission));
        if (admitted_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(gpu_grouped_soa_ingest_error::arithmetic_overflow);
        }
        bioseq::parallelism::memory_permit retained_permit{};
        std::array<bioseq::parallelism::memory_materialization_result, 3> materializations{};
        bioseq::gpu::vulkan::buffer control_buffer{};
        bioseq::gpu::vulkan::buffer position_buffer{};
        bioseq::gpu::vulkan::buffer descriptor_buffer{};
        auto& admission_controller = bioseq::parallelism::memory_admission_controller::instance();

        // Allocate and touch each buffer in a separate memory-admission transaction.
        // Release its permit once the measured process footprint covers the
        // allocation. Otherwise keep that permit for  the buffer lifetime.
        const auto materialize_buffer =
            [&runtime, &admission_controller, &retained_permit, &materializations, usage](
                const std::uint64_t logical_bytes,
                const std::uint64_t allocation_requirement,
                bioseq::gpu::vulkan::buffer& destination,
                const std::size_t receipt_index)
            -> std::expected<void, gpu_grouped_soa_ingest_error> {
            auto permit = admission_controller.try_reserve(allocation_requirement);
            if (!permit) {
                return std::unexpected(gpu_grouped_soa_ingest_error::admission);
            }
            bool allocation_mismatch = false;
            materializations[receipt_index] = admission_controller.materialize(
                *permit,
                [&runtime,
                 &destination,
                 logical_bytes,
                 allocation_requirement,
                 usage,
                 &allocation_mismatch]() -> bioseq::parallelism::memory_materialization_snapshot {
                    destination = runtime.create_device_buffer(logical_bytes, usage);
                    if (destination.allocation_bytes != allocation_requirement) {
                        allocation_mismatch = true;
                        destination.reset();
                        return {};
                    }
                    if (destination.mapped == nullptr || !destination.is_host_visible_coherent()) {
                        return {
                            .actual_allocation_bytes = destination.allocation_bytes,
                            .is_host_visible = destination.mapped != nullptr,
                            .is_host_coherent = destination.is_host_visible_coherent(),
                            .is_process_footprint_reflected = false,
                        };
                    }
                    std::memset(destination.mapped,
                                0,
                                static_cast<std::size_t>(destination.allocation_bytes));
                    return {
                        .actual_allocation_bytes = destination.allocation_bytes,
                        .fully_touched_bytes = destination.allocation_bytes,
                        .is_host_visible = true,
                        .is_host_coherent = true,
                        .is_process_footprint_reflected = destination.process_footprint_reflected,
                    };
                });
            if (allocation_mismatch) {
                return std::unexpected(gpu_grouped_soa_ingest_error::admission);
            }
            if (destination.mapped == nullptr || !destination.is_host_visible_coherent()) {
                // Free the rejected allocation before the local permit releases its budget.
                destination.reset();
                return std::unexpected(gpu_grouped_soa_ingest_error::non_mappable_device_memory);
            }
            if (*permit) {
                if (!retained_permit) {
                    retained_permit = std::move(*permit);
                } else if (!retained_permit.absorb(std::move(*permit))) {
                    destination.reset();
                    return std::unexpected(gpu_grouped_soa_ingest_error::admission);
                }
            }
            return {};
        };

        if (auto result =
                materialize_buffer(control_request, control_admission, control_buffer, 0u);
            !result) {
            return std::unexpected(result.error());
        }
        if (auto result =
                materialize_buffer(position_request, position_admission, position_buffer, 1u);
            !result) {
            return std::unexpected(result.error());
        }
        if (auto result =
                materialize_buffer(descriptor_request, descriptor_admission, descriptor_buffer, 2u);
            !result) {
            return std::unexpected(result.error());
        }
        const std::uint64_t allocation_bytes = bioseq::common::saturating_add(
            control_buffer.allocation_bytes,
            bioseq::common::saturating_add(position_buffer.allocation_bytes,
                                           descriptor_buffer.allocation_bytes));
        if (allocation_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            allocation_bytes != admitted_bytes) {
            return std::unexpected(gpu_grouped_soa_ingest_error::admission);
        }
        if (descriptor_bytes != 0u) {
            std::memcpy(descriptor_buffer.mapped,
                        descriptors.data(),
                        static_cast<std::size_t>(descriptor_bytes));
        }

        const gpu_grouped_soa_resident_identity identity{
            .layout = manifest.layout,
            .generation_id = manifest.generation_id,
            .source_identity = manifest.source_identity,
            .source_content_checksum = manifest.source_content_checksum,
            .storage_contract_checksum = head.storage_contract_checksum,
            .manifest_checksum = head.manifest_checksum,
            .directory_checksum = manifest.directory_checksum,
            .payload_checksum = manifest.payload_checksum,
            .cold_load_identity = cold_load_identity,
            .directory_bytes = manifest.directory_bytes,
            .control_bytes = control_bytes,
            .position_bytes = position_bytes,
            .descriptor_bytes = descriptor_bytes,
            .control_count = control_count,
            .position_count = position_count,
            .descriptor_count = static_cast<std::uint32_t>(entries.size()),
            .device_uuid = runtime.features().device_uuid,
            .gpu_abi_revision = manifest.gpu_abi_revision,
        };
        auto resident = std::make_shared<gpu_grouped_soa_resident_target>(
            gpu_grouped_soa_resident_target::construction_token{},
            std::move(retained_permit),
            std::move(materializations),
            std::move(control_buffer),
            std::move(position_buffer),
            std::move(descriptor_buffer),
            identity,
            manifest.directory_bytes,
            control_bytes,
            position_bytes,
            descriptor_bytes,
            control_count,
            position_count,
            static_cast<std::uint32_t>(entries.size()));
        resident->stats_.descriptor_bytes_copied = descriptor_bytes;

        std::vector<extent_plan> plans;
        std::vector<index_sub_key_request> requests;
        plans.reserve(entries.size() * 2u);
        requests.reserve(entries.size() * 2u);
        // Add one control or position exteqt to the ordered receive, plan.
        const auto append_plan =
            [entries, &manifest, &control_offsets, &position_offsets](
                std::size_t ordinal, bool position, auto& plan_out, auto& request_out) -> bool {
            const grouped_soa_bucket_directory_entry& entry = entries[ordinal];
            const auto position_sub_key = derive_minimizer_position_extent_sub_key(entry.sub_key);
            if (!position_sub_key)
                return false;
            const index_sub_key_request request{
                .generation_id = manifest.generation_id,
                .position_segment = entry.position_segment,
                .sub_key = position ? *position_sub_key : entry.sub_key,
                .first_chunk_no = position ? entry.position_first_chunk : entry.control_first_chunk,
                .expected_chunk_count =
                    position ? entry.position_chunk_count : entry.control_chunk_count,
                .expected_total_bytes =
                    position ? entry.position_encoded_bytes : entry.control_encoded_bytes,
                .expected_checksum = position ? entry.position_checksum : entry.control_checksum,
            };
            plan_out.push_back(extent_plan{
                .request = request,
                .destination_offset =
                    position ? position_offsets[ordinal] : control_offsets[ordinal],
                .position = position,
            });
            request_out.push_back(request);
            return true;
        };
        for (std::size_t ordinal = 0u; ordinal < entries.size(); ++ordinal) {
            if (!append_plan(ordinal, false, plans, requests)) {
                return std::unexpected(gpu_grouped_soa_ingest_error::partition_mismatch);
            }
        }
        for (std::size_t ordinal = 0u; ordinal < entries.size(); ++ordinal) {
            if (!append_plan(ordinal, true, plans, requests)) {
                return std::unexpected(gpu_grouped_soa_ingest_error::partition_mismatch);
            }
        }
        std::uint64_t expected_payload_rows = 0u;
        for (const index_sub_key_request& request : requests) {
            if (expected_payload_rows >
                (std::numeric_limits<std::uint64_t>::max)() - request.expected_chunk_count) {
                return std::unexpected(gpu_grouped_soa_ingest_error::arithmetic_overflow);
            }
            expected_payload_rows += request.expected_chunk_count;
        }
        return gpu_grouped_soa_generation_ingest{std::move(resident),
                                                 std::move(plans),
                                                 std::move(requests),
                                                 manifest.payload_checksum,
                                                 expected_payload_rows};
    }

    /// Allocate GPU buffers from the publisher's plan before it emits the rows.
    /// Payload checksums are still unknown. After streaming,
    /// seal_planned_generation must check the final metadata before finish
    /// can publish the target.
    [[nodiscard]] static std::expected<gpu_grouped_soa_generation_ingest,
                                       gpu_grouped_soa_ingest_error>
    begin_planned(
        bioseq::gpu::vulkan::runtime& runtime,
        const direct_grouped_soa_generation_plan_view& planned,
        const expected_minimizer_generation_identity& expected,
        std::uint64_t cold_load_identity) {
        const auto& storage = planned.storage_plan;
        const auto& options = planned.options;
        const auto& generation = planned.generation_plan;
        const auto canonical_storage =
            minimizer_generation_stream_detail::make_minimizer_generation_storage_plan(
                options, generation);
        const bool checksum_slots_are_fresh = std::ranges::all_of(
            generation.entries,
            [](const grouped_soa_bucket_directory_entry& entry) noexcept {
                return entry.control_checksum == 0u && entry.position_checksum == 0u;
            });
        if (!canonical_storage || storage != *canonical_storage ||
            !checksum_slots_are_fresh) {
            return std::unexpected(gpu_grouped_soa_ingest_error::metadata_mismatch);
        }
        auto provisional_directory_bytes =
            encode_minimizer_bucket_directory(generation.entries, generation.profile);
        if (!provisional_directory_bytes ||
            provisional_directory_bytes->size() != storage.directory_bytes) {
            return std::unexpected(gpu_grouped_soa_ingest_error::metadata_mismatch);
        }
        auto provisional_directory = grouped_soa_directory_view::decode(
            *provisional_directory_bytes, generation.profile);
        auto provisional_manifest = make_grouped_soa_generation_manifest(
            options,
            generation,
            provisional_directory_bytes->size(),
            crc64_ecma182(*provisional_directory_bytes),
            0u);
        if (!provisional_directory || !provisional_manifest ||
            provisional_manifest->payload_bytes != storage.payload_bytes) {
            return std::unexpected(gpu_grouped_soa_ingest_error::metadata_mismatch);
        }
        const auto provisional_manifest_bytes =
            encode_minimizer_generation_manifest_record(*provisional_manifest);
        if (!provisional_manifest_bytes ||
            provisional_manifest_bytes->size() != storage.manifest_bytes) {
            return std::unexpected(gpu_grouped_soa_ingest_error::metadata_mismatch);
        }
        const minimizer_generation_head provisional_head{
            .layout = options.layout,
            .generation_id = options.generation_id,
            .source_identity = options.source_identity,
            .storage_contract_checksum =
                compute_minimizer_generation_storage_contract_checksum(*provisional_manifest),
            .manifest_checksum = crc64_ecma182(
                std::span<const std::byte>{*provisional_manifest_bytes}),
            .source_content_checksum = options.source_content_checksum,
        };
        auto begun = begin(runtime,
                           provisional_head,
                           *provisional_manifest,
                           expected,
                           *provisional_directory,
                           cold_load_identity);
        if (!begun) {
            return std::unexpected(begun.error());
        }
        auto directory_shape = normalized_directory_shape_checksum_(
            generation.profile, generation.entries);
        if (!directory_shape) {
            return std::unexpected(directory_shape.error());
        }
        provisional_manifest->payload_checksum = 0u;
        provisional_manifest->directory_checksum = 0u;
        begun->planned_manifest_ = *provisional_manifest;
        begun->planned_directory_shape_checksum_ = *directory_shape;
        begun->metadata_sealed_ = false;
        return begun;
    }

    /// Check that the final metadata matches the plan used to receive the payload.
    /// Save its checksums so  finish can verify completion and publish the target.
    [[nodiscard]] std::expected<void, gpu_grouped_soa_ingest_error>
    seal_planned_generation(
        const minimizer_generation_head& head,
        const minimizer_generation_manifest& manifest,
        const expected_minimizer_generation_identity& expected,
        const grouped_soa_directory_view& directory) noexcept {
        if (failure_) {
            return std::unexpected(gpu_grouped_soa_ingest_error::poisoned_stream);
        }
        if (metadata_sealed_ || !planned_manifest_ || !resident_ ||
            extent_cursor_ != plans_.size() || chunk_cursor_ != 0u) {
            return fail<void>(gpu_grouped_soa_ingest_error::wrong_stream_phase);
        }
        auto normalized_manifest = manifest;
        normalized_manifest.payload_checksum = 0u;
        normalized_manifest.directory_checksum = 0u;
        const auto directory_shape = normalized_directory_shape_checksum_(
            directory.profile(), directory.entries());
        if (!directory_shape || normalized_manifest != *planned_manifest_ ||
            *directory_shape != planned_directory_shape_checksum_ ||
            !validate_minimizer_generation_storage_contract(manifest, expected) ||
            !validate_minimizer_generation_metadata(head, manifest, directory)) {
            return fail<void>(gpu_grouped_soa_ingest_error::metadata_mismatch);
        }
        const auto entries = directory.entries();
        if (entries.size() > (std::numeric_limits<std::size_t>::max)() / 2u ||
            plans_.size() != entries.size() * 2u || requests_.size() != plans_.size()) {
            return fail<void>(gpu_grouped_soa_ingest_error::completion_mismatch);
        }
        const auto request_shape_matches = [](
            const index_sub_key_request& lhs,
            const index_sub_key_request& rhs) noexcept {
            return lhs.generation_id == rhs.generation_id &&
                   lhs.position_segment == rhs.position_segment &&
                   lhs.sub_key == rhs.sub_key &&
                   lhs.first_chunk_no == rhs.first_chunk_no &&
                   lhs.expected_chunk_count == rhs.expected_chunk_count &&
                   lhs.expected_total_bytes == rhs.expected_total_bytes;
        };
        for (std::size_t ordinal = 0u; ordinal < entries.size(); ++ordinal) {
            const auto& entry = entries[ordinal];
            const auto position_sub_key =
                derive_minimizer_position_extent_sub_key(entry.sub_key);
            if (!position_sub_key) {
                return fail<void>(gpu_grouped_soa_ingest_error::partition_mismatch);
            }
            const index_sub_key_request finalized_control{
                .generation_id = manifest.generation_id,
                .position_segment = entry.position_segment,
                .sub_key = entry.sub_key,
                .first_chunk_no = entry.control_first_chunk,
                .expected_chunk_count = entry.control_chunk_count,
                .expected_total_bytes = entry.control_encoded_bytes,
                .expected_checksum = entry.control_checksum,
            };
            const index_sub_key_request finalized_position{
                .generation_id = manifest.generation_id,
                .position_segment = entry.position_segment,
                .sub_key = *position_sub_key,
                .first_chunk_no = entry.position_first_chunk,
                .expected_chunk_count = entry.position_chunk_count,
                .expected_total_bytes = entry.position_encoded_bytes,
                .expected_checksum = entry.position_checksum,
            };
            const std::size_t position_ordinal = entries.size() + ordinal;
            if (!request_shape_matches(plans_[ordinal].request, finalized_control) ||
                !request_shape_matches(requests_[ordinal], finalized_control) ||
                !request_shape_matches(plans_[position_ordinal].request, finalized_position) ||
                !request_shape_matches(requests_[position_ordinal], finalized_position)) {
                return fail<void>(gpu_grouped_soa_ingest_error::metadata_mismatch);
            }
            plans_[ordinal].request.expected_checksum = entry.control_checksum;
            requests_[ordinal].expected_checksum = entry.control_checksum;
            plans_[position_ordinal].request.expected_checksum = entry.position_checksum;
            requests_[position_ordinal].expected_checksum = entry.position_checksum;
        }
        resident_->identity_.storage_contract_checksum = head.storage_contract_checksum;
        resident_->identity_.manifest_checksum = head.manifest_checksum;
        resident_->identity_.directory_checksum = manifest.directory_checksum;
        resident_->identity_.payload_checksum = manifest.payload_checksum;
        payload_checksum_expected_ = manifest.payload_checksum;
        planned_manifest_.reset();
        metadata_sealed_ = true;
        return {};
    }

    /// Borrow the extent requests in receive order, with controls before positions.
    [[nodiscard]] std::span<const index_sub_key_request> requests() const noexcept {
        return requests_;
    }

    /// Copy one validated row into the next expected region of its GPU buffer
    /// A row or ordering mismatch stops the load and prevents publication.
    [[nodiscard]] std::expected<void, gpu_grouped_soa_ingest_error>
    accept_validated_row(const validated_minimizer_generation_row_receipt& receipt) noexcept {
        std::expected<void, gpu_grouped_soa_ingest_error> accepted{};
        const bool visited = visit_validated_minimizer_generation_row(
            receipt,
            [this, &accepted](const index_chunk_address& address,
                              std::span<const std::byte> bytes,
                              std::uint64_t row_checksum) noexcept {
                accepted = accept_chunk_impl(
                    address, bytes, row_checksum, input_authority::validated_receipt);
            });
        if (!visited) {
            return fail<void>(gpu_grouped_soa_ingest_error::unauthorized_extent);
        }
        return accepted;
    }

    /// Hash and accept raw bytes for corruption tests.
    /// A receiver that uses this path cannot publish a target through finish.
    [[nodiscard]] std::expected<void, gpu_grouped_soa_ingest_error>
    accept_raw_chunk_for_test(const index_chunk_address& address,
                              std::span<const std::byte> bytes) noexcept {
        if (bytes.size() > (std::numeric_limits<std::uint32_t>::max)()) {
            return fail<void>(gpu_grouped_soa_ingest_error::byte_count);
        }
        const std::uint64_t row_checksum =
            crc64_index_chunk_row(address.generation_id,
                                  address.position_segment,
                                  address.sub_key,
                                  address.chunk_no,
                                  static_cast<std::uint32_t>(bytes.size()),
                                  bytes);
        return accept_chunk_impl(address, bytes, row_checksum, input_authority::raw_rehashed);
    }

    [[nodiscard]] std::expected<void, gpu_grouped_soa_ingest_error>
    accept_raw_chunk_for_test(const index_chunk_address& address,
                              const char* payload,
                              std::size_t payload_bytes) noexcept {
        if (payload == nullptr && payload_bytes != 0u) {
            return fail<void>(gpu_grouped_soa_ingest_error::byte_count);
        }
        return accept_raw_chunk_for_test(
            address,
            std::span<const std::byte>{reinterpret_cast<const std::byte*>(payload), payload_bytes});
    }

    /// Verify the completed load and  publish the target for reuse.
    /// Every requested row must arrive in order with a validation receipt, and
    /// their combined checksum must match the final manifest.
    [[nodiscard]] std::expected<std::shared_ptr<const gpu_grouped_soa_resident_target>,
                                gpu_grouped_soa_ingest_error>
    finish() {
        if (failure_)
            return std::unexpected(*failure_);
        if (!metadata_sealed_ || planned_manifest_) {
            return fail<std::shared_ptr<const gpu_grouped_soa_resident_target>>(
                gpu_grouped_soa_ingest_error::authorization_pending);
        }
        if (!resident_ || extent_cursor_ != plans_.size() || chunk_cursor_ != 0u) {
            return fail<std::shared_ptr<const gpu_grouped_soa_resident_target>>(
                gpu_grouped_soa_ingest_error::byte_count);
        }
        // Validated receipts confirm that each row was checked at its source.
        // The checksum below checks that those rows arrived in the order
        // recorded by tis manifest.
        const bool receipt_authority_complete = expected_payload_rows_ == 0u
            ? input_authority_ == input_authority::unset
            : input_authority_ == input_authority::validated_receipt;
        if (!receipt_authority_complete ||
            resident_->stats_.validated_receipt_rows != expected_payload_rows_ ||
            resident_->stats_.raw_rehashed_rows != 0u ||
            resident_->stats_.payload_hash_byte_visits != 0u) {
            return fail<std::shared_ptr<const gpu_grouped_soa_resident_target>>(
                gpu_grouped_soa_ingest_error::completion_mismatch);
        }
        if (payload_row_crc_.checksum() != payload_checksum_expected_) {
            return fail<std::shared_ptr<const gpu_grouped_soa_resident_target>>(
                gpu_grouped_soa_ingest_error::checksum);
        }
        resident_->stats_.cold_cache_misses = 1u;
        resident_->stats_.full_generations_ingested = 1u;
        resident_->stats_.device_allocation_bytes = resident_->controls_.allocation_bytes +
                                                    resident_->positions_.allocation_bytes +
                                                    resident_->descriptors_.allocation_bytes;
        resident_->stats_.admitted_device_bytes = resident_->admitted_device_bytes();
        if (!resident_->validated()) {
            return fail<std::shared_ptr<const gpu_grouped_soa_resident_target>>(
                gpu_grouped_soa_ingest_error::completion_mismatch);
        }
        std::shared_ptr<const gpu_grouped_soa_resident_target> published = resident_;
        resident_.reset();
        return published;
    }

  private:
    /// Fix the input type on the first chunk so validated and raw test rows cannot be mixed.
    enum class input_authority : std::uint8_t {
        unset,
        validated_receipt,
        raw_rehashed,
    };

    /// Checksum the directory layout with extent checksums set to zero.
    /// This lets the final directory be compared with its pre-emission plan.
    [[nodiscard]] static std::expected<std::uint64_t, gpu_grouped_soa_ingest_error>
    normalized_directory_shape_checksum_(
        const grouped_soa_minimizer_profile& profile,
        std::span<const grouped_soa_bucket_directory_entry> entries) noexcept {
        crc64_ecma182_accumulator checksum{};
        for (const grouped_soa_bucket_directory_entry& entry : entries) {
            auto normalized = entry;
            normalized.control_checksum = 0u;
            normalized.position_checksum = 0u;
            const auto encoded =
                encode_minimizer_bucket_dir_entry_record(normalized, profile);
            if (!encoded) {
                return std::unexpected(gpu_grouped_soa_ingest_error::partition_mismatch);
            }
            checksum.update(std::span<const std::byte>{encoded->data(), encoded->size()});
        }
        return checksum.checksum();
    }

    /// Check the  next chunk's address and size, then copy it to its final GPU buffer.
    [[nodiscard]] std::expected<void, gpu_grouped_soa_ingest_error>
    accept_chunk_impl(const index_chunk_address& address,
                      std::span<const std::byte> bytes,
                      std::uint64_t admitted_row_checksum,
                      input_authority authority) noexcept {
        if (failure_) {
            return std::unexpected(gpu_grouped_soa_ingest_error::poisoned_stream);
        }
        if (input_authority_ == input_authority::unset) {
            input_authority_ = authority;
        } else if (input_authority_ != authority) {
            return fail<void>(gpu_grouped_soa_ingest_error::unauthorized_extent);
        }
        if (!resident_ || extent_cursor_ >= plans_.size()) {
            return fail<void>(gpu_grouped_soa_ingest_error::byte_count);
        }
        const extent_plan& plan = plans_[extent_cursor_];
        const index_sub_key_request& request = plan.request;
        if (chunk_cursor_ >= request.expected_chunk_count ||
            address.generation_id != request.generation_id ||
            address.position_segment != request.position_segment ||
            address.sub_key != request.sub_key ||
            address.chunk_no != request.first_chunk_no + chunk_cursor_) {
            return fail<void>(gpu_grouped_soa_ingest_error::unauthorized_extent);
        }
        const std::uint64_t quantum = k_grouped_soa_extent_payload_bytes;
        const std::uint64_t chunk_offset = static_cast<std::uint64_t>(chunk_cursor_) * quantum;
        if (chunk_offset >= request.expected_total_bytes) {
            return fail<void>(gpu_grouped_soa_ingest_error::byte_count);
        }
        const std::uint64_t expected_bytes =
            std::min<std::uint64_t>(quantum, request.expected_total_bytes - chunk_offset);
        bioseq::gpu::vulkan::buffer& destination =
            plan.position ? resident_->positions_ : resident_->controls_;
        const std::uint64_t destination_offset =
            bioseq::common::saturating_add(plan.destination_offset, chunk_offset);
        if (bytes.size() != expected_bytes ||
            destination_offset == (std::numeric_limits<std::uint64_t>::max)() ||
            bytes.size() > destination.bytes ||
            destination_offset > destination.bytes - bytes.size()) {
            return fail<void>(gpu_grouped_soa_ingest_error::byte_count);
        }
        std::memcpy(static_cast<std::byte*>(destination.mapped) + destination_offset,
                    bytes.data(),
                    bytes.size());
        if (authority == input_authority::raw_rehashed) {
            extent_crc_.update(bytes);
            resident_->stats_.raw_rehashed_rows++;
            resident_->stats_.payload_hash_byte_visits += bytes.size() * 2u;
        } else {
            resident_->stats_.validated_receipt_rows++;
        }
        payload_row_crc_.update_little_endian(admitted_row_checksum);
        if (plan.position) {
            resident_->stats_.position_bytes_copied += bytes.size();
        } else {
            resident_->stats_.control_bytes_copied += bytes.size();
        }
        ++chunk_cursor_;
        if (chunk_cursor_ == request.expected_chunk_count) {
            if (authority == input_authority::raw_rehashed &&
                extent_crc_.checksum() != request.expected_checksum) {
                return fail<void>(gpu_grouped_soa_ingest_error::checksum);
            }
            if (plan.position) {
                ++resident_->stats_.position_extents_validated;
            } else {
                ++resident_->stats_.control_extents_validated;
            }
            ++extent_cursor_;
            chunk_cursor_ = 0u;
            extent_crc_ = {};
        }
        return {};
    }
    /// Row request and destination offset for one control or position extent.
    struct extent_plan {
        index_sub_key_request request{};
        std::uint64_t destination_offset = 0u;
        bool position = false;
    };

    /// Take ownership of the allocated target and its ordered receive plan.
    gpu_grouped_soa_generation_ingest(std::shared_ptr<gpu_grouped_soa_resident_target> resident,
                                      std::vector<extent_plan> plans,
                                      std::vector<index_sub_key_request> requests,
                                      std::uint64_t payload_checksum_expected,
                                      std::uint64_t expected_payload_rows) noexcept
        : resident_(std::move(resident)), plans_(std::move(plans)), requests_(std::move(requests)),
          payload_checksum_expected_(payload_checksum_expected),
          expected_payload_rows_(expected_payload_rows) {}

    /// Save an, error that prevents this receiver from publishing a target
    /// @tparam p_value Value type of the failing operation's result.
    template <class p_value>
    [[nodiscard]] std::expected<p_value, gpu_grouped_soa_ingest_error>
    fail(gpu_grouped_soa_ingest_error error) noexcept {
        failure_ = error;
        return std::unexpected(error);
    }

    std::shared_ptr<gpu_grouped_soa_resident_target> resident_{};
    std::vector<extent_plan> plans_{};
    std::vector<index_sub_key_request> requests_{};
    std::size_t extent_cursor_ = 0u;
    std::uint32_t chunk_cursor_ = 0u;
    crc64_ecma182_accumulator extent_crc_{};
    crc64_ecma182_accumulator payload_row_crc_{};
    std::uint64_t payload_checksum_expected_ = 0u;
    std::uint64_t expected_payload_rows_ = 0u;
    std::optional<minimizer_generation_manifest> planned_manifest_{};
    std::uint64_t planned_directory_shape_checksum_ = 0u;
    input_authority input_authority_ = input_authority::unset;
    bool metadata_sealed_ = true;
    std::optional<gpu_grouped_soa_ingest_error> failure_{};
};

} // namespace bioseq::indexing

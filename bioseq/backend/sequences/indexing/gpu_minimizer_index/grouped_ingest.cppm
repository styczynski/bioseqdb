/**
 * @file grouped_ingest.cppm
 * @brief Receipt-authorized grouped-SoA GPU generation ingestion.
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

// Grouped-SoA restart-cold resident ingestion.

[[nodiscard]] inline bool is_gpu_device_uuid_bound(
        const bioseq::gpu::vulkan::gpu_device_uuid& uuid) noexcept {
    return std::any_of(
        uuid.begin(), uuid.end(),
        [](std::uint8_t byte) noexcept { return byte != 0u; });
}

/** Failures exposed by grouped-SoA resident ingestion and publication. */
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
};

static_assert(k_grouped_soa_gpu_abi_revision
    == bioseq::gpu::vulkan::k_gpu_grouped_soa_abi_revision);

/// Immutable identity of one restart-cold grouped-SoA upload. The opaque
/// cold_load_identity is supplied by the PostgreSQL/scheduler owner. It is not
/// derived from a host pointer or from query contents.
struct gpu_grouped_soa_resident_identity {
    minimizer_generation_layout layout =
        minimizer_generation_layout::invalid;
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

    [[nodiscard]] bool operator==(
        const gpu_grouped_soa_resident_identity&) const = default;
};

[[nodiscard]] inline bool is_gpu_grouped_soa_resident_identity_valid(
    const gpu_grouped_soa_resident_identity& identity) noexcept
{
    return identity.layout
            == minimizer_generation_layout::
                grouped_soa_segment_local
        && identity.generation_id != 0u
        && identity.source_identity != 0u
        && identity.cold_load_identity != 0u
        && is_gpu_device_uuid_bound(identity.device_uuid)
        && identity.gpu_abi_revision == k_grouped_soa_gpu_abi_revision;
}

/// Positive and prohibited-work stats owned by a completed cold upload.
/// Counts describe bytes copied directly into their final device buffers. No
/// decoded host index or temporary pair array is part of this representation.
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

    [[nodiscard]] bool has_forbidden_work() const noexcept {
        return forbidden_host_control_decodes != 0u
            || forbidden_host_position_decodes != 0u
            || forbidden_host_position_globalizations != 0u
            || forbidden_host_index_rebuilds != 0u
            || forbidden_cpu_or_simd_fallbacks != 0u;
    }
};

class gpu_grouped_soa_generation_ingest;

/// Cacheable, immutable grouped-SoA target. `controls` retains each persisted
/// SoA extent byte-for-byte and `positions` retains the persisted segment-local
/// scalar lanes byte-for-byte.  The descriptor buffer is the only derived
/// device object and contains metadata/byte offsets, never decoded payload.
class gpu_grouped_soa_resident_target {
private:
    struct construction_token {};

public:
    gpu_grouped_soa_resident_target(
        construction_token,
        bioseq::parallelism::memory_permit permit,
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
        : permit_(std::move(permit)),
          controls_(std::move(controls)),
          positions_(std::move(positions)),
          descriptors_(std::move(descriptors)),
          identity_(identity),
          directory_bytes_(directory_bytes),
          control_bytes_(control_bytes),
          position_bytes_(position_bytes),
          descriptor_bytes_(descriptor_bytes),
          control_count_(control_count),
          position_count_(position_count),
          descriptor_count_(descriptor_count) {}

    [[nodiscard]] const bioseq::gpu::vulkan::buffer& controls() const noexcept {
        return controls_;
    }
    [[nodiscard]] const bioseq::gpu::vulkan::buffer& positions() const noexcept {
        return positions_;
    }
    [[nodiscard]] const bioseq::gpu::vulkan::buffer& descriptors() const noexcept {
        return descriptors_;
    }
    [[nodiscard]] const gpu_grouped_soa_resident_identity& identity() const noexcept {
        return identity_;
    }
    [[nodiscard]] const gpu_grouped_soa_cold_load_stats& cold_load_stats() const noexcept {
        return stats_;
    }
    [[nodiscard]] std::uint64_t admitted_device_bytes() const noexcept {
        return permit_.bytes();
    }
    [[nodiscard]] std::uint64_t directory_bytes() const noexcept {
        return directory_bytes_;
    }
    [[nodiscard]] std::uint64_t control_bytes() const noexcept {
        return control_bytes_;
    }
    [[nodiscard]] std::uint64_t position_bytes() const noexcept {
        return position_bytes_;
    }
    [[nodiscard]] std::uint64_t descriptor_bytes() const noexcept {
        return descriptor_bytes_;
    }
    [[nodiscard]] std::uint64_t control_count() const noexcept {
        return control_count_;
    }
    [[nodiscard]] std::uint64_t position_count() const noexcept {
        return position_count_;
    }
    [[nodiscard]] std::uint32_t descriptor_count() const noexcept {
        return descriptor_count_;
    }

    /// This slice is an index-residency stats, not a scheduler execution stats.
    [[nodiscard]] static constexpr bool
    qualifies_as_all_gpu_execution() noexcept { return false; }
    [[nodiscard]] static constexpr bool
    permits_host_decode_or_rebuild() noexcept { return false; }

    [[nodiscard]] bool validated() const noexcept {
        const std::uint64_t payload_bytes =
            bioseq::common::saturating_add(
                control_bytes_, position_bytes_);
        const std::uint64_t expected_directory_bytes =
            bioseq::common::saturating_mul(
                descriptor_count_,
                grouped_soa_bucket_directory_entry::encoded_bytes);
        const std::uint64_t expected_descriptor_bytes =
            bioseq::common::saturating_mul(
                descriptor_count_,
                sizeof(bioseq::gpu::vulkan::
                    gpu_grouped_soa_resident_partition_descriptor));
        const std::uint64_t allocation_bytes =
            bioseq::common::saturating_add(
                controls_.allocation_bytes,
                bioseq::common::saturating_add(
                    positions_.allocation_bytes,
                    descriptors_.allocation_bytes));
        return is_gpu_grouped_soa_resident_identity_valid(identity_)
            && controls_.is_valid() && positions_.is_valid() && descriptors_.is_valid()
            && payload_bytes
                != (std::numeric_limits<std::uint64_t>::max)()
            && expected_directory_bytes == directory_bytes_
            && expected_descriptor_bytes == descriptor_bytes_
            && identity_.directory_bytes == directory_bytes_
            && identity_.control_bytes == control_bytes_
            && identity_.position_bytes == position_bytes_
            && identity_.descriptor_bytes == descriptor_bytes_
            && identity_.control_count == control_count_
            && identity_.position_count == position_count_
            && identity_.descriptor_count == descriptor_count_
            && control_bytes_ <= controls_.bytes
            && position_bytes_ <= positions_.bytes
            && descriptor_bytes_ <= descriptors_.bytes
            && allocation_bytes
                != (std::numeric_limits<std::uint64_t>::max)()
            && allocation_bytes == permit_.bytes()
            && stats_.cold_cache_misses == 1u
            && stats_.full_generations_ingested == 1u
            && stats_.control_extents_validated == descriptor_count_
            && stats_.position_extents_validated == descriptor_count_
            && stats_.control_bytes_copied == control_bytes_
            && stats_.position_bytes_copied == position_bytes_
            && stats_.descriptor_bytes_copied == descriptor_bytes_
            && stats_.device_allocation_bytes == allocation_bytes
            && stats_.admitted_device_bytes == permit_.bytes()
            && stats_.validated_receipt_rows != 0u
            && stats_.raw_rehashed_rows == 0u
            && stats_.payload_hash_byte_visits == 0u
            && !stats_.has_forbidden_work();
    }

private:
    // First declaration ensures buffers are destroyed before their admission.
    bioseq::parallelism::memory_permit permit_{};
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

/// Direct full-generation receiver for the first restart-cold measurement.
/// Requests follow physical grouped-SoA order: every control extent,
/// then every position extent. Chunks are copied once into final resident
/// buffers. PostgreSQL's validated-row receipts supply already-checked row
/// checksums, avoiding a second payload-byte hash pass. The raw entry point is
/// deliberately test-only and cannot publish a resident.
class gpu_grouped_soa_generation_ingest {
public:
    gpu_grouped_soa_generation_ingest(
        const gpu_grouped_soa_generation_ingest&) = delete;
    gpu_grouped_soa_generation_ingest& operator=(
        const gpu_grouped_soa_generation_ingest&) = delete;
    gpu_grouped_soa_generation_ingest(
        gpu_grouped_soa_generation_ingest&&) noexcept = default;
    gpu_grouped_soa_generation_ingest& operator=(
        gpu_grouped_soa_generation_ingest&&) = delete;

    template <class p_directory_view>
    [[nodiscard]] static std::expected<
        gpu_grouped_soa_generation_ingest,
        gpu_grouped_soa_ingest_error>
    begin(
        bioseq::gpu::vulkan::runtime& runtime,
        const minimizer_generation_head& head,
        const minimizer_generation_manifest& manifest,
        const expected_minimizer_generation_identity& expected,
        const p_directory_view& directory,
        std::uint64_t cold_load_identity)
    {
        if constexpr (std::endian::native != std::endian::little) {
            return std::unexpected(
                gpu_grouped_soa_ingest_error::unsupported_endianness);
        }
        if (!runtime.is_usable()
            || !is_gpu_device_uuid_bound(runtime.features().device_uuid)
            || cold_load_identity == 0u) {
            return std::unexpected(
                gpu_grouped_soa_ingest_error::invalid_identity);
        }
        const auto manifest_profile =
            derive_grouped_soa_minimizer_profile(manifest);
        const bool compatible = manifest_profile
            && manifest.layout
                == minimizer_generation_layout::
                    grouped_soa_segment_local
            && manifest.gpu_abi_revision == k_grouped_soa_gpu_abi_revision
            && directory.profile().layout
                == minimizer_generation_layout::
                    grouped_soa_segment_local
            && directory.profile().gpu_abi_revision
                == k_grouped_soa_gpu_abi_revision
            && directory.profile() == *manifest_profile;
        if (!compatible
            || !validate_minimizer_generation_storage_contract(
                manifest, expected)
            || !validate_minimizer_generation_metadata(
                head, manifest, directory)) {
            return std::unexpected(
                gpu_grouped_soa_ingest_error::metadata_mismatch);
        }
        const auto entries = directory.entries();
        if (entries.size()
                > (std::numeric_limits<std::uint32_t>::max)()
            || entries.size()
                > (std::numeric_limits<std::size_t>::max)() / 2u) {
            return std::unexpected(
                gpu_grouped_soa_ingest_error::arithmetic_overflow);
        }

        std::vector<std::uint64_t> control_offsets(entries.size());
        std::vector<std::uint64_t> position_offsets(entries.size());
        std::vector<bioseq::gpu::vulkan::
            gpu_grouped_soa_resident_partition_descriptor> descriptors;
        descriptors.reserve(entries.size());
        std::uint64_t control_bytes = 0u;
        std::uint64_t position_bytes = 0u;
        std::uint64_t control_count = 0u;
        std::uint64_t position_count = 0u;
        for (std::size_t ordinal = 0u; ordinal < entries.size(); ++ordinal) {
            const grouped_soa_bucket_directory_entry& entry = entries[ordinal];
            if (validate_minimizer_bucket_dir_entry(
                    entry, directory.profile()).has_value()
                || entry.partition_id != ordinal
                || entry.control_element_count
                    > (std::numeric_limits<std::uint32_t>::max)()
                || entry.position_element_count
                    > (std::numeric_limits<std::uint32_t>::max)()) {
                return std::unexpected(
                    gpu_grouped_soa_ingest_error::partition_mismatch);
            }
            const auto core = derive_minimizer_position_segment_bounds(
                manifest, static_cast<std::uint32_t>(entry.position_segment));
            const auto layout = plan_grouped_soa_control_plane(
                entry.control_element_count,
                manifest.hash_width,
                entry.shape_offset_width,
                manifest.count_width);
            if (!core || !layout
                || control_bytes
                    % static_cast<std::uint8_t>(
                        manifest.hash_width) != 0u
                || position_bytes
                    % static_cast<std::uint8_t>(
                        manifest.position_width) != 0u
                || control_bytes
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - entry.control_encoded_bytes
                || position_bytes
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - entry.position_encoded_bytes
                || control_count
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - entry.control_element_count
                || position_count
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - entry.position_element_count) {
                return std::unexpected(
                    gpu_grouped_soa_ingest_error::arithmetic_overflow);
            }
            control_offsets[ordinal] = control_bytes;
            position_offsets[ordinal] = position_bytes;
            descriptors.push_back(
                bioseq::gpu::vulkan::gpu_grouped_soa_resident_partition_descriptor{
                    .partition_id = entry.partition_id,
                    .directory_ordinal = static_cast<std::uint32_t>(ordinal),
                    .position_segment = static_cast<std::uint32_t>(
                        entry.position_segment),
                    .flags = bioseq::gpu::vulkan::
                        k_gpu_grouped_soa_partition_required_flags,
                    .hash_byte_offset = control_bytes + layout->hash_offset,
                    .prefix_byte_offset =
                        control_bytes + layout->prefix_offset,
                    .count_byte_offset = control_bytes + layout->count_offset,
                    .position_byte_offset = position_bytes,
                    .control_count = static_cast<std::uint32_t>(
                        entry.control_element_count),
                    .position_count = static_cast<std::uint32_t>(
                        entry.position_element_count),
                    .hash_width = static_cast<std::uint32_t>(
                        manifest.hash_width),
                    .offset_width = static_cast<std::uint32_t>(
                        entry.shape_offset_width),
                    .count_width = static_cast<std::uint32_t>(
                        manifest.count_width),
                    .position_width = static_cast<std::uint32_t>(
                        manifest.position_width),
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
            bioseq::common::saturating_add(
                control_bytes, position_bytes);
        const std::uint64_t descriptor_bytes =
            bioseq::common::saturating_mul(
                descriptors.size(),
                sizeof(bioseq::gpu::vulkan::
                    gpu_grouped_soa_resident_partition_descriptor));
        if (payload_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || descriptor_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || payload_bytes != manifest.payload_bytes
            || control_count != manifest.control_count
            || position_count != manifest.position_count
            || directory.encoded_bytes().size() != manifest.directory_bytes
            || manifest.directory_bytes
                != static_cast<std::uint64_t>(entries.size())
                    * grouped_soa_bucket_directory_entry::encoded_bytes) {
            return std::unexpected(
                gpu_grouped_soa_ingest_error::metadata_mismatch);
        }

        const std::uint64_t control_request =
            std::max<std::uint64_t>(control_bytes, 16u);
        const std::uint64_t position_request =
            std::max<std::uint64_t>(position_bytes, 16u);
        const std::uint64_t descriptor_request =
            std::max<std::uint64_t>(descriptor_bytes, 16u);
        if (control_request > runtime.features().max_storage_buffer_range
            || position_request > runtime.features().max_storage_buffer_range
            || descriptor_request > runtime.features().max_storage_buffer_range) {
            return std::unexpected(
                gpu_grouped_soa_ingest_error::arithmetic_overflow);
        }
        constexpr VkBufferUsageFlags usage =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        const std::uint64_t control_admission =
            runtime.buffer_allocation_requirement(control_request, usage);
        const std::uint64_t position_admission =
            runtime.buffer_allocation_requirement(position_request, usage);
        const std::uint64_t descriptor_admission =
            runtime.buffer_allocation_requirement(descriptor_request, usage);
        const std::uint64_t admitted_bytes =
            bioseq::common::saturating_add(
                control_admission,
                bioseq::common::saturating_add(
                    position_admission, descriptor_admission));
        if (admitted_bytes
                == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(
                gpu_grouped_soa_ingest_error::arithmetic_overflow);
        }
        auto permit = bioseq::parallelism::memory_admission_controller::instance()
            .try_reserve(admitted_bytes);
        if (!permit) {
            return std::unexpected(
                gpu_grouped_soa_ingest_error::admission);
        }
        auto control_buffer = runtime.create_device_buffer(
            control_request, usage);
        auto position_buffer = runtime.create_device_buffer(
            position_request, usage);
        auto descriptor_buffer = runtime.create_device_buffer(
            descriptor_request, usage);
        const std::uint64_t allocation_bytes =
            bioseq::common::saturating_add(
                control_buffer.allocation_bytes,
                bioseq::common::saturating_add(
                    position_buffer.allocation_bytes,
                    descriptor_buffer.allocation_bytes));
        if (allocation_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || allocation_bytes > admitted_bytes) {
            return std::unexpected(
                gpu_grouped_soa_ingest_error::admission);
        }
        if (control_buffer.mapped == nullptr
            || position_buffer.mapped == nullptr
            || descriptor_buffer.mapped == nullptr) {
            return std::unexpected(
                gpu_grouped_soa_ingest_error::non_mappable_device_memory);
        }
        permit->shrink_to(allocation_bytes);
        std::memset(control_buffer.mapped, 0,
            static_cast<std::size_t>(control_buffer.bytes));
        std::memset(position_buffer.mapped, 0,
            static_cast<std::size_t>(position_buffer.bytes));
        std::memset(descriptor_buffer.mapped, 0,
            static_cast<std::size_t>(descriptor_buffer.bytes));
        if (descriptor_bytes != 0u) {
            std::memcpy(descriptor_buffer.mapped, descriptors.data(),
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
            std::move(*permit), std::move(control_buffer),
            std::move(position_buffer), std::move(descriptor_buffer),
            identity, manifest.directory_bytes, control_bytes,
            position_bytes, descriptor_bytes, control_count,
            position_count, static_cast<std::uint32_t>(entries.size()));
        resident->stats_.descriptor_bytes_copied = descriptor_bytes;

        std::vector<extent_plan> plans;
        std::vector<index_sub_key_request> requests;
        plans.reserve(entries.size() * 2u);
        requests.reserve(entries.size() * 2u);
        const auto append_plan = [entries, &manifest, &control_offsets,
                                  &position_offsets](
                                     std::size_t ordinal, bool position,
                                     auto& plan_out, auto& request_out)
                -> bool {
            const grouped_soa_bucket_directory_entry& entry = entries[ordinal];
            const auto position_sub_key =
                derive_minimizer_position_extent_sub_key(entry.sub_key);
            if (!position_sub_key) return false;
            const index_sub_key_request request{
                .generation_id = manifest.generation_id,
                .position_segment = entry.position_segment,
                .sub_key = position ? *position_sub_key : entry.sub_key,
                .first_chunk_no = position
                    ? entry.position_first_chunk : entry.control_first_chunk,
                .expected_chunk_count = position
                    ? entry.position_chunk_count : entry.control_chunk_count,
                .expected_total_bytes = position
                    ? entry.position_encoded_bytes : entry.control_encoded_bytes,
                .expected_checksum = position
                    ? entry.position_checksum : entry.control_checksum,
            };
            plan_out.push_back(extent_plan{
                .request = request,
                .destination_offset = position
                    ? position_offsets[ordinal] : control_offsets[ordinal],
                .position = position,
            });
            request_out.push_back(request);
            return true;
        };
        for (std::size_t ordinal = 0u; ordinal < entries.size(); ++ordinal) {
            if (!append_plan(ordinal, false, plans, requests)) {
                return std::unexpected(
                    gpu_grouped_soa_ingest_error::partition_mismatch);
            }
        }
        for (std::size_t ordinal = 0u; ordinal < entries.size(); ++ordinal) {
            if (!append_plan(ordinal, true, plans, requests)) {
                return std::unexpected(
                    gpu_grouped_soa_ingest_error::partition_mismatch);
            }
        }
        std::uint64_t expected_payload_rows = 0u;
        for (const index_sub_key_request& request : requests) {
            if (expected_payload_rows
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - request.expected_chunk_count) {
                return std::unexpected(
                    gpu_grouped_soa_ingest_error::arithmetic_overflow);
            }
            expected_payload_rows += request.expected_chunk_count;
        }
        return gpu_grouped_soa_generation_ingest{
            std::move(resident), std::move(plans), std::move(requests),
            manifest.payload_checksum, expected_payload_rows};
    }

    [[nodiscard]] std::span<const index_sub_key_request> requests() const noexcept {
        return requests_;
    }

    [[nodiscard]] std::expected<void, gpu_grouped_soa_ingest_error>
    accept_validated_row(
        const validated_minimizer_generation_row_receipt& receipt) noexcept
    {
        std::expected<void, gpu_grouped_soa_ingest_error> accepted{};
        const bool visited = visit_validated_minimizer_generation_row(
            receipt,
            [this, &accepted](
                const index_chunk_address& address,
                std::span<const std::byte> bytes,
                std::uint64_t row_checksum) noexcept {
                accepted = accept_chunk_impl(
                    address, bytes, row_checksum,
                    input_authority::validated_receipt);
            });
        if (!visited) {
            return fail<void>(
                gpu_grouped_soa_ingest_error::unauthorized_extent);
        }
        return accepted;
    }

    /// Standalone corruption-test path. Production full-generation loading
    /// must pass validated_minimizer_generation_row_receipt instead.
    [[nodiscard]] std::expected<void, gpu_grouped_soa_ingest_error>
    accept_raw_chunk_for_test(
        const index_chunk_address& address,
        std::span<const std::byte> bytes) noexcept
    {
        if (bytes.size() > (std::numeric_limits<std::uint32_t>::max)()) {
            return fail<void>(gpu_grouped_soa_ingest_error::byte_count);
        }
        const std::uint64_t row_checksum = crc64_index_chunk_row(
            address.generation_id, address.position_segment,
            address.sub_key, address.chunk_no,
            static_cast<std::uint32_t>(bytes.size()), bytes);
        return accept_chunk_impl(
            address, bytes, row_checksum, input_authority::raw_rehashed);
    }

    [[nodiscard]] std::expected<void, gpu_grouped_soa_ingest_error>
    accept_raw_chunk_for_test(
        const index_chunk_address& address,
        const char* payload,
        std::size_t payload_bytes) noexcept
    {
        if (payload == nullptr && payload_bytes != 0u) {
            return fail<void>(gpu_grouped_soa_ingest_error::byte_count);
        }
        return accept_raw_chunk_for_test(address, std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(payload), payload_bytes});
    }

    [[nodiscard]] std::expected<
        std::shared_ptr<const gpu_grouped_soa_resident_target>,
        gpu_grouped_soa_ingest_error>
    finish()
    {
        if (failure_) return std::unexpected(*failure_);
        if (!resident_ || extent_cursor_ != plans_.size()
            || chunk_cursor_ != 0u) {
            return fail<std::shared_ptr<const gpu_grouped_soa_resident_target>>(
                gpu_grouped_soa_ingest_error::byte_count);
        }
        // Only the receipt path is publishable. It proves that PostgreSQL's
        // single scan already checked each row while the aggregate below binds
        // the exact ordered checksum sequence to this manifest.
        if (input_authority_ != input_authority::validated_receipt
            || resident_->stats_.validated_receipt_rows
                != expected_payload_rows_
            || resident_->stats_.raw_rehashed_rows != 0u
            || resident_->stats_.payload_hash_byte_visits != 0u) {
            return fail<std::shared_ptr<const gpu_grouped_soa_resident_target>>(
                gpu_grouped_soa_ingest_error::completion_mismatch);
        }
        if (payload_row_crc_.checksum() != payload_checksum_expected_) {
            return fail<std::shared_ptr<const gpu_grouped_soa_resident_target>>(
                gpu_grouped_soa_ingest_error::checksum);
        }
        resident_->stats_.cold_cache_misses = 1u;
        resident_->stats_.full_generations_ingested = 1u;
        resident_->stats_.device_allocation_bytes =
            resident_->controls_.allocation_bytes
            + resident_->positions_.allocation_bytes
            + resident_->descriptors_.allocation_bytes;
        resident_->stats_.admitted_device_bytes = resident_->permit_.bytes();
        if (!resident_->validated()) {
            return fail<std::shared_ptr<const gpu_grouped_soa_resident_target>>(
                gpu_grouped_soa_ingest_error::completion_mismatch);
        }
        std::shared_ptr<const gpu_grouped_soa_resident_target> published = resident_;
        resident_.reset();
        return published;
    }

private:
    enum class input_authority : std::uint8_t {
        unset,
        validated_receipt,
        raw_rehashed,
    };

    [[nodiscard]] std::expected<void, gpu_grouped_soa_ingest_error>
    accept_chunk_impl(
        const index_chunk_address& address,
        std::span<const std::byte> bytes,
        std::uint64_t admitted_row_checksum,
        input_authority authority) noexcept
    {
        if (failure_) {
            return std::unexpected(
                gpu_grouped_soa_ingest_error::poisoned_stream);
        }
        if (input_authority_ == input_authority::unset) {
            input_authority_ = authority;
        } else if (input_authority_ != authority) {
            return fail<void>(
                gpu_grouped_soa_ingest_error::unauthorized_extent);
        }
        if (!resident_ || extent_cursor_ >= plans_.size()) {
            return fail<void>(gpu_grouped_soa_ingest_error::byte_count);
        }
        const extent_plan& plan = plans_[extent_cursor_];
        const index_sub_key_request& request = plan.request;
        if (chunk_cursor_ >= request.expected_chunk_count
            || address.generation_id != request.generation_id
            || address.position_segment != request.position_segment
            || address.sub_key != request.sub_key
            || address.chunk_no != request.first_chunk_no + chunk_cursor_) {
            return fail<void>(
                gpu_grouped_soa_ingest_error::unauthorized_extent);
        }
        const std::uint64_t quantum = k_grouped_soa_extent_payload_bytes;
        const std::uint64_t chunk_offset =
            static_cast<std::uint64_t>(chunk_cursor_) * quantum;
        if (chunk_offset >= request.expected_total_bytes) {
            return fail<void>(gpu_grouped_soa_ingest_error::byte_count);
        }
        const std::uint64_t expected_bytes = std::min<std::uint64_t>(
            quantum, request.expected_total_bytes - chunk_offset);
        bioseq::gpu::vulkan::buffer& destination = plan.position
            ? resident_->positions_ : resident_->controls_;
        const std::uint64_t destination_offset =
            bioseq::common::saturating_add(
                plan.destination_offset, chunk_offset);
        if (bytes.size() != expected_bytes
            || destination_offset
                == (std::numeric_limits<std::uint64_t>::max)()
            || bytes.size() > destination.bytes
            || destination_offset
                > destination.bytes - bytes.size()) {
            return fail<void>(gpu_grouped_soa_ingest_error::byte_count);
        }
        std::memcpy(
            static_cast<std::byte*>(destination.mapped)
                + destination_offset,
            bytes.data(), bytes.size());
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
            if (authority == input_authority::raw_rehashed
                && extent_crc_.checksum() != request.expected_checksum) {
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
    struct extent_plan {
        index_sub_key_request request{};
        std::uint64_t destination_offset = 0u;
        bool position = false;
    };

    gpu_grouped_soa_generation_ingest(
        std::shared_ptr<gpu_grouped_soa_resident_target> resident,
        std::vector<extent_plan> plans,
        std::vector<index_sub_key_request> requests,
        std::uint64_t payload_checksum_expected,
        std::uint64_t expected_payload_rows) noexcept
        : resident_(std::move(resident)), plans_(std::move(plans)),
          requests_(std::move(requests)),
          payload_checksum_expected_(payload_checksum_expected),
          expected_payload_rows_(expected_payload_rows) {}

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
    input_authority input_authority_ = input_authority::unset;
    std::optional<gpu_grouped_soa_ingest_error> failure_{};
};


} // namespace bioseq::indexing
